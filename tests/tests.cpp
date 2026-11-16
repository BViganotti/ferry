#include "test.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <netinet/in.h>

#include "codec.hpp"
#include "compression.hpp"
#include "router.hpp"

namespace {

using namespace std::chrono_literals;

ferry::ServerConfig test_config(int threads = 2) {
  ferry::ServerConfig config;
  config.bind_address = "127.0.0.1";
  config.port = 0;
  config.threads = threads;
  config.backlog = 256;
  config.header_timeout = 2000ms;
  config.body_timeout = 2000ms;
  config.idle_timeout = 2000ms;
  config.write_timeout = 2000ms;
  config.shutdown_timeout = 500ms;
  return config;
}

std::string base_url(const ferry::Server& server) {
  return "http://127.0.0.1:" + std::to_string(server.port());
}

ferry::task<std::string> raw_request(std::uint16_t port, std::string request) {
  auto connected = co_await ferry::connect_tcp("127.0.0.1", port, ferry::Deadline::after(2000ms));
  CHECK(connected.socket.valid());
  ferry::PlainStream stream(std::move(connected.socket));
  auto written = co_await ferry::write_all(stream, request, ferry::Deadline::after(2000ms));
  CHECK(written.ok());

  std::string output;
  char buffer[8192];
  for (;;) {
    auto read = co_await stream.read_some(ferry::as_bytes(std::span<char>(buffer, sizeof buffer)),
                                          ferry::Deadline::after(500ms));
    if (read.eof() || read.timeout()) break;
    CHECK(read.ok());
    output.append(buffer, read.n);
  }
  co_return output;
}

bool contains(const std::string& haystack, std::string_view needle) {
  return haystack.find(needle) != std::string::npos;
}

std::string lower(std::string text) {
  for (char& c : text) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return text;
}

class MiniProxy {
 public:
  MiniProxy() {
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    ::bind(listen_fd_, reinterpret_cast<sockaddr*>(&address), sizeof address);
    ::listen(listen_fd_, 16);
    socklen_t length = sizeof address;
    ::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&address), &length);
    port_ = ntohs(address.sin_port);
    thread_ = std::thread([this] { accept_loop(); });
  }

  ~MiniProxy() {
    stopping_ = true;
    if (listen_fd_ >= 0) {
      ::shutdown(listen_fd_, SHUT_RDWR);
      ::close(listen_fd_);
    }
    if (thread_.joinable()) thread_.join();
    for (auto& thread : handlers_) {
      if (thread.joinable()) thread.join();
    }
  }

  std::uint16_t port() const { return port_; }

 private:
  void accept_loop() {
    while (!stopping_) {
      int fd = ::accept(listen_fd_, nullptr, nullptr);
      if (fd < 0) break;
      handlers_.emplace_back([this, fd] { handle(fd); });
    }
  }

  static bool write_all(int fd, const char* data, std::size_t size) {
    while (size > 0) {
      ssize_t n = ::send(fd, data, size, 0);
      if (n <= 0) return false;
      data += n;
      size -= static_cast<std::size_t>(n);
    }
    return true;
  }

  void handle(int client) {
    std::string request;
    char buffer[4096];
    while (request.find("\r\n\r\n") == std::string::npos) {
      ssize_t n = ::recv(client, buffer, sizeof buffer, 0);
      if (n <= 0) {
        ::close(client);
        return;
      }
      request.append(buffer, static_cast<std::size_t>(n));
      if (request.size() > 65536) {
        ::close(client);
        return;
      }
    }

    auto line_end = request.find("\r\n");
    auto line = request.substr(0, line_end);
    if (!line.starts_with("CONNECT ")) {
      ::close(client);
      return;
    }
    auto authority = line.substr(8, line.find(' ', 8) - 8);
    auto colon = authority.rfind(':');
    if (colon == std::string::npos) {
      ::close(client);
      return;
    }
    std::string host = authority.substr(0, colon);
    auto port = static_cast<std::uint16_t>(std::atoi(authority.substr(colon + 1).c_str()));
    auto addresses = ferry::Address::resolve(host, port);
    if (addresses.empty()) {
      ::close(client);
      return;
    }

    int upstream = -1;
    for (const auto& address : addresses) {
      int candidate = ::socket(address.family(), SOCK_STREAM, 0);
      if (candidate < 0) continue;
      if (::connect(candidate, reinterpret_cast<const sockaddr*>(&address.storage),
                    address.length) == 0) {
        upstream = candidate;
        break;
      }
      ::close(candidate);
    }
    if (upstream < 0) {
      ::close(client);
      return;
    }

    const char* established = "HTTP/1.1 200 Connection Established\r\n\r\n";
    if (!write_all(client, established, std::strlen(established))) {
      ::close(upstream);
      ::close(client);
      return;
    }

    pollfd fds[2] = {{client, POLLIN, 0}, {upstream, POLLIN, 0}};
    for (;;) {
      int ready = ::poll(fds, 2, 5000);
      if (ready <= 0) break;
      bool closed = false;
      for (int i = 0; i < 2; ++i) {
        if ((fds[i].revents & (POLLIN | POLLHUP | POLLERR)) == 0) continue;
        ssize_t n = ::recv(fds[i].fd, buffer, sizeof buffer, 0);
        if (n <= 0) {
          closed = true;
          break;
        }
        if (!write_all(fds[1 - i].fd, buffer, static_cast<std::size_t>(n))) {
          closed = true;
          break;
        }
      }
      if (closed) break;
    }
    ::close(upstream);
    ::close(client);
  }

  int listen_fd_ = -1;
  std::uint16_t port_ = 0;
  std::thread thread_;
  std::vector<std::thread> handlers_;
  std::atomic<bool> stopping_{false};
};

struct TempDir {
  std::filesystem::path path;
  TempDir() {
    path = std::filesystem::temp_directory_path() /
           ("ferry-tests-" + std::to_string(static_cast<long>(::getpid())) + "-" +
            std::to_string(std::rand()));
    std::filesystem::create_directories(path);
  }
  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }
};

void write_file(const std::filesystem::path& path, std::string_view content) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream file(path, std::ios::binary);
  file.write(content.data(), static_cast<std::streamsize>(content.size()));
}

}  // namespace

std::vector<TestCase>& test_registry() {
  static std::vector<TestCase> tests;
  return tests;
}

TestRegistration::TestRegistration(const char* name, std::function<void()> fn) {
  test_registry().push_back(TestCase{name, std::move(fn)});
}

TEST(headers) {
  ferry::Headers headers;
  headers.set("Content-Type", "text/plain");
  CHECK(headers.has("content-type"));
  CHECK_EQ(headers.get("CONTENT-TYPE"), "text/plain");
  headers.add("set-cookie", "a=1");
  headers.add("set-cookie", "b=2");
  CHECK_EQ(headers.get_all("Set-Cookie").size(), std::size_t{2});
  headers.set("content-type", "application/json");
  CHECK_EQ(headers.get("content-type"), "application/json");
  CHECK_EQ(headers.get_all("content-type").size(), std::size_t{1});
  CHECK(headers.remove("SET-COOKIE"));
  CHECK(!headers.has("set-cookie"));

  bool threw = false;
  try {
    headers.set("x-bad", "value\r\ninjected: yes");
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  CHECK(threw);
}

TEST(url_parsing) {
  auto url = ferry::Url::parse("https://user:pass@example.com:8443/a/b?q=1#frag");
  CHECK(url.has_value());
  CHECK_EQ(url->scheme, "https");
  CHECK_EQ(url->host, "example.com");
  CHECK_EQ(url->port, std::uint16_t{8443});
  CHECK_EQ(url->path, "/a/b");
  CHECK_EQ(url->query, "q=1");
  CHECK_EQ(url->fragment, "frag");
  CHECK_EQ(url->userinfo, "user:pass");
  CHECK_EQ(url->target(), "/a/b?q=1");
  CHECK_EQ(url->host_header(), "example.com:8443");

  auto ipv6 = ferry::Url::parse("http://[::1]:8080/");
  CHECK(ipv6.has_value());
  CHECK_EQ(ipv6->host, "::1");
  CHECK_EQ(ipv6->effective_port(), std::uint16_t{8080});
  CHECK_EQ(ipv6->host_header(), "[::1]:8080");

  auto plain = ferry::Url::parse("http://example.com");
  CHECK(plain.has_value());
  CHECK_EQ(plain->path, "/");
  CHECK_EQ(plain->effective_port(), std::uint16_t{80});

  CHECK(!ferry::Url::parse("example.com/path").has_value());
  CHECK(!ferry::Url::parse("http://").has_value());

  CHECK_EQ(ferry::percent_decode("a%20b%2Fc"), "a b/c");
  CHECK_EQ(ferry::percent_decode("a+b", true), "a b");
  CHECK_EQ(ferry::percent_encode("a b/c"), "a%20b%2Fc");

  auto base = ferry::Url::parse("http://example.com/dir/page?x=1");
  CHECK(base.has_value());
  CHECK_EQ(ferry::resolve_url(*base, "other"), "http://example.com/dir/other");
  CHECK_EQ(ferry::resolve_url(*base, "../up"), "http://example.com/up");
  CHECK_EQ(ferry::resolve_url(*base, "/root"), "http://example.com/root");
  CHECK_EQ(ferry::resolve_url(*base, "//cdn.example.com/x"), "http://cdn.example.com/x");
  CHECK_EQ(ferry::resolve_url(*base, "https://other.example/z"), "https://other.example/z");
}

TEST(router_matching) {
  ferry::detail::Router router;
  auto handler = [](const ferry::Request&) -> ferry::task<ferry::Response> {
    co_return ferry::Response::text("ok");
  };

  router.add(ferry::Method::Get, "/exact", handler);
  router.add(ferry::Method::Get, "/users/{id}", handler);
  router.add(ferry::Method::Post, "/users", handler);
  router.add(ferry::Method::Get, "/files/{*path}", handler);

  auto exact = router.match(ferry::Method::Get, "/exact");
  CHECK(exact.handler != nullptr);

  auto parameter = router.match(ferry::Method::Get, "/users/42");
  CHECK(parameter.handler != nullptr);
  CHECK_EQ(parameter.params.size(), std::size_t{1});
  CHECK_EQ(parameter.params[0].first, "id");
  CHECK_EQ(parameter.params[0].second, "42");

  auto head = router.match(ferry::Method::Head, "/users/42");
  CHECK(head.handler != nullptr);

  auto wrong_method = router.match(ferry::Method::Delete, "/users/42");
  CHECK(wrong_method.handler == nullptr);
  CHECK(wrong_method.path_matched);
  CHECK(!wrong_method.allowed.empty());

  auto wildcard = router.match(ferry::Method::Get, "/files/a/b/c");
  CHECK(wildcard.handler != nullptr);
  CHECK_EQ(wildcard.params[0].second, "a/b/c");

  auto missing = router.match(ferry::Method::Get, "/nope");
  CHECK(missing.handler == nullptr);
  CHECK(!missing.path_matched);
}

TEST(request_parser) {
  ferry::detail::Limits limits;
  ferry::Request request;
  std::string head =
      "POST /echo?a=1 HTTP/1.1\r\nHost: example.com\r\nContent-Length: 5\r\n"
      "Content-Type: text/plain\r\n\r\n";
  auto parsed = ferry::detail::parse_request_head(head, request, limits);
  CHECK(parsed.ok);
  CHECK(request.method == ferry::Method::Post);
  CHECK_EQ(request.path, "/echo");
  CHECK_EQ(request.query_string, "a=1");
  CHECK_EQ(request.content_length, std::uint64_t{5});
  CHECK(!request.chunked);
  CHECK_EQ(parsed.head_length, head.size());

  ferry::Request chunked;
  std::string chunked_head =
      "POST / HTTP/1.1\r\nHost: example.com\r\nTransfer-Encoding: chunked\r\n\r\n";
  auto chunked_parsed = ferry::detail::parse_request_head(chunked_head, chunked, limits);
  CHECK(chunked_parsed.ok);
  CHECK(chunked.chunked);

  ferry::Request bad;
  auto no_host = ferry::detail::parse_request_head(
      "GET / HTTP/1.1\r\nUser-Agent: x\r\n\r\n", bad, limits);
  CHECK(!no_host.ok);
  CHECK_EQ(no_host.http_status, 400);

  ferry::Request smuggle;
  auto both = ferry::detail::parse_request_head(
      "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nTransfer-Encoding: chunked\r\n\r\n",
      smuggle, limits);
  CHECK(!both.ok);
  CHECK_EQ(both.http_status, 400);

  ferry::Request duplicate;
  auto conflicting = ferry::detail::parse_request_head(
      "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nContent-Length: 6\r\n\r\n", duplicate,
      limits);
  CHECK(!conflicting.ok);

  ferry::Request folded;
  auto obs_fold = ferry::detail::parse_request_head(
      "GET / HTTP/1.1\r\nHost: x\r\nX-Test: a\r\n b\r\n\r\n", folded, limits);
  CHECK(!obs_fold.ok);

  ferry::Request version;
  auto bad_version = ferry::detail::parse_request_head(
      "GET / HTTP/2.0\r\nHost: x\r\n\r\n", version, limits);
  CHECK(!bad_version.ok);
  CHECK_EQ(bad_version.http_status, 505);

  ferry::Response response;
  std::string response_head = "HTTP/1.1 204 No Content\r\nX-Test: 1\r\n\r\n";
  auto response_parsed = ferry::detail::parse_response_head(response_head, response, limits);
  CHECK(response_parsed.ok);
  CHECK_EQ(response.status, 204);
  CHECK_EQ(response.headers.get("x-test"), "1");
}

TEST(server_core) {
  ferry::Server server(test_config());
  server.get("/", [](const ferry::Request&) -> ferry::task<ferry::Response> {
    co_return ferry::Response::text("root");
  });
  server.get("/hello/{name}", [](const ferry::Request& request) -> ferry::task<ferry::Response> {
    co_return ferry::Response::text("hello " + std::string(request.param("name")));
  });
  server.post("/echo", [](const ferry::Request& request) -> ferry::task<ferry::Response> {
    co_return ferry::Response(200, request.body, request.header("content-type"));
  });
  server.get("/query", [](const ferry::Request& request) -> ferry::task<ferry::Response> {
    co_return ferry::Response::text(std::string(request.query("x")) + "|" +
                                    std::string(request.query("y")));
  });
  server.get("/headers", [](const ferry::Request& request) -> ferry::task<ferry::Response> {
    co_return ferry::Response::text(std::string(request.header("x-test")));
  });
  server.get("/cookie", [](const ferry::Request&) -> ferry::task<ferry::Response> {
    ferry::Response response = ferry::Response::text("cookies");
    response.set_cookie("session", "abc123");
    response.set_cookie("theme", "dark");
    co_return response;
  });
  server.get("/jar/set", [](const ferry::Request&) -> ferry::task<ferry::Response> {
    ferry::Response response = ferry::Response::text("set");
    ferry::CookieOptions scoped;
    scoped.path = "/jar";
    response.set_cookie("session", "jar-value", scoped);
    ferry::CookieOptions secure;
    secure.secure = true;
    response.set_cookie("secure-cookie", "secret", secure);
    co_return response;
  });
  server.get("/jar/check", [](const ferry::Request& request) -> ferry::task<ferry::Response> {
    co_return ferry::Response::text(std::string(request.cookie("session")) + "|" +
                                    std::string(request.cookie("secure-cookie")));
  });
  server.get("/other/jar-check",
             [](const ferry::Request& request) -> ferry::task<ferry::Response> {
               co_return ferry::Response::text(std::string(request.cookie("session")));
             });
  server.get("/jar/delete", [](const ferry::Request&) -> ferry::task<ferry::Response> {
    ferry::Response response = ferry::Response::text("deleted");
    ferry::CookieOptions expired;
    expired.path = "/jar";
    expired.has_max_age = true;
    expired.max_age_seconds = 0;
    response.set_cookie("session", "", expired);
    co_return response;
  });
  server.get("/read-cookie", [](const ferry::Request& request) -> ferry::task<ferry::Response> {
    co_return ferry::Response::text(std::string(request.cookie("session")));
  });
  server.post("/form", [](const ferry::Request& request) -> ferry::task<ferry::Response> {
    co_return ferry::Response::text(std::string(request.form("a")) + "|" +
                                    std::string(request.form("b")));
  });
  server.get("/trailers", [](const ferry::Request&) -> ferry::task<ferry::Response> {
    ferry::Response response = ferry::Response::text("trailer body");
    response.trailers.set("x-checksum", "abc123");
    response.trailers.set("x-note", "done");
    co_return response;
  });
  server.post("/request-trailers",
              [](const ferry::Request& request) -> ferry::task<ferry::Response> {
                co_return ferry::Response::text(request.body + "|" +
                                                std::string(request.trailers.get("x-tag")));
              });
  server.post("/multipart", [](const ferry::Request& request) -> ferry::task<ferry::Response> {
    const auto& multipart = request.multipart();
    const auto* title = request.part("title");
    const auto* file = request.part("file");
    if (!multipart.ok() || title == nullptr || file == nullptr) {
      co_return ferry::Response::bad_request(multipart.error.empty() ? "missing parts"
                                                                    : multipart.error);
    }
    co_return ferry::Response::text(std::to_string(multipart.parts.size()) + "|" + title->body +
                                    "|" + file->filename + "|" + file->content_type + "|" +
                                    file->body);
  });
  server.get("/redirect", [](const ferry::Request&) -> ferry::task<ferry::Response> {
    co_return ferry::Response::redirect("/landed", 302);
  });
  server.get("/landed", [](const ferry::Request&) -> ferry::task<ferry::Response> {
    co_return ferry::Response::text("landed");
  });
  server.get("/bad-header", [](const ferry::Request&) -> ferry::task<ferry::Response> {
    ferry::Response response = ferry::Response::text("nope");
    response.set("x-bad", "a\r\ninjected: yes");
    co_return response;
  });
  server.get("/raw-bad-header", [](const ferry::Request&) -> ferry::task<ferry::Response> {
    ferry::Response response = ferry::Response::text("nope");
    response.headers.entries().emplace_back("x-raw", "a\r\ninjected: yes");
    co_return response;
  });
  server.start();

  run_async([&]() -> ferry::task<void> {
    ferry::Client client;
    auto base = base_url(server);

    auto root = co_await client.get(base + "/");
    CHECK_EQ(root.status, 200);
    CHECK_EQ(root.body.view(), "root");

    auto hello = co_await client.get(base + "/hello/ada%20lovelace");
    CHECK_EQ(hello.status, 200);
    CHECK_EQ(std::string(hello.body.view()), "hello ada lovelace");

    auto query = co_await client.get(base + "/query?x=1&y=hello+world");
    CHECK_EQ(std::string(query.body.view()), "1|hello+world");

    auto echo = co_await client.post(base + "/echo", "payload", "text/plain");
    CHECK_EQ(echo.status, 200);
    CHECK_EQ(std::string(echo.body.view()), "payload");
    CHECK_EQ(echo.header("content-type"), "text/plain");

    auto head = co_await client.head(base + "/hello/head");
    CHECK_EQ(head.status, 200);
    CHECK(head.body.view().empty());
    CHECK_EQ(head.header("content-length"), "10");

    auto missing = co_await client.get(base + "/missing");
    CHECK_EQ(missing.status, 404);

    auto wrong_method = co_await client.del(base + "/");
    CHECK_EQ(wrong_method.status, 405);
    CHECK(contains(std::string(wrong_method.header("allow")), "GET"));

    auto options = co_await client.send(ferry::Method::Options, base + "/");
    CHECK_EQ(options.status, 204);
    CHECK(contains(std::string(options.header("allow")), "GET"));

    auto cookies = co_await client.get(base + "/cookie");
    CHECK_EQ(cookies.headers.get_all("set-cookie").size(), std::size_t{2});

    ferry::Headers cookie_headers;
    cookie_headers.set("cookie", "session=abc123; theme=dark");
    auto cookie_value = co_await client.get(base + "/read-cookie", cookie_headers);
    CHECK_EQ(std::string(cookie_value.body.view()), "abc123");

    ferry::Headers form_headers;
    form_headers.set("content-type", "application/x-www-form-urlencoded");
    auto form = co_await client.post(base + "/form", "a=1&b=hello+world",
                                     "application/x-www-form-urlencoded");
    CHECK_EQ(std::string(form.body.view()), "1|hello world");

    client.clear_cookies();
    auto jar_set = co_await client.get(base + "/jar/set");
    CHECK_EQ(jar_set.status, 200);
    auto jar_check = co_await client.get(base + "/jar/check");
    CHECK_EQ(std::string(jar_check.body.view()), "jar-value|");
    auto other_path = co_await client.get(base + "/other/jar-check");
    CHECK_EQ(other_path.body.view(), "");
    auto deleted = co_await client.get(base + "/jar/delete");
    CHECK_EQ(deleted.status, 200);
    auto after_delete = co_await client.get(base + "/jar/check");
    CHECK_EQ(std::string(after_delete.body.view()), "|");
    client.clear_cookies();
    CHECK_EQ(client.cookies().size(), std::size_t{0});

    auto trailer_response = co_await client.get(base + "/trailers");
    CHECK_EQ(trailer_response.status, 200);
    CHECK_EQ(std::string(trailer_response.body.view()), "trailer body");
    CHECK_EQ(trailer_response.trailers.get("x-checksum"), "abc123");
    CHECK_EQ(trailer_response.trailers.get("x-note"), "done");

    const std::string boundary = "XyZ";
    std::string multipart_body =
        "--XyZ\r\nContent-Disposition: form-data; name=\"title\"\r\n\r\nhello\r\n"
        "--XyZ\r\nContent-Disposition: form-data; name=\"file\"; filename=\"a.txt\"\r\n"
        "Content-Type: text/plain\r\n\r\nfile body\r\n--XyZ--\r\n";
    auto multipart_response = co_await client.post(
        base + "/multipart", multipart_body,
        "multipart/form-data; boundary=" + boundary);
    CHECK_EQ(multipart_response.status, 200);
    CHECK_EQ(std::string(multipart_response.body.view()), "2|hello|a.txt|text/plain|file body");

    ferry::Headers custom;
    custom.set("x-test", "custom-value");
    auto echoed = co_await client.get(base + "/headers", custom);
    CHECK_EQ(std::string(echoed.body.view()), "custom-value");

    auto redirected = co_await client.get(base + "/redirect");
    CHECK_EQ(redirected.status, 200);
    CHECK_EQ(std::string(redirected.body.view()), "landed");

    auto bad_header = co_await client.get(base + "/bad-header");
    CHECK_EQ(bad_header.status, 500);

    auto raw_bad_header = co_await client.get(base + "/raw-bad-header");
    CHECK_EQ(raw_bad_header.status, 500);

    // Exercise keep-alive and the idle pool.
    for (int i = 0; i < 5; ++i) {
      auto response = co_await client.get(base + "/");
      CHECK_EQ(response.status, 200);
    }
  });
  server.stop();
}

TEST(streaming_and_gzip) {
  auto config = test_config();
  config.gzip = true;
  ferry::Server server(config);

  server.get("/stream", [](const ferry::Request&) -> ferry::task<ferry::Response> {
    auto counter = std::make_shared<int>(0);
    co_return ferry::Response::stream(
        "text/plain", [counter](std::string& chunk) -> ferry::task<bool> {
          if (*counter >= 3) co_return false;
          ++*counter;
          chunk = "chunk" + std::to_string(*counter);
          co_return true;
        });
  });
  server.get("/big", [](const ferry::Request&) -> ferry::task<ferry::Response> {
    co_return ferry::Response::text(std::string(4096, 'a') + "tail");
  });
  server.post("/gunzip", [](const ferry::Request& request) -> ferry::task<ferry::Response> {
    co_return ferry::Response::text(request.body);
  });
  server.start();

  run_async([&]() -> ferry::task<void> {
    ferry::Client client;
    auto base = base_url(server);

    auto streamed = co_await client.get(base + "/stream");
    CHECK_EQ(streamed.status, 200);
    CHECK_EQ(std::string(streamed.body.view()), "chunk1chunk2chunk3");

    auto streamed_head = co_await client.head(base + "/stream");
    CHECK_EQ(streamed_head.status, 200);
    CHECK(streamed_head.body.view().empty());
    CHECK(streamed_head.header("content-length").empty());
    CHECK(streamed_head.header("transfer-encoding").empty());

    auto after_head = co_await client.get(base + "/stream");
    CHECK_EQ(std::string(after_head.body.view()), "chunk1chunk2chunk3");

    auto big = co_await client.get(base + "/big");
    CHECK_EQ(big.status, 200);
    CHECK_EQ(big.body.view().size(), std::size_t{4100});
    CHECK_EQ(std::string(big.body.view().substr(4096)), "tail");

#if defined(FERRY_HAS_ZLIB)
    auto compressed = ferry::detail::gzip_compress(std::string(2048, 'z'));
    CHECK(!compressed.empty());
    ferry::Headers headers;
    headers.set("content-encoding", "gzip");
    auto echoed = co_await client.send(ferry::Method::Post, base + "/gunzip",
                                       ferry::Body(std::move(compressed)), headers);
    CHECK_EQ(echoed.status, 200);
    CHECK_EQ(echoed.body.view().size(), std::size_t{2048});
    CHECK_EQ(std::string(echoed.body.view()), std::string(2048, 'z'));

    auto raw = co_await raw_request(
        server.port(),
        "GET /big HTTP/1.1\r\nHost: x\r\nAccept-Encoding: gzip\r\nConnection: close\r\n\r\n");
    CHECK(contains(lower(raw), "content-encoding: gzip"));
#endif
  });
  server.stop();
}

TEST(proxy_forwarding) {
  ferry::Server proxy(test_config());
  proxy.any("/{*path}", [](const ferry::Request& request) -> ferry::task<ferry::Response> {
    co_return ferry::Response::text(std::string(request.target) + "|" +
                                    std::string(request.header("host")) + "|" +
                                    std::string(request.header("proxy-authorization")));
  });
  proxy.start();

  run_async([&]() -> ferry::task<void> {
    ferry::ClientConfig config;
    config.proxy = "http://user:pass@127.0.0.1:" + std::to_string(proxy.port());
    ferry::Client client(config);

    auto first = co_await client.get("http://example.invalid/thing?q=1");
    CHECK_EQ(first.status, 200);
    CHECK_EQ(std::string(first.body.view()),
             "http://example.invalid/thing?q=1|example.invalid|Basic dXNlcjpwYXNz");

    auto second = co_await client.get("http://other.invalid/");
    CHECK_EQ(second.status, 200);
    CHECK_EQ(std::string(second.body.view()),
             "http://other.invalid/|other.invalid|Basic dXNlcjpwYXNz");
  });
  proxy.stop();
}

TEST(proxy_connect_tls) {
  auto config = test_config();
  config.tls = ferry::TlsConfig{std::string(FERRY_TEST_CERT_DIR) + "/localhost.crt",
                                std::string(FERRY_TEST_CERT_DIR) + "/localhost.key"};
  ferry::Server server(config);
  server.get("/", [](const ferry::Request&) -> ferry::task<ferry::Response> {
    co_return ferry::Response::text("through tunnel");
  });
  server.start();

  MiniProxy proxy;

  run_async([&]() -> ferry::task<void> {
    ferry::ClientConfig client_config;
    client_config.ca_file = std::string(FERRY_TEST_CERT_DIR) + "/localhost.crt";
    client_config.proxy = "http://127.0.0.1:" + std::to_string(proxy.port());
    ferry::Client client(client_config);
    auto response = co_await client.get("https://localhost:" + std::to_string(server.port()) + "/");
    CHECK_EQ(response.status, 200);
    CHECK_EQ(std::string(response.body.view()), "through tunnel");
  });
  server.stop();
}

TEST(async_dns) {
  ferry::Server server(test_config());
  server.get("/", [](const ferry::Request&) -> ferry::task<ferry::Response> {
    co_return ferry::Response::text("dns");
  });
  server.start();

  run_async([&]() -> ferry::task<void> {
    auto resolved = co_await ferry::resolve_async(
        "localhost", server.port(), ferry::Deadline::after(2000ms));
    CHECK(resolved.ok());
    CHECK(!resolved.addresses.empty());

    ferry::Client client;
    auto response = co_await client.get("http://localhost:" + std::to_string(server.port()) + "/");
    CHECK_EQ(response.status, 200);
    CHECK_EQ(response.body.view(), "dns");

    auto expired = co_await ferry::resolve_async(
        "localhost", server.port(), ferry::Deadline::after(0ms));
    CHECK(!expired.ok());
    CHECK(expired.error.find("timed out") != std::string::npos);
  });
  server.stop();
}

TEST(large_body) {
  auto config = test_config();
  config.max_body_bytes = 8 * 1024 * 1024;
  ferry::Server server(config);
  server.post("/echo", [](const ferry::Request& request) -> ferry::task<ferry::Response> {
    co_return ferry::Response(200, request.body);
  });
  server.start();

  run_async([&]() -> ferry::task<void> {
    ferry::Client client;
    std::string payload(2 * 1024 * 1024, 'x');
    payload += "end";
    auto response = co_await client.post(base_url(server) + "/echo", payload);
    CHECK_EQ(response.status, 200);
    CHECK_EQ(response.body.view().size(), payload.size());
    CHECK_EQ(std::string(response.body.view().substr(payload.size() - 3)), "end");
  });
  server.stop();
}

TEST(client_timeout_and_server_timeout) {
  auto config = test_config();
  config.header_timeout = 200ms;
  ferry::Server server(config);
  server.get("/slow", [](const ferry::Request&) -> ferry::task<ferry::Response> {
    co_await ferry::sleep_for(400ms);
    co_return ferry::Response::text("late");
  });
  server.start();

  run_async([&]() -> ferry::task<void> {
    ferry::ClientConfig client_config;
    client_config.timeout = 100ms;
    ferry::Client client(client_config);
    auto response = co_await client.get(base_url(server) + "/slow");
    CHECK_EQ(response.status, 0);
    CHECK(!response.error.empty());
  });

  run_async([&]() -> ferry::task<void> {
    auto connected =
        co_await ferry::connect_tcp("127.0.0.1", server.port(), ferry::Deadline::after(1000ms));
    CHECK(connected.socket.valid());
    ferry::PlainStream stream(std::move(connected.socket));
    std::string buffer;
    auto head = co_await ferry::detail::read_head(stream, buffer, 65536,
                                                  ferry::Deadline::after(1000ms));
    CHECK(head.status == ferry::detail::ReadStatus::Timeout ||
          head.status == ferry::detail::ReadStatus::Eof ||
          (head.status == ferry::detail::ReadStatus::Ok && buffer.starts_with("HTTP/1.1 408")));
  });
  server.stop();
}

TEST(static_files) {
  TempDir temp;
  write_file(temp.path / "index.html", "<h1>index</h1>");
  write_file(temp.path / "hello.txt", "hello world");
  write_file(temp.path / "a b.txt", "spaced");
  write_file(temp.path / "sub" / "nested.txt", "nested");
  write_file(temp.path / ".hidden", "secret");
  write_file(temp.path.parent_path() / "ferry-outside.txt", "outside");

  ferry::Server server(test_config());
  server.static_files("/static", temp.path.string());
  server.start();

  run_async([&]() -> ferry::task<void> {
    ferry::Client client;
    auto base = base_url(server) + "/static";

    auto hello = co_await client.get(base + "/hello.txt");
    CHECK_EQ(hello.status, 200);
    CHECK_EQ(std::string(hello.body.view()), "hello world");
    CHECK_EQ(hello.header("content-type"), "text/plain; charset=utf-8");
    CHECK_EQ(hello.header("accept-ranges"), "bytes");

    auto head = co_await client.head(base + "/hello.txt");
    CHECK_EQ(head.status, 200);
    CHECK_EQ(head.header("content-length"), "11");
    CHECK(head.body.view().empty());

    std::string etag(head.header("etag"));
    CHECK(!etag.empty());
    ferry::Headers conditional;
    conditional.set("if-none-match", etag);
    auto not_modified = co_await client.get(base + "/hello.txt", conditional);
    CHECK_EQ(not_modified.status, 304);
    CHECK(not_modified.body.view().empty());

    ferry::Headers range;
    range.set("range", "bytes=0-4");
    auto partial = co_await client.get(base + "/hello.txt", range);
    CHECK_EQ(partial.status, 206);
    CHECK_EQ(std::string(partial.body.view()), "hello");
    CHECK_EQ(partial.header("content-range"), "bytes 0-4/11");

    ferry::Headers suffix;
    suffix.set("range", "bytes=-5");
    auto suffix_response = co_await client.get(base + "/hello.txt", suffix);
    CHECK_EQ(suffix_response.status, 206);
    CHECK_EQ(std::string(suffix_response.body.view()), "world");

    ferry::Headers invalid_range;
    invalid_range.set("range", "bytes=100-");
    auto unsatisfiable = co_await client.get(base + "/hello.txt", invalid_range);
    CHECK_EQ(unsatisfiable.status, 416);

    auto last_modified = std::string(hello.header("last-modified"));
    CHECK(!last_modified.empty());
    ferry::Headers modified_since;
    modified_since.set("if-modified-since", last_modified);
    auto not_modified_since = co_await client.get(base + "/hello.txt", modified_since);
    CHECK_EQ(not_modified_since.status, 304);

    ferry::Headers wrong_if_range;
    wrong_if_range.set("range", "bytes=0-4");
    wrong_if_range.set("if-range", "\"wrong-etag\"");
    auto full_response = co_await client.get(base + "/hello.txt", wrong_if_range);
    CHECK_EQ(full_response.status, 200);
    CHECK_EQ(std::string(full_response.body.view()), "hello world");

    ferry::Headers right_if_range;
    right_if_range.set("range", "bytes=0-4");
    right_if_range.set("if-range", etag);
    auto ranged_response = co_await client.get(base + "/hello.txt", right_if_range);
    CHECK_EQ(ranged_response.status, 206);
    CHECK_EQ(std::string(ranged_response.body.view()), "hello");

    ferry::Headers multi_range;
    multi_range.set("range", "bytes=0-4,6-10");
    auto multi = co_await client.get(base + "/hello.txt", multi_range);
    CHECK_EQ(multi.status, 206);
    CHECK(multi.header("content-type").starts_with("multipart/byteranges"));
    CHECK(contains(std::string(multi.body.view()), "hello"));
    CHECK(contains(std::string(multi.body.view()), "world"));
    CHECK(contains(std::string(multi.body.view()), "content-range: bytes 0-4/11"));
    CHECK(contains(std::string(multi.body.view()), "content-range: bytes 6-10/11"));

    auto index = co_await client.get(base + "/");
    CHECK_EQ(index.status, 200);
    CHECK_EQ(std::string(index.body.view()), "<h1>index</h1>");

    auto nested = co_await client.get(base + "/sub/nested.txt");
    CHECK_EQ(std::string(nested.body.view()), "nested");

    auto spaced = co_await client.get(base + "/a%20b.txt");
    CHECK_EQ(std::string(spaced.body.view()), "spaced");

    auto traversal = co_await client.get(base + "/%2e%2e/ferry-outside.txt");
    CHECK_EQ(traversal.status, 404);

    auto hidden = co_await client.get(base + "/.hidden");
    CHECK_EQ(hidden.status, 404);

    auto missing = co_await client.get(base + "/missing.txt");
    CHECK_EQ(missing.status, 404);
  });
  server.stop();
}

TEST(raw_protocol_security) {
  auto config = test_config();
  config.max_body_bytes = 1024;
  ferry::Server server(config);
  server.post("/echo", [](const ferry::Request& request) -> ferry::task<ferry::Response> {
    co_return ferry::Response(200, request.body);
  });
  server.get("/", [](const ferry::Request&) -> ferry::task<ferry::Response> {
    co_return ferry::Response::text("ok");
  });
  server.post("/request-trailers",
              [](const ferry::Request& request) -> ferry::task<ferry::Response> {
                co_return ferry::Response::text(request.body + "|" +
                                                std::string(request.trailers.get("x-tag")));
              });
  server.start();

  run_async([&]() -> ferry::task<void> {
    auto smuggle = co_await raw_request(
        server.port(),
        "POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n"
        "Transfer-Encoding: chunked\r\nConnection: close\r\n\r\n");
    CHECK(contains(smuggle, " 400 "));

    auto conflicting = co_await raw_request(
        server.port(),
        "POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nContent-Length: 6\r\n"
        "Connection: close\r\n\r\n");
    CHECK(contains(conflicting, " 400 "));

    auto bad_header = co_await raw_request(
        server.port(),
        "GET / HTTP/1.1\r\nHost: x\r\nBad Header: value\r\nConnection: close\r\n\r\n");
    CHECK(contains(bad_header, " 400 "));

    auto missing_host = co_await raw_request(server.port(),
                                             "GET / HTTP/1.1\r\nConnection: close\r\n\r\n");
    CHECK(contains(missing_host, " 400 "));

    auto bare_lf = co_await raw_request(
        server.port(), "GET / HTTP/1.1\nHost: x\r\nConnection: close\r\n\r\n");
    CHECK(contains(bare_lf, " 400 ") || contains(bare_lf, " 431 "));

    auto bad_version = co_await raw_request(
        server.port(), "GET / HTTP/2.0\r\nHost: x\r\nConnection: close\r\n\r\n");
    CHECK(contains(bad_version, " 505 "));

    auto http10 = co_await raw_request(server.port(), "GET / HTTP/1.0\r\n\r\n");
    CHECK(http10.starts_with("HTTP/1.0 200"));

    auto unknown_method = co_await raw_request(
        server.port(),
        "BREW / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
    CHECK(contains(unknown_method, " 501 "));

    auto oversized = co_await raw_request(
        server.port(),
        "POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 2048\r\nConnection: close\r\n\r\n");
    CHECK(contains(oversized, " 413 "));

    auto chunked = co_await raw_request(
        server.port(),
        "POST /echo HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n"
        "Connection: close\r\n\r\n5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n");
    CHECK(contains(chunked, "hello world"));

    auto trailers = co_await raw_request(
        server.port(),
        "POST /request-trailers HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n"
        "Connection: close\r\n\r\n5\r\nhello\r\n0\r\nx-tag: tail\r\n\r\n");
    CHECK(contains(trailers, "hello|tail"));

    auto bad_chunk = co_await raw_request(
        server.port(),
        "POST /echo HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n"
        "Connection: close\r\n\r\nzz\r\nhello\r\n0\r\n\r\n");
    CHECK(contains(bad_chunk, " 400 "));

    // Expect: 100-continue.
    auto connected =
        co_await ferry::connect_tcp("127.0.0.1", server.port(), ferry::Deadline::after(2000ms));
    CHECK(connected.socket.valid());
    ferry::PlainStream stream(std::move(connected.socket));
    std::string buffer;
    auto written = co_await ferry::write_all(
        stream,
        "POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nExpect: 100-continue\r\n"
        "Connection: close\r\n\r\n",
        ferry::Deadline::after(2000ms));
    CHECK(written.ok());
    auto interim = co_await ferry::detail::read_head(stream, buffer, 65536,
                                                     ferry::Deadline::after(2000ms));
    CHECK(interim.status == ferry::detail::ReadStatus::Ok);
    CHECK(buffer.starts_with("HTTP/1.1 100"));
    buffer.clear();
    written = co_await ferry::write_all(stream, "hello", ferry::Deadline::after(2000ms));
    CHECK(written.ok());
    auto final_head = co_await ferry::detail::read_head(stream, buffer, 65536,
                                                        ferry::Deadline::after(2000ms));
    CHECK(final_head.status == ferry::detail::ReadStatus::Ok);
    CHECK(buffer.starts_with("HTTP/1.1 200"));
  });
  server.stop();
}

TEST(tls) {
  auto config = test_config();
  config.tls = ferry::TlsConfig{std::string(FERRY_TEST_CERT_DIR) + "/localhost.crt",
                                std::string(FERRY_TEST_CERT_DIR) + "/localhost.key"};
  ferry::Server server(config);
  server.get("/", [](const ferry::Request& request) -> ferry::task<ferry::Response> {
    co_return ferry::Response::text("secure=" + std::string(request.secure ? "yes" : "no") +
                                    ";tls=" + request.tls_version);
  });
  server.start();

  run_async([&]() -> ferry::task<void> {
    ferry::ClientConfig verified;
    verified.ca_file = std::string(FERRY_TEST_CERT_DIR) + "/localhost.crt";
    ferry::Client client(verified);
    auto url = "https://localhost:" + std::to_string(server.port()) + "/";
    auto response = co_await client.get(url);
    CHECK_EQ(response.status, 200);
    CHECK(contains(std::string(response.body.view()), "secure=yes"));
    CHECK(contains(std::string(response.body.view()), "TLS"));

    ferry::ClientConfig insecure;
    insecure.verify_tls = false;
    ferry::Client insecure_client(insecure);
    auto insecure_response = co_await insecure_client.get(url);
    CHECK_EQ(insecure_response.status, 200);

    ferry::Client trusting_default_store;
    auto untrusted = co_await trusting_default_store.get(url);
    CHECK_EQ(untrusted.status, 0);
    CHECK(!untrusted.error.empty());
  });
  server.stop();
}

TEST(mutual_tls) {
  auto config = test_config();
  config.tls = ferry::TlsConfig{std::string(FERRY_TEST_CERT_DIR) + "/localhost.crt",
                                std::string(FERRY_TEST_CERT_DIR) + "/localhost.key"};
  config.tls->ca_file = std::string(FERRY_TEST_CERT_DIR) + "/localhost.crt";
  config.tls->require_client_certificate = true;

  ferry::Server server(config);
  server.get("/", [](const ferry::Request&) -> ferry::task<ferry::Response> {
    co_return ferry::Response::text("mtls ok");
  });
  server.start();

  run_async([&]() -> ferry::task<void> {
    auto url = "https://localhost:" + std::to_string(server.port()) + "/";

    ferry::ClientConfig with_certificate;
    with_certificate.ca_file = std::string(FERRY_TEST_CERT_DIR) + "/localhost.crt";
    with_certificate.client_certificate_file = std::string(FERRY_TEST_CERT_DIR) + "/localhost.crt";
    with_certificate.client_private_key_file = std::string(FERRY_TEST_CERT_DIR) + "/localhost.key";
    ferry::Client authenticated(with_certificate);
    auto response = co_await authenticated.get(url);
    CHECK_EQ(response.status, 200);
    CHECK_EQ(std::string(response.body.view()), "mtls ok");

    ferry::ClientConfig without_certificate;
    without_certificate.ca_file = std::string(FERRY_TEST_CERT_DIR) + "/localhost.crt";
    ferry::Client anonymous_client(without_certificate);
    auto rejected = co_await anonymous_client.get(url);
    CHECK_EQ(rejected.status, 0);
  });
  server.stop();
}

TEST(redirects_preserve_methods) {
  ferry::Server server(test_config());
  server.post("/submit", [](const ferry::Request&) -> ferry::task<ferry::Response> {
    co_return ferry::Response::redirect("/done", 303);
  });
  server.get("/done", [](const ferry::Request& request) -> ferry::task<ferry::Response> {
    co_return ferry::Response::text(std::string(ferry::method_name(request.method)));
  });
  server.post("/keep", [](const ferry::Request&) -> ferry::task<ferry::Response> {
    co_return ferry::Response::redirect("/echo", 307);
  });
  server.post("/echo", [](const ferry::Request& request) -> ferry::task<ferry::Response> {
    co_return ferry::Response::text(std::string(ferry::method_name(request.method)) + ":" +
                                    request.body);
  });
  server.start();

  run_async([&]() -> ferry::task<void> {
    ferry::Client client;
    auto base = base_url(server);

    auto see_other = co_await client.post(base + "/submit", "ignored");
    CHECK_EQ(see_other.status, 200);
    CHECK_EQ(std::string(see_other.body.view()), "GET");

    auto temporary = co_await client.post(base + "/keep", "kept");
    CHECK_EQ(temporary.status, 200);
    CHECK_EQ(std::string(temporary.body.view()), "POST:kept");
  });
  server.stop();
}

TEST(spawn_starts_jobs) {
  run_async([]() -> ferry::task<void> {
    int count = 0;
    for (int i = 0; i < 10; ++i) {
      ferry::spawn([&count]() -> ferry::task<void> {
        ++count;
        co_return;
      });
    }
    co_await ferry::sleep_for(10ms);
    CHECK_EQ(count, 10);
  });
}

TEST(concurrency) {
  auto config = test_config(4);
  ferry::Server server(config);
  server.get("/work/{id}", [](const ferry::Request& request) -> ferry::task<ferry::Response> {
    co_return ferry::Response::text(request.param("id"));
  });
  server.start();

  run_async([&]() -> ferry::task<void> {
    ferry::ClientConfig client_config;
    client_config.timeout = 5000ms;
    client_config.max_idle_per_host = 32;
    ferry::Client client(client_config);
    auto base = base_url(server);

    constexpr int kRequests = 100;
    std::atomic<int> remaining{kRequests};
    std::atomic<int> failures{0};

    for (int i = 0; i < kRequests; ++i) {
      ferry::spawn([&, i]() -> ferry::task<void> {
        std::string job_url = base + "/work/" + std::to_string(i);
        try {
          auto response = co_await client.get(job_url);
          if (response.status != 200 || response.body.view() != std::to_string(i)) {
            failures.fetch_add(1);
          }
        } catch (...) {
          failures.fetch_add(1);
        }
        remaining.fetch_sub(1);
      });
    }

    for (int i = 0; i < 1000 && remaining.load() > 0; ++i) {
      co_await ferry::sleep_for(5ms);
    }
    CHECK_EQ(remaining.load(), 0);
    CHECK_EQ(failures.load(), 0);
  });
  server.stop();
}

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  int failed = 0;
  const auto& tests = test_registry();
  for (const auto& test : tests) {
    try {
      test.fn();
      std::printf("ok   %s\n", test.name);
    } catch (const std::exception& e) {
      std::printf("FAIL %s: %s\n", test.name, e.what());
      ++failed;
    } catch (...) {
      std::printf("FAIL %s: unknown exception\n", test.name);
      ++failed;
    }
  }
  std::printf("\n%d/%zu tests passed\n", static_cast<int>(tests.size()) - failed, tests.size());
  return failed == 0 ? 0 : 1;
}
