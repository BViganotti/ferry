// ferry::Server — HTTP/1.1 server with TLS, routing, middleware, streaming
// responses and static files.

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "ferry/handler.hpp"
#include "ferry/http.hpp"
#include "ferry/tls.hpp"
#include "ferry/websocket.hpp"

namespace ferry {

struct StaticOptions {
  bool etag = true;
  bool dotfiles = false;
  std::string index = "index.html";
  std::chrono::seconds cache_max_age{0};
};

struct ServerConfig {
  std::string bind_address = "0.0.0.0";
  std::uint16_t port = 8080;
  int threads = 1;
  int backlog = 1024;

  std::string server_name = "ferry/0.1.0";
  std::size_t max_header_bytes = 64 * 1024;
  std::size_t max_body_bytes = 16 * 1024 * 1024;
  std::size_t max_headers = 100;
  std::size_t max_connections = 0;  // 0 = unlimited

  std::chrono::milliseconds header_timeout{10'000};
  std::chrono::milliseconds body_timeout{30'000};
  std::chrono::milliseconds idle_timeout{60'000};
  std::chrono::milliseconds write_timeout{30'000};
  std::chrono::milliseconds shutdown_timeout{5'000};

  bool tcp_nodelay = true;
  bool gzip = false;
  bool trust_proxy = false;

  std::optional<TlsConfig> tls;

  std::function<void(const Request&, const Response&, std::chrono::nanoseconds)> access_log;
};

class Server {
 public:
  explicit Server(ServerConfig config = {});
  ~Server();

  Server(const Server&) = delete;
  Server& operator=(const Server&) = delete;

  Server& get(std::string_view pattern, Handler handler);
  Server& post(std::string_view pattern, Handler handler);
  Server& put(std::string_view pattern, Handler handler);
  Server& patch(std::string_view pattern, Handler handler);
  Server& del(std::string_view pattern, Handler handler);
  Server& head(std::string_view pattern, Handler handler);
  Server& options(std::string_view pattern, Handler handler);
  Server& any(std::string_view pattern, Handler handler);
  Server& ws(std::string_view pattern, WsHandler handler);

  Server& use(Middleware middleware);
  Server& static_files(std::string_view prefix, std::string root,
                       StaticOptions options = StaticOptions{});
  Server& on_not_found(Handler handler);
  Server& on_error(ErrorHandler handler);

  // Binds, starts worker threads and returns. Throws std::runtime_error on
  // bind/TLS failure.
  void start();

  // start() + wait().
  void serve();

  // Graceful shutdown: stops accepting, closes idle keep-alive connections,
  // gives in-flight requests shutdown_timeout to finish, then closes the rest.
  // Joins worker threads. Safe to call once; must not be called from a handler.
  void stop();

  // Blocks until stop() completes.
  void wait();

  bool running() const noexcept;
  std::uint16_t port() const noexcept;
  const ServerConfig& config() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace ferry
