// ferry::net — non-blocking sockets, addresses and the Stream abstraction.

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <sys/socket.h>

#include "ferry/event_loop.hpp"

namespace ferry {

struct Address {
  sockaddr_storage storage{};
  socklen_t length = 0;

  int family() const noexcept;
  std::string host() const;
  std::string port_string() const;
  uint16_t port() const;
  std::string to_string() const;

  static std::optional<Address> parse(std::string_view host, uint16_t port);
  static std::vector<Address> resolve(std::string_view host, uint16_t port);
};

class Socket {
 public:
  Socket() noexcept = default;
  explicit Socket(int fd) noexcept : fd_(fd) {}
  ~Socket();

  Socket(Socket&& other) noexcept : fd_(other.release()) {}
  Socket& operator=(Socket&& other) noexcept;
  Socket(const Socket&) = delete;
  Socket& operator=(const Socket&) = delete;

  int fd() const noexcept { return fd_; }
  bool valid() const noexcept { return fd_ >= 0; }
  void close() noexcept;
  int release() noexcept;

  bool set_nonblocking() noexcept;
  bool set_nodelay() noexcept;
  bool set_keepalive() noexcept;

  std::optional<Socket> accept(Address* peer) noexcept;

 private:
  int fd_ = -1;
};

struct ListenResult {
  Socket socket;
  std::string error;
};

// Creates, binds and listens on a non-blocking TCP socket. When reuse_port is
// true, SO_REUSEPORT is required and an error is returned if unavailable.
ListenResult listen_tcp(std::string_view host, uint16_t port, int backlog, bool reuse_port);

struct ConnectResult {
  Socket socket;
  std::string error;
};

// Resolves host and connects asynchronously (the DNS lookup itself is
// synchronous; connect is non-blocking).
task<ConnectResult> connect_tcp(std::string_view host, uint16_t port, Deadline deadline);

// A byte stream. Reads and writes are single-shot: read_some may return fewer
// bytes, write_some may accept fewer bytes. Use write_all() to drain a buffer.
class Stream {
 public:
  virtual ~Stream() = default;

  virtual task<IoResult> read_some(std::span<std::byte> buffer, Deadline deadline) = 0;
  virtual task<IoResult> write_some(std::span<const std::byte> buffer, Deadline deadline) = 0;
  virtual task<void> shutdown(Deadline deadline) = 0;
  virtual int fd() const noexcept = 0;
  virtual bool is_tls() const noexcept { return false; }
  virtual std::string tls_version() const { return {}; }
  virtual std::string peer_certificate_subject() const { return {}; }
};

class PlainStream final : public Stream {
 public:
  explicit PlainStream(Socket socket) : socket_(std::move(socket)) {}

  task<IoResult> read_some(std::span<std::byte> buffer, Deadline deadline) override;
  task<IoResult> write_some(std::span<const std::byte> buffer, Deadline deadline) override;
  task<void> shutdown(Deadline deadline) override;
  int fd() const noexcept override { return socket_.fd(); }

 private:
  Socket socket_;
};

// Writes all bytes or returns the first error/timeout.
task<IoResult> write_all(Stream& stream, std::span<const std::byte> data, Deadline deadline);
task<IoResult> write_all(Stream& stream, std::string_view data, Deadline deadline);

// Reads exactly buffer.size() bytes, or returns Eof/Timeout/Error.
task<IoResult> read_exact(Stream& stream, std::span<std::byte> buffer, Deadline deadline);

}  // namespace ferry
