#include "ferry/client.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "codec.hpp"
#include "compression.hpp"
#include "ferry/event_loop.hpp"
#include "ferry/net.hpp"
#include "ferry/tls.hpp"

namespace ferry {

namespace {

std::string_view trim_ows(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
  return text;
}

bool is_safe_method(Method method) {
  return method == Method::Get || method == Method::Head || method == Method::Options ||
         method == Method::Trace;
}

std::string base64_encode(std::string_view input) {
  static const char* alphabet =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string output;
  output.reserve((input.size() + 2) / 3 * 4);
  std::size_t i = 0;
  while (i + 2 < input.size()) {
    std::uint32_t value = (static_cast<unsigned char>(input[i]) << 16) |
                          (static_cast<unsigned char>(input[i + 1]) << 8) |
                          static_cast<unsigned char>(input[i + 2]);
    output.push_back(alphabet[(value >> 18) & 63]);
    output.push_back(alphabet[(value >> 12) & 63]);
    output.push_back(alphabet[(value >> 6) & 63]);
    output.push_back(alphabet[value & 63]);
    i += 3;
  }
  if (i + 1 == input.size()) {
    std::uint32_t value = static_cast<unsigned char>(input[i]) << 16;
    output.push_back(alphabet[(value >> 18) & 63]);
    output.push_back(alphabet[(value >> 12) & 63]);
    output.push_back('=');
    output.push_back('=');
  } else if (i + 2 == input.size()) {
    std::uint32_t value = (static_cast<unsigned char>(input[i]) << 16) |
                          (static_cast<unsigned char>(input[i + 1]) << 8);
    output.push_back(alphabet[(value >> 18) & 63]);
    output.push_back(alphabet[(value >> 12) & 63]);
    output.push_back(alphabet[(value >> 6) & 63]);
    output.push_back('=');
  }
  return output;
}

std::string origin_key(const Url& url) {
  return url.scheme + "://" + url.host + ":" + std::to_string(url.effective_port());
}

}  // namespace

struct Client::Impl {
  struct Pending {
    Method method = Method::Get;
    Url url;
    Body body;
    Headers headers;
  };

  struct IdleConnection {
    std::unique_ptr<Stream> stream;
    std::chrono::steady_clock::time_point used;
  };

  struct Acquired {
    std::unique_ptr<Stream> stream;
    bool reused = false;
  };

  ClientConfig config;
  CookieJar jar;
  EventLoop* loop = nullptr;
  std::shared_ptr<TlsContext> verified_tls;
  std::shared_ptr<TlsContext> insecure_tls;
  std::unordered_map<std::string, std::vector<IdleConnection>> pool;

  explicit Impl(ClientConfig cfg) : config(std::move(cfg)) {}

  std::shared_ptr<TlsContext> tls_context(std::string& error) {
    if (config.verify_tls) {
      if (!verified_tls) {
        verified_tls = TlsContext::make_client(true, config.ca_file, error,
                                               config.client_certificate_file,
                                               config.client_private_key_file);
      }
      return verified_tls;
    }
    if (!insecure_tls) {
      insecure_tls = TlsContext::make_client(false, {}, error, config.client_certificate_file,
                                             config.client_private_key_file);
    }
    return insecure_tls;
  }

  task<Acquired> acquire(const Url& url, Deadline deadline, bool allow_reuse, std::string& error) {
    Acquired acquired;
    auto key = origin_key(url);

    if (allow_reuse && config.reuse_connections) {
      auto found = pool.find(key);
      if (found != pool.end()) {
        auto& idle = found->second;
        auto now = std::chrono::steady_clock::now();
        while (!idle.empty()) {
          auto connection = std::move(idle.back());
          idle.pop_back();
          if (config.idle_timeout.count() > 0 && now - connection.used > config.idle_timeout) {
            continue;
          }
          if (connection.stream && connection.stream->fd() >= 0) {
            acquired.stream = std::move(connection.stream);
            acquired.reused = true;
            break;
          }
        }
        if (idle.empty()) pool.erase(found);
      }
    }

    if (acquired.stream) co_return acquired;

    auto connect_deadline = Deadline::earlier(
        deadline, Deadline::after(config.connect_timeout));
    auto connected = co_await connect_tcp(url.host, url.effective_port(), connect_deadline);
    if (!connected.socket.valid()) {
      error = connected.error;
      co_return acquired;
    }

    if (url.is_tls()) {
      auto context = tls_context(error);
      if (!context) co_return acquired;
      auto result = co_await TlsStream::connect(std::move(connected.socket), url.host, context,
                                                config.verify_tls, deadline);
      if (!result.ok()) {
        error = result.error;
        co_return acquired;
      }
      acquired.stream = std::move(result.stream);
    } else {
      acquired.stream = std::make_unique<PlainStream>(std::move(connected.socket));
    }
    co_return acquired;
  }

  void release(std::unique_ptr<Stream> stream, const Url& url) {
    if (!stream) return;
    if (!config.reuse_connections || config.max_idle_per_host == 0) return;
    auto& idle = pool[origin_key(url)];
    idle.push_back(IdleConnection{std::move(stream), std::chrono::steady_clock::now()});
    while (idle.size() > config.max_idle_per_host) {
      idle.erase(idle.begin());
    }
  }

  task<bool> send_request(Stream& stream, const Pending& pending, bool keep_alive,
                          Deadline deadline, std::string& error) {
    Headers headers = config.default_headers;
    for (const auto& [name, value] : pending.headers.entries()) {
      headers.set(name, value);
    }

    headers.set("host", pending.url.host_header());
    if (!headers.has("user-agent")) headers.set("user-agent", config.user_agent);
    if (!headers.has("accept")) headers.set("accept", "*/*");
    if (config.accept_gzip && detail::compression_available() &&
        !headers.has("accept-encoding")) {
      headers.set("accept-encoding", "gzip");
    }
    headers.set("connection", keep_alive ? "keep-alive" : "close");

    if (config.use_cookies) {
      auto cookie = jar.header_for(pending.url);
      if (!cookie.empty()) {
        auto existing = headers.get("cookie");
        if (existing.empty()) {
          headers.set("cookie", std::move(cookie));
        } else {
          headers.set("cookie", std::string(existing) + "; " + cookie);
        }
      }
    }

    if (!pending.url.userinfo.empty() && !headers.has("authorization")) {
      headers.set("authorization",
                  "Basic " + base64_encode(percent_decode(pending.url.userinfo)));
    }

    bool chunked = pending.body.kind() == Body::Kind::Stream;
    std::uint64_t size = pending.body.size();

    if (pending.body.kind() == Body::Kind::File) {
      const auto* file = pending.body.file();
      if (!file) {
        error = "invalid file body";
        co_return false;
      }
      if (size == Body::unknown_size) {
        struct stat info{};
        if (::stat(file->path.c_str(), &info) != 0) {
          error = "cannot stat request body file";
          co_return false;
        }
        auto total = static_cast<std::uint64_t>(info.st_size);
        if (file->offset > total) {
          error = "file body offset past end of file";
          co_return false;
        }
        size = total - file->offset;
      }
    }

    headers.remove("transfer-encoding");
    headers.remove("content-length");
    if (chunked) {
      headers.set("transfer-encoding", "chunked");
    } else if (size > 0 || pending.method == Method::Post || pending.method == Method::Put ||
               pending.method == Method::Patch || pending.method == Method::Delete) {
      headers.set("content-length", std::to_string(size));
    }

    std::string head;
    head.reserve(256 + headers.size() * 32);
    head += method_name(pending.method);
    head += ' ';
    head += pending.url.target();
    head += " HTTP/1.1\r\n";
    for (const auto& [name, value] : headers.entries()) {
      head += name;
      head += ": ";
      head += value;
      head += "\r\n";
    }
    head += "\r\n";

    const bool combine_body = !chunked && (pending.body.kind() == Body::Kind::Owned ||
                                           pending.body.kind() == Body::Kind::Borrowed) &&
                              pending.body.view().size() <= 64 * 1024;
    if (combine_body) head.append(pending.body.view());

    auto result = co_await write_all(stream, head, deadline);
    if (!result.ok()) {
      error = result.message();
      co_return false;
    }

    switch (pending.body.kind()) {
      case Body::Kind::Empty:
        break;
      case Body::Kind::Owned:
      case Body::Kind::Borrowed: {
        if (combine_body) break;
        result = co_await write_all(stream, pending.body.view(), deadline);
        if (!result.ok()) {
          error = result.message();
          co_return false;
        }
        break;
      }
      case Body::Kind::File: {
        const auto* file = pending.body.file();
        if (!file) {
          error = "invalid file body";
          co_return false;
        }
        int fd = ::open(file->path.c_str(), O_RDONLY
#if defined(O_CLOEXEC)
                                               | O_CLOEXEC
#endif
        );
        if (fd < 0) {
          error = "cannot open request body file";
          co_return false;
        }
        std::vector<char> buffer(64 * 1024);
        std::uint64_t offset = file->offset;
        std::uint64_t remaining = size;
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
          result = co_await write_all(
              stream, as_bytes(std::span<const char>(buffer.data(), static_cast<std::size_t>(n))),
              deadline);
          if (!result.ok()) {
            error = result.message();
            ok = false;
            break;
          }
          offset += static_cast<std::uint64_t>(n);
          remaining -= static_cast<std::uint64_t>(n);
        }
        ::close(fd);
        if (!ok) {
          if (error.empty()) error = "failed to read request body file";
          co_return false;
        }
        break;
      }
      case Body::Kind::Stream: {
        const auto* producer = pending.body.producer();
        if (!producer) {
          error = "invalid streaming body";
          co_return false;
        }
        std::string chunk;
        for (;;) {
          bool more = co_await (*producer)(chunk);
          if (!more) break;
          if (chunk.empty()) continue;
          char size_buffer[32];
          std::snprintf(size_buffer, sizeof size_buffer, "%zx\r\n", chunk.size());
          std::string frame = size_buffer;
          frame += chunk;
          frame += "\r\n";
          result = co_await write_all(stream, frame, deadline);
          if (!result.ok()) {
            error = result.message();
            co_return false;
          }
          chunk.clear();
        }
        result = co_await write_all(stream, "0\r\n\r\n", deadline);
        if (!result.ok()) {
          error = result.message();
          co_return false;
        }
        break;
      }
    }

    co_return true;
  }

  task<bool> exchange(Stream& stream, const Pending& pending, bool keep_alive, Response& response,
                      Deadline deadline, std::string& error, bool& close_after) {
    if (!co_await send_request(stream, pending, keep_alive, deadline, error)) co_return false;

    detail::Limits limits;
    limits.max_header_bytes = config.max_header_bytes;
    limits.max_body_bytes = config.max_response_body;

    std::string buffer;
    for (;;) {
      auto head = co_await detail::read_head(stream, buffer, config.max_header_bytes, deadline);
      if (head.status != detail::ReadStatus::Ok) {
        error = head.status == detail::ReadStatus::Eof ? "server closed the connection"
                                                       : head.message;
        co_return false;
      }
      auto head_end = buffer.find("\r\n\r\n");
      if (head_end == std::string::npos) {
        error = "malformed response";
        co_return false;
      }
      std::string_view head_view(buffer.data(), head_end + 4);

      Response parsed;
      auto parse = detail::parse_response_head(head_view, parsed, limits);
      if (!parse.ok) {
        error = parse.message;
        co_return false;
      }

      if (parsed.status >= 100 && parsed.status < 200) {
        if (parsed.status == 101) {
          error = "protocol switching is not supported";
          co_return false;
        }
        buffer.erase(0, head_end + 4);
        continue;
      }

      auto body = co_await detail::read_response_body(
          stream, buffer, head_end + 4, parsed, limits, deadline, pending.method == Method::Head,
          close_after);
      if (body.status != detail::ReadStatus::Ok) {
        error = body.message;
        co_return false;
      }

      auto encoding = trim_ows(parsed.headers.get("content-encoding"));
      if (Headers::equals(encoding, "gzip")) {
        if (!detail::compression_available()) {
          error = "server sent gzip but zlib support is not built in";
          co_return false;
        }
        auto decoded = detail::gzip_decompress(parsed.body.view(), config.max_response_body);
        if (!decoded) {
          error = "invalid or oversized gzip response body";
          co_return false;
        }
        parsed.body = Body(std::move(*decoded));
        parsed.headers.remove("content-encoding");
        parsed.headers.set("content-length", std::to_string(parsed.body.size()));
      }

      response = std::move(parsed);
      co_return true;
    }
  }

  task<Response> execute(Pending pending) {
    Response failure;
    failure.status = 0;
    EventLoop* current = EventLoop::current();
    if (!current) {
      failure.error = "ferry::Client requires an event loop (use ferry::run)";
      co_return failure;
    }
    if (!loop) {
      loop = current;
    } else if (loop != current) {
      failure.error = "ferry::Client cannot be used from multiple event loops";
      co_return failure;
    }

    if (!pending.url.valid() || (pending.url.scheme != "http" && pending.url.scheme != "https")) {
      failure.error = "invalid or unsupported URL";
      co_return failure;
    }

    auto deadline = Deadline::after(config.timeout);
    int redirects = 0;
    bool force_new_connection = false;

    for (;;) {
      if (deadline.expired()) {
        failure.error = "request timed out";
        co_return failure;
      }

      std::string error;
      auto acquired = co_await acquire(pending.url, deadline, !force_new_connection, error);
      if (!acquired.stream) {
        failure.error = error.empty() ? "connection failed" : error;
        co_return failure;
      }

      bool close_after = false;
      Response response;
      bool ok = co_await exchange(*acquired.stream, pending, config.reuse_connections, response,
                                  deadline, error, close_after);

      if (!ok && acquired.reused && !force_new_connection && is_safe_method(pending.method) &&
          pending.body.kind() != Body::Kind::Stream) {
        acquired.stream.reset();
        force_new_connection = true;
        continue;
      }
      force_new_connection = false;

      if (!ok) {
        failure.error = error.empty() ? "request failed" : error;
        co_return failure;
      }

      if (config.use_cookies) {
        for (auto set_cookie : response.headers.get_all("set-cookie")) {
          jar.store(pending.url, set_cookie);
        }
      }

      bool keep = config.reuse_connections && !close_after && !response.close_connection;
      if (keep) {
        release(std::move(acquired.stream), pending.url);
      } else {
        acquired.stream.reset();
      }

      auto location = response.headers.get("location");
      bool redirect = (response.status == 301 || response.status == 302 ||
                       response.status == 303 || response.status == 307 || response.status == 308) &&
                      !location.empty() && config.follow_redirects;

      if (!redirect) co_return response;

      if (redirects >= config.max_redirects) {
        failure.error = "too many redirects";
        co_return failure;
      }
      ++redirects;

      auto resolved = resolve_url(pending.url, location);
      auto next = Url::parse(resolved);
      if (!next || (next->scheme != "http" && next->scheme != "https")) {
        failure.error = "invalid redirect location";
        co_return failure;
      }

      bool switch_to_get = response.status == 303 ||
                           ((response.status == 301 || response.status == 302) &&
                            pending.method != Method::Get && pending.method != Method::Head);
      if (switch_to_get) {
        pending.method = Method::Get;
        pending.body = Body();
        pending.headers.remove("content-type");
        pending.headers.remove("content-length");
        pending.headers.remove("transfer-encoding");
      } else if ((response.status == 307 || response.status == 308) &&
                 pending.body.kind() == Body::Kind::Stream) {
        failure.error = "cannot replay a streaming body after a redirect";
        co_return failure;
      }

      if (next->origin() != pending.url.origin()) {
        pending.headers.remove("authorization");
      }
      pending.url = std::move(*next);
    }
  }
};

Client::Client(ClientConfig config) : impl_(std::make_unique<Impl>(std::move(config))) {}

Client::~Client() = default;

Client::Client(Client&&) noexcept = default;
Client& Client::operator=(Client&&) noexcept = default;

task<Response> Client::get(std::string_view url, Headers headers) {
  co_return co_await send(Method::Get, url, Body{}, std::move(headers));
}

task<Response> Client::head(std::string_view url, Headers headers) {
  co_return co_await send(Method::Head, url, Body{}, std::move(headers));
}

task<Response> Client::post(std::string_view url, std::string body, std::string content_type) {
  Headers headers;
  if (!content_type.empty()) headers.set("content-type", std::move(content_type));
  co_return co_await send(Method::Post, url, Body(std::move(body)), std::move(headers));
}

task<Response> Client::put(std::string_view url, std::string body, std::string content_type) {
  Headers headers;
  if (!content_type.empty()) headers.set("content-type", std::move(content_type));
  co_return co_await send(Method::Put, url, Body(std::move(body)), std::move(headers));
}

task<Response> Client::patch(std::string_view url, std::string body, std::string content_type) {
  Headers headers;
  if (!content_type.empty()) headers.set("content-type", std::move(content_type));
  co_return co_await send(Method::Patch, url, Body(std::move(body)), std::move(headers));
}

task<Response> Client::del(std::string_view url, Headers headers) {
  co_return co_await send(Method::Delete, url, Body{}, std::move(headers));
}

task<Response> Client::send(Method method, std::string_view url, Body body, Headers headers) {
  try {
    auto parsed = Url::parse(url);
    if (!parsed) {
      Response failure;
      failure.status = 0;
      failure.error = "invalid URL";
      co_return failure;
    }
    Impl::Pending pending;
    pending.method = method;
    pending.url = std::move(*parsed);
    pending.body = std::move(body);
    pending.headers = std::move(headers);
    co_return co_await impl_->execute(std::move(pending));
  } catch (const std::exception& e) {
    Response failure;
    failure.status = 0;
    failure.error = e.what();
    co_return failure;
  } catch (...) {
    Response failure;
    failure.status = 0;
    failure.error = "unknown request failure";
    co_return failure;
  }
}

void Client::close() {
  if (impl_) impl_->pool.clear();
}

CookieJar& Client::cookies() { return impl_->jar; }

void Client::clear_cookies() { impl_->jar.clear(); }

const ClientConfig& Client::config() const noexcept { return impl_->config; }

}  // namespace ferry
