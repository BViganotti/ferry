#include "ferry/server.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/stat.h>
#include <unistd.h>

#include "codec.hpp"
#include "compression.hpp"
#include "ferry/event_loop.hpp"
#include "ferry/net.hpp"
#include "ferry/url.hpp"
#include "router.hpp"

namespace ferry {

namespace {

std::uint16_t socket_port(int fd) {
  sockaddr_storage storage{};
  socklen_t length = sizeof storage;
  if (::getsockname(fd, reinterpret_cast<sockaddr*>(&storage), &length) != 0) return 0;
  if (storage.ss_family == AF_INET) {
    return ntohs(reinterpret_cast<sockaddr_in*>(&storage)->sin_port);
  }
  if (storage.ss_family == AF_INET6) {
    return ntohs(reinterpret_cast<sockaddr_in6*>(&storage)->sin6_port);
  }
  return 0;
}

Address socket_address(int fd) {
  Address address;
  address.length = sizeof address.storage;
  if (::getsockname(fd, reinterpret_cast<sockaddr*>(&address.storage), &address.length) != 0) {
    address.length = 0;
  }
  return address;
}

std::string http_date(std::time_t when) {
  static const char* weekdays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  static const char* months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                 "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  std::tm tm{};
  ::gmtime_r(&when, &tm);
  char buffer[64];
  std::snprintf(buffer, sizeof buffer, "%s, %02d %s %04d %02d:%02d:%02d GMT",
                weekdays[tm.tm_wday], tm.tm_mday, months[tm.tm_mon], tm.tm_year + 1900,
                tm.tm_hour, tm.tm_min, tm.tm_sec);
  return buffer;
}

std::string http_date_now() {
  // Date changes at most once per second; formatting it per response is pure
  // overhead. A per-loop-thread cache removes gmtime/snprintf from the hot path.
  thread_local std::time_t cached_second = 0;
  thread_local std::string cached_date;
  std::time_t now = std::time(nullptr);
  if (now != cached_second) {
    cached_second = now;
    cached_date = http_date(now);
  }
  return cached_date;
}

void log_warning(const std::string& message) {
  std::fprintf(stderr, "ferry: %s\n", message.c_str());
}

std::string_view trim_ows(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
  return text;
}

bool accepts_gzip(std::string_view header) {
  while (!header.empty()) {
    auto comma = header.find(',');
    auto item = trim_ows(comma == std::string_view::npos ? header : header.substr(0, comma));
    auto semicolon = item.find(';');
    auto coding = trim_ows(item.substr(0, semicolon));
    if (Headers::equals(coding, "gzip")) {
      if (semicolon == std::string_view::npos) return true;
      auto parameters = item.substr(semicolon + 1);
      auto q = parameters.find("q=");
      if (q == std::string_view::npos) return true;
      auto value = trim_ows(parameters.substr(q + 2));
      return !value.starts_with("0");
    }
    if (comma == std::string_view::npos) break;
    header.remove_prefix(comma + 1);
  }
  return false;
}

std::string join_allowed(const std::vector<std::string>& allowed) {
  std::string result;
  for (const auto& item : allowed) {
    if (!result.empty()) result += ", ";
    result += item;
  }
  return result;
}

bool no_body_status(int status) {
  return status == 204 || status == 304 || (status >= 100 && status < 200);
}

std::string mime_type(const std::string& path) {
  auto dot = path.rfind('.');
  if (dot == std::string::npos) return "application/octet-stream";
  std::string extension = path.substr(dot + 1);
  std::transform(extension.begin(), extension.end(), extension.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (extension == "html" || extension == "htm") return "text/html; charset=utf-8";
  if (extension == "css") return "text/css; charset=utf-8";
  if (extension == "js" || extension == "mjs") return "text/javascript; charset=utf-8";
  if (extension == "json") return "application/json";
  if (extension == "txt" || extension == "md") return "text/plain; charset=utf-8";
  if (extension == "xml") return "application/xml";
  if (extension == "svg") return "image/svg+xml";
  if (extension == "png") return "image/png";
  if (extension == "jpg" || extension == "jpeg") return "image/jpeg";
  if (extension == "gif") return "image/gif";
  if (extension == "webp") return "image/webp";
  if (extension == "ico") return "image/x-icon";
  if (extension == "woff") return "font/woff";
  if (extension == "woff2") return "font/woff2";
  if (extension == "pdf") return "application/pdf";
  if (extension == "wasm") return "application/wasm";
  if (extension == "mp4") return "video/mp4";
  return "application/octet-stream";
}

std::string etag_for(std::int64_t mtime, std::uint64_t size) {
  char buffer[64];
  std::snprintf(buffer, sizeof buffer, "\"%llx-%llx\"",
                static_cast<unsigned long long>(mtime),
                static_cast<unsigned long long>(size));
  return buffer;
}

bool etag_matches(std::string_view header, std::string_view etag) {
  while (!header.empty()) {
    auto comma = header.find(',');
    auto item = trim_ows(comma == std::string_view::npos ? header : header.substr(0, comma));
    if (item == "*" || Headers::equals(item, etag)) return true;
    if (comma == std::string_view::npos) break;
    header.remove_prefix(comma + 1);
  }
  return false;
}

enum class RangeResult { Ignore, Satisfiable, Unsatisfiable };

struct ByteRange {
  std::uint64_t start = 0;
  std::uint64_t end = 0;
};

bool parse_number(std::string_view text, std::uint64_t& value) {
  if (text.empty()) return false;
  std::uint64_t result = 0;
  for (char c : text) {
    if (c < '0' || c > '9') return false;
    if (result > (UINT64_MAX - static_cast<std::uint64_t>(c - '0')) / 10) return false;
    result = result * 10 + static_cast<std::uint64_t>(c - '0');
  }
  value = result;
  return true;
}

struct RangeParse {
  RangeResult status = RangeResult::Ignore;
  std::vector<ByteRange> ranges;
};

RangeParse parse_ranges(std::string_view header, std::uint64_t size) {
  RangeParse result;
  header = trim_ows(header);
  if (!header.starts_with("bytes=")) return result;
  header.remove_prefix(6);

  bool syntactic = false;
  bool unsatisfiable = false;
  while (!header.empty()) {
    auto comma = header.find(',');
    auto spec = trim_ows(comma == std::string_view::npos ? header : header.substr(0, comma));
    if (comma == std::string_view::npos) header = {};
    else header.remove_prefix(comma + 1);
    if (spec.empty()) continue;
    auto dash = spec.find('-');
    if (dash == std::string_view::npos) return result;
    auto left = trim_ows(spec.substr(0, dash));
    auto right = trim_ows(spec.substr(dash + 1));
    syntactic = true;

    if (left.empty()) {
      std::uint64_t suffix = 0;
      if (!parse_number(right, suffix) || suffix == 0) return RangeParse{};
      if (size == 0) {
        unsatisfiable = true;
        continue;
      }
      ByteRange range;
      range.start = suffix >= size ? 0 : size - suffix;
      range.end = size - 1;
      result.ranges.push_back(range);
    } else {
      std::uint64_t first = 0;
      if (!parse_number(left, first)) return RangeParse{};
      if (first >= size) {
        unsatisfiable = true;
        continue;
      }
      ByteRange range;
      range.start = first;
      if (right.empty()) {
        range.end = size - 1;
      } else {
        std::uint64_t last = 0;
        if (!parse_number(right, last)) return RangeParse{};
        if (last < first) {
          unsatisfiable = true;
          continue;
        }
        range.end = std::min(last, size - 1);
      }
      result.ranges.push_back(range);
    }
    if (result.ranges.size() > 32) return RangeParse{};
  }

  if (!result.ranges.empty()) {
    result.status = RangeResult::Satisfiable;
  } else if (syntactic && unsatisfiable) {
    result.status = RangeResult::Unsatisfiable;
  }
  return result;
}

std::time_t parse_http_date_value(std::string_view text) {
  std::string copy(text);
  char weekday[32]{};
  char month[8]{};
  int day = 0;
  int year = 0;
  int hour = 0;
  int minute = 0;
  int second = 0;
  if (std::sscanf(copy.c_str(), "%31[^,], %d %7s %d %d:%d:%d", weekday, &day, month, &year, &hour,
                  &minute, &second) != 7) {
    return 0;
  }
  static const char* months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                 "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  int month_index = -1;
  for (int i = 0; i < 12; ++i) {
    if (std::strcmp(month, months[i]) == 0) {
      month_index = i;
      break;
    }
  }
  if (month_index < 0) return 0;
  std::tm tm{};
  tm.tm_mday = day;
  tm.tm_mon = month_index;
  tm.tm_year = year - 1900;
  tm.tm_hour = hour;
  tm.tm_min = minute;
  tm.tm_sec = second;
  return ::timegm(&tm);
}

struct MultipartRangeState {
  std::string path;
  std::string content_type;
  std::string boundary;
  std::uint64_t total = 0;
  std::vector<ByteRange> ranges;
  std::size_t index = 0;
  std::uint64_t offset = 0;
  int fd = -1;
  bool need_header = true;
  bool finished = false;

  ~MultipartRangeState() {
    if (fd >= 0) ::close(fd);
  }
};

Body::ChunkProducer make_multipart_range_producer(std::string path, std::string content_type,
                                                  std::string boundary, std::uint64_t total,
                                                  std::vector<ByteRange> ranges) {
  auto state = std::make_shared<MultipartRangeState>();
  state->path = std::move(path);
  state->content_type = std::move(content_type);
  state->boundary = std::move(boundary);
  state->total = total;
  state->ranges = std::move(ranges);

  return [state](std::string& chunk) -> task<bool> {
    if (state->finished) co_return false;

    if (state->index >= state->ranges.size()) {
      chunk = std::string(state->index == 0 ? "" : "\r\n") + "--" + state->boundary + "--\r\n";
      state->finished = true;
      co_return true;
    }

    auto& range = state->ranges[state->index];
    if (state->need_header) {
      std::string header;
      if (state->index != 0) header += "\r\n";
      header += "--" + state->boundary + "\r\n";
      header += "content-type: " + state->content_type + "\r\n";
      header += "content-range: bytes " + std::to_string(range.start) + "-" +
                std::to_string(range.end) + "/" + std::to_string(state->total) + "\r\n\r\n";
      chunk = std::move(header);
      state->need_header = false;
      state->offset = range.start;
      if (state->fd < 0) {
        state->fd = ::open(state->path.c_str(), O_RDONLY
#if defined(O_CLOEXEC)
                                                    | O_CLOEXEC
#endif
#if defined(O_NOFOLLOW)
                                                    | O_NOFOLLOW
#endif
        );
        if (state->fd < 0) {
          state->finished = true;
          co_return false;
        }
      }
      co_return true;
    }

    if (state->offset <= range.end) {
      std::size_t wanted = static_cast<std::size_t>(
          std::min<std::uint64_t>(64 * 1024, range.end - state->offset + 1));
      std::vector<char> buffer(wanted);
      ssize_t n = 0;
      do {
        n = ::pread(state->fd, buffer.data(), wanted, static_cast<off_t>(state->offset));
      } while (n < 0 && errno == EINTR);
      if (n <= 0) {
        state->finished = true;
        co_return false;
      }
      chunk.assign(buffer.data(), static_cast<std::size_t>(n));
      state->offset += static_cast<std::uint64_t>(n);
      co_return true;
    }

    ++state->index;
    state->need_header = true;
    co_return true;
  };
}

}  // namespace

struct Server::Impl {
  struct Worker;
  struct Connection;

  struct Connection {
    std::uint64_t id = 0;
    Worker* worker = nullptr;
    Socket socket;
    std::unique_ptr<Stream> stream;
    Address peer;
    Address local;
    std::string buffer;
    std::string peer_text;
    std::string local_text;
    bool busy = false;
    bool secure = false;
    std::string tls_version;

    int fd() const noexcept { return stream ? stream->fd() : socket.fd(); }
  };

  struct Worker {
    explicit Worker(Impl* owner) : impl(owner) {}
    Impl* impl;
    EventLoop loop;
    Socket listener;
    std::thread thread;
    std::unordered_map<std::uint64_t, std::shared_ptr<Connection>> connections;
    bool draining = false;
    std::uint64_t next_id = 1;
  };

  explicit Impl(ServerConfig cfg) : config(std::move(cfg)) {}

  ServerConfig config;
  detail::Router router;
  std::vector<Middleware> middleware;
  Handler not_found;
  ErrorHandler error_handler;
  std::shared_ptr<TlsContext> tls_context;
  std::vector<std::unique_ptr<Worker>> workers;

  std::uint16_t port = 0;
  std::atomic<bool> stopping{false};
  std::atomic<bool> running{false};
  std::atomic<std::size_t> active_connections{0};
  std::atomic<std::size_t> next_worker{0};

  std::mutex wait_mutex;
  std::condition_variable wait_cv;
  std::mutex stop_mutex;

  void require_configurable() const {
    if (running.load()) {
      throw std::logic_error("ferry: routes and middleware must be configured before start()");
    }
  }

  detail::Limits limits() const {
    detail::Limits value;
    value.max_header_bytes = config.max_header_bytes;
    value.max_body_bytes = config.max_body_bytes;
    value.max_headers = config.max_headers;
    return value;
  }

  Response make_error(const Request& request, int status, std::string_view message) {
    if (error_handler) {
      try {
        Response response = error_handler(request, status, message);
        if (response.status == 200) response.status = status;
        if (response.reason.empty()) response.reason = std::string(reason_phrase(response.status));
        return response;
      } catch (const std::exception& e) {
        log_warning(std::string("error handler threw: ") + e.what());
      }
    }
    Response response(status, std::string(message) + "\n", "text/plain; charset=utf-8");
    return response;
  }

  task<bool> send_error(Stream& stream, int status, std::string_view message,
                        std::string_view version, Deadline deadline) {
    std::string body = std::to_string(status) + " " + std::string(reason_phrase(status)) + "\n" +
                       std::string(message) + "\n";
    std::string protocol = version.empty() ? std::string("HTTP/1.1") : std::string(version);
    std::string head;
    head += protocol;
    head += ' ';
    head += std::to_string(status);
    head += ' ';
    head += reason_phrase(status);
    head += "\r\nconnection: close\r\ncontent-type: text/plain; charset=utf-8\r\n";
    head += "content-length: " + std::to_string(body.size()) + "\r\n";
    head += "date: " + http_date_now() + "\r\n";
    if (!config.server_name.empty()) head += "server: " + config.server_name + "\r\n";
    head += "\r\n";

    auto write = co_await write_all(stream, head, deadline);
    if (!write.ok()) co_return false;
    write = co_await write_all(stream, body, deadline);
    co_return write.ok();
  }

  // A single acceptor feeds every worker loop. This is more predictable than
  // SO_REUSEPORT distribution and scales linearly with worker threads.
  task<void> accept_loop(Worker& acceptor) {
    for (;;) {
      if (stopping.load(std::memory_order_relaxed)) co_return;
      auto ready =
          co_await wait_readable(acceptor.loop, acceptor.listener.fd(), Deadline::infinite());
      if (!ready.ok()) co_return;

      for (;;) {
        Address peer;
        auto accepted = acceptor.listener.accept(&peer);
        if (!accepted) break;
        if (stopping.load(std::memory_order_relaxed)) {
          accepted->close();
          co_return;
        }
        if (config.max_connections != 0 &&
            active_connections.load(std::memory_order_relaxed) >= config.max_connections) {
          accepted->close();
          continue;
        }
        if (config.tcp_nodelay) accepted->set_nodelay();

        auto shared_socket = std::make_shared<Socket>(std::move(*accepted));
        std::size_t index = next_worker.fetch_add(1, std::memory_order_relaxed) % workers.size();
        Worker* target = workers[index].get();

        auto install = [this, target, peer, shared_socket] {
          if (stopping.load(std::memory_order_relaxed)) return;
          auto connection = std::make_shared<Connection>();
          connection->id = target->next_id++;
          connection->worker = target;
          connection->socket = std::move(*shared_socket);
          connection->peer = peer;
          connection->local = socket_address(connection->socket.fd());
          connection->peer_text = connection->peer.to_string();
          connection->local_text = connection->local.to_string();
          target->connections[connection->id] = connection;
          active_connections.fetch_add(1, std::memory_order_relaxed);
          spawn(handle_connection(*target, std::move(connection)));
        };

        if (target == &acceptor) {
          install();
        } else {
          target->loop.post(std::move(install));
        }
      }
    }
  }

  struct ConnectionGuard {
    Impl* impl;
    Worker* worker;
    std::shared_ptr<Connection> connection;
    ~ConnectionGuard() {
      connection->stream.reset();
      connection->socket.close();
      worker->connections.erase(connection->id);
      impl->active_connections.fetch_sub(1, std::memory_order_relaxed);
      if (worker->draining && worker->connections.empty()) worker->loop.stop();
    }
  };

  task<void> handle_connection(Worker& worker, std::shared_ptr<Connection> connection) {
    ConnectionGuard guard{this, &worker, connection};
    bool first_request = true;

    try {
      if (tls_context) {
        auto deadline = Deadline::after(config.header_timeout);
        auto result = co_await TlsStream::accept(std::move(connection->socket), tls_context, deadline);
        if (!result.ok()) {
          log_warning("TLS handshake failed: " + result.error);
          co_return;
        }
        connection->stream = std::move(result.stream);
        connection->secure = true;
        connection->tls_version = connection->stream->tls_version();
      } else {
        connection->stream = std::make_unique<PlainStream>(std::move(connection->socket));
      }

      for (;;) {
        if (stopping.load(std::memory_order_relaxed)) break;

        connection->busy = false;
        auto head_deadline =
            Deadline::after(first_request ? config.header_timeout : config.idle_timeout);
        auto head = co_await detail::read_head(*connection->stream, connection->buffer,
                                               config.max_header_bytes, head_deadline);
        if (head.status == detail::ReadStatus::Eof) break;
        if (head.status == detail::ReadStatus::Cancelled) break;
        if (head.status == detail::ReadStatus::Timeout) {
          if (first_request) {
            co_await send_error(*connection->stream, 408, "request header timeout", "HTTP/1.1",
                                Deadline::after(config.write_timeout));
          }
          break;
        }
        if (head.status == detail::ReadStatus::TooLarge) {
          co_await send_error(*connection->stream, 431, head.message, "HTTP/1.1",
                              Deadline::after(config.write_timeout));
          break;
        }
        if (head.status != detail::ReadStatus::Ok) {
          co_await send_error(*connection->stream, 400, head.message, "HTTP/1.1",
                              Deadline::after(config.write_timeout));
          break;
        }

        auto head_end = connection->buffer.find("\r\n\r\n");
        if (head_end == std::string::npos) break;
        std::string_view head_view(connection->buffer.data(), head_end + 4);

        Request request;
        auto parsed = detail::parse_request_head(head_view, request, limits());
        if (!parsed.ok) {
          co_await send_error(*connection->stream, parsed.http_status, parsed.message, "HTTP/1.1",
                              Deadline::after(config.write_timeout));
          break;
        }

        connection->busy = true;
        first_request = false;
        request.remote_address = connection->peer_text;
        request.local_address = connection->local_text;
        request.secure = connection->secure;
        request.tls_version = connection->tls_version;

        if (config.trust_proxy) {
          auto forwarded = request.headers.get("x-forwarded-for");
          if (!forwarded.empty()) {
            auto comma = forwarded.find(',');
            request.remote_address =
                std::string(trim_ows(comma == std::string_view::npos
                                         ? forwarded
                                         : forwarded.substr(0, comma)));
          }
        }

        if (request.expects_continue() && (request.chunked || request.content_length > 0)) {
          auto written = co_await write_all(*connection->stream, "HTTP/1.1 100 Continue\r\n\r\n",
                                            Deadline::after(config.write_timeout));
          if (!written.ok()) break;
        }

        detail::BodyResult body;
        if (request.chunked || request.content_length != 0) {
          body = co_await detail::read_request_body(
              *connection->stream, connection->buffer, parsed.head_length, request, limits(),
              Deadline::after(config.body_timeout));
        } else {
          connection->buffer.erase(0, parsed.head_length);
        }
        if (body.status != detail::ReadStatus::Ok) {
          int status = body.status == detail::ReadStatus::TooLarge ? 413
                       : body.status == detail::ReadStatus::Timeout ? 408
                                                                    : 400;
          co_await send_error(*connection->stream, status, body.message, request.version,
                              Deadline::after(config.write_timeout));
          break;
        }

        auto content_encoding = trim_ows(request.headers.get("content-encoding"));
        if (!content_encoding.empty() && !Headers::equals(content_encoding, "identity")) {
          if (Headers::equals(content_encoding, "gzip")) {
            if (!detail::compression_available()) {
              co_await send_error(*connection->stream, 415, "gzip request bodies are not supported",
                                  request.version, Deadline::after(config.write_timeout));
              break;
            }
            auto decoded = detail::gzip_decompress(request.body, config.max_body_bytes);
            if (!decoded) {
              co_await send_error(*connection->stream, 400, "invalid gzip request body",
                                  request.version, Deadline::after(config.write_timeout));
              break;
            }
            request.body = std::move(*decoded);
            request.headers.remove("content-encoding");
            request.headers.set("content-length", std::to_string(request.body.size()));
            request.content_length = request.body.size();
            request.chunked = false;
          } else {
            co_await send_error(*connection->stream, 415, "unsupported content-encoding",
                                request.version, Deadline::after(config.write_timeout));
            break;
          }
        }

        Response response;
        auto started = std::chrono::steady_clock::now();
        try {
          co_await run_chain(0, request, response);
        } catch (const std::exception& e) {
          log_warning(std::string("handler threw: ") + e.what());
          response = make_error(request, 500, "internal server error");
        } catch (...) {
          log_warning("handler threw a non-standard exception");
          response = make_error(request, 500, "internal server error");
        }

        bool keep_alive = parsed.keep_alive && !response.close_connection &&
                          !stopping.load(std::memory_order_relaxed);
        bool sent = co_await write_response(*connection->stream, request, response, keep_alive,
                                            Deadline::after(config.write_timeout));
        auto elapsed = std::chrono::steady_clock::now() - started;
        if (config.access_log) {
          try {
            config.access_log(request, response, elapsed);
          } catch (...) {
            log_warning("access log callback threw");
          }
        }

        if (response.upgrade) {
          try {
            co_await response.upgrade(request, *connection->stream, connection->buffer);
          } catch (const std::exception& e) {
            log_warning(std::string("upgrade handler failed: ") + e.what());
          }
          break;
        }

        if (!sent || !keep_alive) break;
      }
    } catch (const std::exception& e) {
      log_warning(std::string("connection error: ") + e.what());
    } catch (...) {
      log_warning("connection error");
    }

    co_return;
  }

  task<void> run_chain(std::size_t index, Request& request, Response& response) {
    if (index < middleware.size()) {
      Middleware current = middleware[index];
      Next next = [this, index, &request, &response]() -> task<void> {
        co_await run_chain(index + 1, request, response);
      };
      co_await current(request, response, std::move(next));
      co_return;
    }
    co_await invoke_handler(request, response);
  }

  task<void> invoke_handler(Request& request, Response& response) {
    auto match = router.match(request.method, request.path);

    if (!match.handler && request.method == Method::Options) {
      if (match.path_matched || request.path == "*") {
        response.status = 204;
        response.reason = "No Content";
        if (!match.allowed.empty()) response.headers.set("allow", join_allowed(match.allowed));
        co_return;
      }
    }

    if (!match.handler) {
      if (match.path_matched) {
        response = Response(405, "method not allowed\n", "text/plain; charset=utf-8");
        response.headers.set("allow", join_allowed(match.allowed));
        co_return;
      }
      if (request.method == Method::Other) {
        response = Response(501, "method not implemented\n", "text/plain; charset=utf-8");
        co_return;
      }
      if (not_found) {
        response = co_await not_found(request);
        co_return;
      }
      response = Response::not_found();
      co_return;
    }

    request.route_params = std::move(match.params);
    Handler handler = *match.handler;
    response = co_await handler(request);
    co_return;
  }

  task<bool> write_response(Stream& stream, Request& request, Response& response, bool keep_alive,
                            Deadline deadline) {
    if (response.status < 100 || response.status > 599) {
      response = Response(500, "invalid response status\n", "text/plain; charset=utf-8");
    }
    bool invalid_trailer = false;
    for (const auto& [name, value] : response.trailers.entries()) {
      if (!detail::is_token(name) || !detail::is_valid_header_value(value)) invalid_trailer = true;
    }
    if (invalid_trailer) {
      response = Response(500, "invalid response trailer\n", "text/plain; charset=utf-8");
    }
    for (const auto& [name, value] : response.headers.entries()) {
      if (!detail::is_token(name) || !detail::is_valid_header_value(value)) {
        response = Response(500, "invalid response header\n", "text/plain; charset=utf-8");
        break;
      }
    }
    const bool http11 = request.version == "HTTP/1.1";
    const bool head_request = request.method == Method::Head;
    if (response.reason.empty() || response.reason.find('\r') != std::string::npos ||
        response.reason.find('\n') != std::string::npos) {
      response.reason = std::string(reason_phrase(response.status));
    }

    // Compress owned bodies. Streamed and file bodies are left alone.
    if (config.gzip && detail::compression_available() &&
        response.body.kind() == Body::Kind::Owned && !no_body_status(response.status) &&
        response.body.size() >= 256 && !response.headers.has("content-encoding") &&
        !response.headers.has("content-range") && accepts_gzip(request.headers.get("accept-encoding"))) {
      auto compressed = detail::gzip_compress(response.body.view());
      if (!compressed.empty()) {
        response.body = Body(std::move(compressed));
        response.headers.set("content-encoding", "gzip");
        response.headers.set("vary", "Accept-Encoding");
      }
    }

    bool status_has_no_body = no_body_status(response.status);

    // Resolve file bodies before writing headers so a missing file is a clean
    // 500 instead of a truncated response.
    std::uint64_t file_offset = 0;
    std::uint64_t file_length = 0;
    if (response.body.kind() == Body::Kind::File) {
      const auto* spec = response.body.file();
      struct stat info{};
      if (!spec || ::stat(spec->path.c_str(), &info) != 0 || !S_ISREG(info.st_mode)) {
        response = make_error(request, 500, "cannot read file");
        status_has_no_body = false;
      } else {
        file_offset = spec->offset;
        file_length = spec->length_known ? spec->length
                                         : static_cast<std::uint64_t>(info.st_size) - file_offset;
        if (file_offset > static_cast<std::uint64_t>(info.st_size)) file_offset = 0;
      }
    }

    const bool send_trailers =
        !response.trailers.empty() && http11 && !head_request && !status_has_no_body;
    std::uint64_t body_size = response.body.size();
    bool chunked = send_trailers;
    if (!status_has_no_body) {
      if (response.body.kind() == Body::Kind::Stream) {
        if (!head_request && http11 && (keep_alive || send_trailers)) {
          chunked = true;
        } else if (!head_request) {
          keep_alive = false;
        }
      } else if (response.body.kind() == Body::Kind::File) {
        body_size = file_length;
      } else if (body_size == Body::unknown_size) {
        body_size = 0;
      }
    }

    if (chunked) {
      if (!response.headers.has("trailer") && !response.trailers.empty()) {
        std::string names;
        for (const auto& [name, value] : response.trailers.entries()) {
          if (!names.empty()) names += ", ";
          names += name;
        }
        response.headers.set("trailer", names);
      }
    }

    if (!keep_alive) {
      response.headers.set("connection", "close");
    } else if (request.version == "HTTP/1.0") {
      response.headers.set("connection", "keep-alive");
    }

    if (!response.headers.has("date")) response.headers.set("date", http_date_now());
    if (!config.server_name.empty() && !response.headers.has("server")) {
      response.headers.set("server", config.server_name);
    }

    if (status_has_no_body) {
      response.headers.remove("content-length");
      response.headers.remove("transfer-encoding");
    } else if (chunked) {
      response.headers.set("transfer-encoding", "chunked");
      response.headers.remove("content-length");
    } else if (response.body.kind() != Body::Kind::Stream) {
      response.headers.set("content-length", std::to_string(body_size));
    }

    std::string head;
    head.reserve(256 + response.headers.size() * 32);
    head += std::string(http11 ? "HTTP/1.1" : "HTTP/1.0");
    head += ' ';
    head += std::to_string(response.status);
    head += ' ';
    head += response.reason;
    head += "\r\n";
    for (const auto& [name, value] : response.headers.entries()) {
      head += name;
      head += ": ";
      head += value;
      head += "\r\n";
    }
    head += "\r\n";

    const bool combine_body = !head_request && !status_has_no_body && !chunked &&
                              (response.body.kind() == Body::Kind::Owned ||
                               response.body.kind() == Body::Kind::Borrowed) &&
                              response.body.view().size() <= 64 * 1024;
    if (combine_body) head.append(response.body.view());

    auto written = co_await write_all(stream, head, deadline);
    if (!written.ok()) co_return false;

    if (head_request || status_has_no_body) co_return true;

    try {
      switch (response.body.kind()) {
        case Body::Kind::Empty:
          break;
        case Body::Kind::Owned:
        case Body::Kind::Borrowed: {
          if (combine_body) break;
          if (chunked && !response.body.view().empty()) {
            char size_buffer[32];
            std::snprintf(size_buffer, sizeof size_buffer, "%zx\r\n",
                          response.body.view().size());
            std::string frame = size_buffer;
            frame += response.body.view();
            frame += "\r\n";
            auto result = co_await write_all(stream, frame, deadline);
            if (!result.ok()) co_return false;
          } else {
            auto result = co_await write_all(stream, response.body.view(), deadline);
            if (!result.ok()) co_return false;
          }
          break;
        }
        case Body::Kind::File: {
          const auto* spec = response.body.file();
          if (!spec) co_return false;
          int fd = ::open(spec->path.c_str(), O_RDONLY
#if defined(O_CLOEXEC)
                                           | O_CLOEXEC
#endif
#if defined(O_NOFOLLOW)
                                           | O_NOFOLLOW
#endif
          );
          if (fd < 0) co_return false;
          std::vector<char> buffer(64 * 1024);
          std::uint64_t offset = file_offset;
          std::uint64_t remaining = file_length;
          bool ok = true;
          while (remaining > 0) {
            std::size_t wanted =
                static_cast<std::size_t>(std::min<std::uint64_t>(remaining, buffer.size()));
            ssize_t n = ::pread(fd, buffer.data(), wanted, static_cast<off_t>(offset));
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) {
              ok = false;
              break;
            }
            if (chunked) {
              char size_buffer[32];
              std::snprintf(size_buffer, sizeof size_buffer, "%zx\r\n",
                            static_cast<std::size_t>(n));
              std::string frame = size_buffer;
              frame.append(buffer.data(), static_cast<std::size_t>(n));
              frame += "\r\n";
              auto result = co_await write_all(stream, frame, deadline);
              if (!result.ok()) {
                ok = false;
                break;
              }
            } else {
              auto result = co_await write_all(
                  stream,
                  as_bytes(std::span<const char>(buffer.data(), static_cast<std::size_t>(n))),
                  deadline);
              if (!result.ok()) {
                ok = false;
                break;
              }
            }
            offset += static_cast<std::uint64_t>(n);
            remaining -= static_cast<std::uint64_t>(n);
          }
          ::close(fd);
          if (!ok) co_return false;
          break;
        }
        case Body::Kind::Stream: {
          const auto* producer = response.body.producer();
          if (!producer) co_return false;
          std::string chunk;
          for (;;) {
            bool more = co_await (*producer)(chunk);
            if (!more) break;
            if (chunk.empty()) continue;
            if (chunked) {
              char size_buffer[32];
              std::snprintf(size_buffer, sizeof size_buffer, "%zx\r\n", chunk.size());
              std::string frame = size_buffer;
              frame += chunk;
              frame += "\r\n";
              auto result = co_await write_all(stream, frame, deadline);
              if (!result.ok()) co_return false;
            } else {
              auto result = co_await write_all(stream, chunk, deadline);
              if (!result.ok()) co_return false;
            }
            chunk.clear();
          }
          break;
        }
      }

      if (chunked) {
        std::string ending = "0\r\n";
        for (const auto& [name, value] : response.trailers.entries()) {
          ending += name;
          ending += ": ";
          ending += value;
          ending += "\r\n";
        }
        ending += "\r\n";
        auto result = co_await write_all(stream, ending, deadline);
        if (!result.ok()) co_return false;
      }
    } catch (const std::exception& e) {
      log_warning(std::string("response body failed: ") + e.what());
      co_return false;
    }

    co_return true;
  }

  void begin_drain(Worker& worker) {
    if (worker.draining) return;
    worker.draining = true;
    if (worker.listener.valid()) {
      worker.loop.cancel_io(worker.listener.fd());
      worker.listener.close();
    }

    std::vector<int> idle;
    for (auto& [id, connection] : worker.connections) {
      if (!connection->busy && connection->fd() >= 0) idle.push_back(connection->fd());
    }
    for (int fd : idle) worker.loop.cancel_io(fd);
    if (worker.connections.empty()) worker.loop.stop();

    auto timeout = config.shutdown_timeout;
    worker.loop.after(timeout, [&worker] {
      std::vector<int> remaining;
      for (auto& [id, connection] : worker.connections) {
        if (connection->fd() >= 0) remaining.push_back(connection->fd());
      }
      for (int fd : remaining) worker.loop.cancel_io(fd);
      worker.loop.after(std::chrono::milliseconds(100), [&worker] { worker.loop.stop(); });
    });
  }
};

Server::Server(ServerConfig config) : impl_(std::make_unique<Impl>(std::move(config))) {}

Server::~Server() {
  if (impl_ && impl_->running.load()) {
    try {
      stop();
    } catch (...) {
    }
  }
}

Server& Server::get(std::string_view pattern, Handler handler) {
  impl_->require_configurable();
  impl_->router.add(Method::Get, std::string(pattern), std::move(handler));
  return *this;
}
Server& Server::post(std::string_view pattern, Handler handler) {
  impl_->require_configurable();
  impl_->router.add(Method::Post, std::string(pattern), std::move(handler));
  return *this;
}
Server& Server::put(std::string_view pattern, Handler handler) {
  impl_->require_configurable();
  impl_->router.add(Method::Put, std::string(pattern), std::move(handler));
  return *this;
}
Server& Server::patch(std::string_view pattern, Handler handler) {
  impl_->require_configurable();
  impl_->router.add(Method::Patch, std::string(pattern), std::move(handler));
  return *this;
}
Server& Server::del(std::string_view pattern, Handler handler) {
  impl_->require_configurable();
  impl_->router.add(Method::Delete, std::string(pattern), std::move(handler));
  return *this;
}
Server& Server::head(std::string_view pattern, Handler handler) {
  impl_->require_configurable();
  impl_->router.add(Method::Head, std::string(pattern), std::move(handler));
  return *this;
}
Server& Server::options(std::string_view pattern, Handler handler) {
  impl_->require_configurable();
  impl_->router.add(Method::Options, std::string(pattern), std::move(handler));
  return *this;
}
Server& Server::any(std::string_view pattern, Handler handler) {
  impl_->require_configurable();
  impl_->router.add(Method::Get, std::string(pattern), std::move(handler), true);
  return *this;
}

Server& Server::ws(std::string_view pattern, WsHandler handler) {
  impl_->require_configurable();
  auto route = [handler = std::move(handler)](const Request& request) -> task<Response> {
    auto upgrade = trim_ows(request.header("upgrade"));
    auto connection = request.header("connection");
    auto key = request.header("sec-websocket-key");
    auto version = request.header("sec-websocket-version");

    bool connection_upgrade = false;
    auto tokens = connection;
    while (!tokens.empty()) {
      auto comma = tokens.find(',');
      auto token = trim_ows(comma == std::string_view::npos ? tokens : tokens.substr(0, comma));
      if (Headers::equals(token, "upgrade")) connection_upgrade = true;
      if (comma == std::string_view::npos) break;
      tokens.remove_prefix(comma + 1);
    }

    if (!Headers::equals(upgrade, "websocket") || !connection_upgrade || key.empty() ||
        version != "13") {
      Response response(400, "invalid WebSocket handshake\n", "text/plain; charset=utf-8");
      if (version != "13") {
        response.status = 426;
        response.reason = "Upgrade Required";
        response.headers.set("sec-websocket-version", "13");
      }
      co_return response;
    }

    Response response;
    response.status = 101;
    response.reason = "Switching Protocols";
    response.headers.set("upgrade", "websocket");
    response.headers.set("connection", "Upgrade");
    response.headers.set("sec-websocket-accept", websocket_accept_key(key));
    auto protocols = request.header("sec-websocket-protocol");
    if (!protocols.empty()) {
      auto comma = protocols.find(',');
      response.headers.set("sec-websocket-protocol",
                           std::string(trim_ows(comma == std::string_view::npos
                                                    ? protocols
                                                    : protocols.substr(0, comma))));
    }
    response.upgrade = [handler](Request&, Stream& stream,
                                 std::string& buffer) -> task<void> {
      WebSocket websocket(stream, buffer, false);
      co_await handler(websocket);
    };
    co_return response;
  };
  impl_->router.add(Method::Get, std::string(pattern), std::move(route));
  return *this;
}

Server& Server::use(Middleware middleware_fn) {
  impl_->require_configurable();
  impl_->middleware.push_back(std::move(middleware_fn));
  return *this;
}

Server& Server::on_not_found(Handler handler) {
  impl_->require_configurable();
  impl_->not_found = std::move(handler);
  return *this;
}

Server& Server::on_error(ErrorHandler handler) {
  impl_->require_configurable();
  impl_->error_handler = std::move(handler);
  return *this;
}

Server& Server::static_files(std::string_view prefix, std::string root, StaticOptions options) {
  impl_->require_configurable();
  std::string normalized(prefix);
  if (normalized.empty() || normalized.front() != '/') normalized.insert(normalized.begin(), '/');
  while (normalized.size() > 1 && normalized.back() == '/') normalized.pop_back();

  std::filesystem::path root_path;
  try {
    root_path = std::filesystem::weakly_canonical(root);
  } catch (const std::exception&) {
    root_path = std::filesystem::absolute(root);
  }
  std::string root_string = root_path.string();
  std::string prefix_copy = normalized;
  StaticOptions options_copy = std::move(options);

  auto handler = [root_path, root_string, prefix_copy, options_copy](const Request& request)
      -> task<Response> {
    std::string relative;
    if (prefix_copy == "/") {
      relative = request.path.size() > 1 ? request.path.substr(1) : std::string();
    } else if (request.path == prefix_copy) {
      relative.clear();
    } else if (request.path.starts_with(prefix_copy + "/")) {
      relative = request.path.substr(prefix_copy.size() + 1);
    } else {
      co_return Response::not_found();
    }

    relative = percent_decode(relative);
    if (relative.find('\0') != std::string::npos || relative.find('\\') != std::string::npos ||
        (!relative.empty() && relative.front() == '/')) {
      co_return Response::not_found();
    }

    bool directory_request = relative.empty() || relative.back() == '/';
    if (directory_request) {
      if (!relative.empty() && relative.back() != '/') relative += '/';
      relative += options_copy.index;
    }

    std::filesystem::path candidate;
    try {
      candidate = std::filesystem::weakly_canonical(root_path / relative);
    } catch (const std::exception&) {
      co_return Response::not_found();
    }

    std::string candidate_string = candidate.string();
    bool within = root_string == "/"
                      ? candidate_string.starts_with('/')
                      : (candidate_string == root_string ||
                         candidate_string.starts_with(root_string + "/"));
    if (!within) co_return Response::not_found();

    if (!options_copy.dotfiles) {
      auto relative_path = candidate.lexically_relative(root_path);
      for (const auto& part : relative_path) {
        auto name = part.string();
        if (name.size() > 1 && name.front() == '.') co_return Response::not_found();
      }
    }

    struct stat info{};
    if (::stat(candidate_string.c_str(), &info) != 0) co_return Response::not_found();
    if (S_ISDIR(info.st_mode)) {
      std::filesystem::path with_index;
      try {
        with_index = std::filesystem::weakly_canonical(candidate / options_copy.index);
      } catch (const std::exception&) {
        co_return Response::not_found();
      }
      std::string with_index_string = with_index.string();
      bool index_within = root_string == "/"
                              ? with_index_string.starts_with('/')
                              : (with_index_string == root_string ||
                                 with_index_string.starts_with(root_string + "/"));
      if (!index_within) co_return Response::not_found();
      candidate = std::move(with_index);
      candidate_string = std::move(with_index_string);
      if (::stat(candidate_string.c_str(), &info) != 0 || !S_ISREG(info.st_mode)) {
        co_return Response::not_found();
      }
    }
    if (!S_ISREG(info.st_mode)) co_return Response::not_found();

    std::string etag = etag_for(static_cast<std::int64_t>(info.st_mtime),
                                static_cast<std::uint64_t>(info.st_size));
    std::string last_modified = http_date(info.st_mtime);

    auto cache_control = [&]() {
      if (options_copy.cache_max_age.count() > 0) {
        return std::string("public, max-age=") +
               std::to_string(options_copy.cache_max_age.count());
      }
      return std::string();
    };

    bool not_modified = false;
    auto if_none_match = request.headers.get("if-none-match");
    if (options_copy.etag && !if_none_match.empty()) {
      not_modified = etag_matches(if_none_match, etag);
    } else {
      auto if_modified_since = request.headers.get("if-modified-since");
      if (!if_modified_since.empty()) {
        auto since = parse_http_date_value(trim_ows(if_modified_since));
        not_modified = since != 0 && info.st_mtime <= since;
      }
    }
    if (not_modified) {
      Response response;
      response.status = 304;
      response.reason = "Not Modified";
      if (options_copy.etag) response.headers.set("etag", etag);
      response.headers.set("last-modified", last_modified);
      if (!cache_control().empty()) response.headers.set("cache-control", cache_control());
      co_return response;
    }

    std::uint64_t size = static_cast<std::uint64_t>(info.st_size);
    std::string content_type = mime_type(candidate_string);
    RangeParse range;
    if (request.method == Method::Get && size > 0) {
      range = parse_ranges(request.headers.get("range"), size);
      auto if_range = trim_ows(request.headers.get("if-range"));
      if (!if_range.empty() && range.status != RangeResult::Ignore) {
        bool apply = false;
        if (if_range.starts_with("W/")) {
          apply = false;
        } else if (!if_range.empty() && if_range.front() == '"') {
          apply = options_copy.etag && Headers::equals(if_range, etag);
        } else {
          auto date = parse_http_date_value(if_range);
          apply = date != 0 && info.st_mtime <= date;
        }
        if (!apply) range = RangeParse{};
      }
    }

    Response response;
    if (range.status == RangeResult::Unsatisfiable) {
      response.status = 416;
      response.reason = "Range Not Satisfiable";
      response.headers.set("content-range", "bytes */" + std::to_string(size));
      response.headers.set("content-type", "text/plain; charset=utf-8");
      response.body = Body("range not satisfiable\n");
      co_return response;
    }

    if (range.status == RangeResult::Satisfiable && range.ranges.size() == 1) {
      auto [start, end] = range.ranges.front();
      response.status = 206;
      response.reason = "Partial Content";
      response.body = Body::file_range(candidate_string, start, end - start + 1);
      response.headers.set("content-range",
                           "bytes " + std::to_string(start) + "-" + std::to_string(end) + "/" +
                               std::to_string(size));
      response.headers.set("content-type", content_type);
    } else if (range.status == RangeResult::Satisfiable) {
      static std::atomic<std::uint64_t> boundary_counter{0};
      std::string boundary = "ferry" + std::to_string(info.st_mtime) + "-" +
                             std::to_string(boundary_counter.fetch_add(1));
      response.status = 206;
      response.reason = "Partial Content";
      response.body = Body::stream(make_multipart_range_producer(
          candidate_string, content_type, boundary, size, range.ranges));
      response.headers.set("content-type", "multipart/byteranges; boundary=" + boundary);
    } else {
      response.status = 200;
      response.reason = "OK";
      response.body = Body::file_range(candidate_string, 0, size);
      response.headers.set("content-type", content_type);
    }

    response.headers.set("accept-ranges", "bytes");
    response.headers.set("last-modified", last_modified);
    if (options_copy.etag) response.headers.set("etag", etag);
    if (!cache_control().empty()) response.headers.set("cache-control", cache_control());
    co_return response;
  };

  std::string pattern = normalized == "/" ? "/{*path}" : normalized + "/{*path}";
  impl_->router.add(Method::Get, pattern, std::move(handler));
  return *this;
}

void Server::start() {
  if (impl_->running.load()) throw std::runtime_error("ferry: server already started");

  impl_->stopping.store(false);
  impl_->workers.clear();

  if (impl_->config.threads < 1) impl_->config.threads = 1;

  if (impl_->config.tls) {
    std::string error;
    impl_->tls_context = TlsContext::make_server(*impl_->config.tls, error);
    if (!impl_->tls_context) throw std::runtime_error("ferry: " + error);
  } else {
    impl_->tls_context.reset();
  }

  int worker_count = impl_->config.threads;
  auto listener = listen_tcp(impl_->config.bind_address, impl_->config.port,
                             impl_->config.backlog, false);
  if (!listener.socket.valid()) {
    throw std::runtime_error("ferry: " + listener.error);
  }
  impl_->port = socket_port(listener.socket.fd());

  for (int i = 0; i < worker_count; ++i) {
    auto worker = std::make_unique<Impl::Worker>(impl_.get());
    if (i == 0) worker->listener = std::move(listener.socket);
    impl_->workers.push_back(std::move(worker));
  }

  impl_->running.store(true);
  impl_->next_worker.store(0, std::memory_order_relaxed);
  for (std::size_t i = 0; i < impl_->workers.size(); ++i) {
    Impl::Worker* raw = impl_->workers[i].get();
    const bool acceptor = i == 0;
    raw->thread = std::thread([raw, acceptor] {
      if (acceptor) raw->loop.post([raw] { spawn(raw->impl->accept_loop(*raw)); });
      raw->loop.run();
    });
  }
}

void Server::serve() {
  start();
  wait();
}

void Server::stop() {
  for (const auto& worker : impl_->workers) {
    if (worker->thread.joinable() && worker->thread.get_id() == std::this_thread::get_id()) {
      throw std::logic_error("ferry: Server::stop() cannot be called from a handler thread");
    }
  }

  std::lock_guard<std::mutex> stop_lock(impl_->stop_mutex);
  if (!impl_->running.load()) return;

  impl_->stopping.store(true);
  for (auto& worker : impl_->workers) {
    Impl::Worker* raw = worker.get();
    worker->loop.post([raw] { raw->impl->begin_drain(*raw); });
  }
  for (auto& worker : impl_->workers) {
    if (worker->thread.joinable()) worker->thread.join();
  }
  impl_->running.store(false);
  impl_->wait_cv.notify_all();
}

void Server::wait() {
  if (!impl_->running.load()) return;
  std::unique_lock<std::mutex> lock(impl_->wait_mutex);
  impl_->wait_cv.wait(lock, [this] { return !impl_->running.load(); });
}

bool Server::running() const noexcept { return impl_->running.load(); }

std::uint16_t Server::port() const noexcept { return impl_->port; }

const ServerConfig& Server::config() const noexcept { return impl_->config; }

}  // namespace ferry
