// ferry::Client — coroutine HTTP/1.1 client with connection pooling and TLS.

#pragma once

#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>

#include "ferry/cookie_jar.hpp"
#include "ferry/http.hpp"
#include "ferry/url.hpp"

namespace ferry {

struct ClientConfig {
  std::chrono::milliseconds connect_timeout{10'000};
  std::chrono::milliseconds timeout{30'000};
  std::chrono::milliseconds idle_timeout{60'000};

  std::size_t max_response_body = 64 * 1024 * 1024;
  std::size_t max_header_bytes = 64 * 1024;

  bool follow_redirects = true;
  int max_redirects = 5;
  bool verify_tls = true;
  std::string ca_file;

  // Optional forward proxy. Supports http://[user:pass@]host:port.
  // Plain HTTP requests use absolute-form; HTTPS requests use CONNECT.
  std::string proxy;
  std::string client_certificate_file;  // optional mTLS client certificate
  std::string client_private_key_file;

  bool accept_gzip = true;
  bool reuse_connections = true;
  bool use_cookies = true;
  std::size_t max_idle_per_host = 8;

  std::string user_agent = "ferry/0.1.0";
  Headers default_headers;
};

class Client {
 public:
  explicit Client(ClientConfig config = ClientConfig{});
  ~Client();

  Client(Client&&) noexcept;
  Client& operator=(Client&&) noexcept;
  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;

  task<Response> get(std::string_view url, Headers headers = {});
  task<Response> head(std::string_view url, Headers headers = {});
  task<Response> post(std::string_view url, std::string body,
                      std::string content_type = "text/plain; charset=utf-8");
  task<Response> put(std::string_view url, std::string body,
                     std::string content_type = "text/plain; charset=utf-8");
  task<Response> patch(std::string_view url, std::string body,
                       std::string content_type = "text/plain; charset=utf-8");
  task<Response> del(std::string_view url, Headers headers = {});

  // Full control. Returns a Response with status 0 and `error` set if the
  // request could not be completed at the transport level.
  task<Response> send(Method method, std::string_view url, Body body = Body{},
                      Headers headers = {});

  // Closes all idle pooled connections.
  void close();

  // Cookie jar used for automatic Cookie / Set-Cookie handling.
  CookieJar& cookies();
  void clear_cookies();

  const ClientConfig& config() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace ferry
