#include "ferry/net.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdexcept>
#include <unistd.h>

#if defined(__APPLE__)
#include <sys/socket.h>
#endif

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

namespace ferry {

namespace {

int create_socket(int family) {
  int fd = ::socket(family, SOCK_STREAM, 0);
  if (fd < 0) return -1;
#if defined(SO_NOSIGPIPE)
  int one = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
  return fd;
}

bool set_nonblocking(int fd) {
  int flags = ::fcntl(fd, F_GETFL, 0);
  if (flags < 0) return false;
  return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

bool set_cloexec(int fd) {
  int flags = ::fcntl(fd, F_GETFD, 0);
  if (flags < 0) return false;
  return ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == 0;
}

std::string gai_error(int code) {
  return std::string(::gai_strerror(code));
}

}  // namespace

int Address::family() const noexcept {
  return storage.ss_family;
}

std::string Address::host() const {
  char buffer[INET6_ADDRSTRLEN];
  if (storage.ss_family == AF_INET) {
    const auto* address = reinterpret_cast<const sockaddr_in*>(&storage);
    if (::inet_ntop(AF_INET, &address->sin_addr, buffer, sizeof buffer)) return buffer;
  } else if (storage.ss_family == AF_INET6) {
    const auto* address = reinterpret_cast<const sockaddr_in6*>(&storage);
    if (::inet_ntop(AF_INET6, &address->sin6_addr, buffer, sizeof buffer)) return buffer;
  }
  return {};
}

uint16_t Address::port() const {
  if (storage.ss_family == AF_INET) {
    return ntohs(reinterpret_cast<const sockaddr_in*>(&storage)->sin_port);
  }
  if (storage.ss_family == AF_INET6) {
    return ntohs(reinterpret_cast<const sockaddr_in6*>(&storage)->sin6_port);
  }
  return 0;
}

std::string Address::port_string() const {
  return std::to_string(port());
}

std::string Address::to_string() const {
  auto h = host();
  auto p = port_string();
  if (family() == AF_INET6) return "[" + h + "]:" + p;
  return h + ":" + p;
}

std::optional<Address> Address::parse(std::string_view host, uint16_t port) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_NUMERICHOST | AI_NUMERICSERV;
  addrinfo* result = nullptr;
  std::string host_string(host);
  std::string port_text = std::to_string(port);
  int rc = ::getaddrinfo(host_string.c_str(), port_text.c_str(), &hints, &result);
  if (rc != 0 || !result) return std::nullopt;
  Address address;
  std::memcpy(&address.storage, result->ai_addr, result->ai_addrlen);
  address.length = static_cast<socklen_t>(result->ai_addrlen);
  ::freeaddrinfo(result);
  return address;
}

std::vector<Address> Address::resolve(std::string_view host, uint16_t port) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_ADDRCONFIG;
  addrinfo* result = nullptr;
  std::string host_string(host);
  std::string port_text = std::to_string(port);
  int rc = ::getaddrinfo(host_string.c_str(), port_text.c_str(), &hints, &result);
  if (rc != 0) return {};
  std::vector<Address> addresses;
  for (addrinfo* it = result; it != nullptr; it = it->ai_next) {
    Address address;
    std::memcpy(&address.storage, it->ai_addr, it->ai_addrlen);
    address.length = static_cast<socklen_t>(it->ai_addrlen);
    addresses.push_back(address);
  }
  ::freeaddrinfo(result);
  return addresses;
}

Socket::~Socket() { close(); }

Socket& Socket::operator=(Socket&& other) noexcept {
  if (this != &other) {
    close();
    fd_ = other.release();
  }
  return *this;
}

void Socket::close() noexcept {
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
}

int Socket::release() noexcept {
  int fd = fd_;
  fd_ = -1;
  return fd;
}

bool Socket::set_nonblocking() noexcept {
  return fd_ >= 0 && ::ferry::set_nonblocking(fd_);
}

bool Socket::set_nodelay() noexcept {
  if (fd_ < 0) return false;
  int one = 1;
  return ::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one) == 0;
}

bool Socket::set_keepalive() noexcept {
  if (fd_ < 0) return false;
  int one = 1;
  return ::setsockopt(fd_, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof one) == 0;
}

std::optional<Socket> Socket::accept(Address* peer) noexcept {
  if (fd_ < 0) return std::nullopt;
  sockaddr_storage storage{};
  socklen_t length = sizeof storage;
  int client = -1;
  do {
    client = ::accept(fd_, reinterpret_cast<sockaddr*>(&storage), &length);
  } while (client < 0 && errno == EINTR);

  if (client < 0) return std::nullopt;

  if (!::ferry::set_nonblocking(client) || !::ferry::set_cloexec(client)) {
    ::close(client);
    return std::nullopt;
  }
#if defined(SO_NOSIGPIPE)
  int one = 1;
  ::setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
  int nodelay = 1;
  ::setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof nodelay);
  ::setsockopt(client, SOL_SOCKET, SO_KEEPALIVE, &nodelay, sizeof nodelay);

  if (peer) {
    peer->storage = storage;
    peer->length = length;
  }
  return Socket(client);
}

ListenResult listen_tcp(std::string_view host, uint16_t port, int backlog, bool reuse_port) {
  ListenResult result;
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE;
  addrinfo* addresses = nullptr;
  std::string host_string(host);
  std::string port_text = std::to_string(port);
  int rc = ::getaddrinfo(host_string.empty() ? nullptr : host_string.c_str(), port_text.c_str(),
                         &hints, &addresses);
  if (rc != 0) {
    result.error = "cannot resolve bind address '" + host_string + "': " + gai_error(rc);
    return result;
  }

  std::string last_error = "no usable address";
  for (addrinfo* it = addresses; it != nullptr; it = it->ai_next) {
    int fd = create_socket(it->ai_family);
    if (fd < 0) {
      last_error = std::strerror(errno);
      continue;
    }

    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (reuse_port) {
      if (::setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one) != 0) {
        last_error = "SO_REUSEPORT unavailable: " + std::string(std::strerror(errno));
        ::close(fd);
        continue;
      }
    }
    if (!::ferry::set_nonblocking(fd) || !::ferry::set_cloexec(fd)) {
      last_error = std::strerror(errno);
      ::close(fd);
      continue;
    }

    if (::bind(fd, it->ai_addr, it->ai_addrlen) != 0) {
      last_error = "bind failed: " + std::string(std::strerror(errno));
      ::close(fd);
      continue;
    }
    if (::listen(fd, backlog) != 0) {
      last_error = "listen failed: " + std::string(std::strerror(errno));
      ::close(fd);
      continue;
    }

    result.socket = Socket(fd);
    result.error.clear();
    ::freeaddrinfo(addresses);
    return result;
  }

  ::freeaddrinfo(addresses);
  result.error = last_error;
  return result;
}

task<ConnectResult> connect_tcp(std::string_view host, uint16_t port, Deadline deadline) {
  ConnectResult result;
  EventLoop* loop = EventLoop::current();
  if (!loop) {
    result.error = "connect_tcp requires an event loop";
    co_return result;
  }

  auto resolved = co_await resolve_async(std::string(host), port, deadline);
  if (!resolved.ok()) {
    result.error = resolved.error.empty() ? "cannot resolve host '" + std::string(host) + "'"
                                          : resolved.error;
    co_return result;
  }

  std::string last_error = "connection failed";
  for (const auto& address : resolved.addresses) {
    if (deadline.expired()) {
      result.error = "connection timed out";
      co_return result;
    }

    int fd = create_socket(address.family());
    if (fd < 0) {
      last_error = std::strerror(errno);
      continue;
    }
    Socket socket(fd);
    if (!socket.set_nonblocking()) {
      last_error = std::strerror(errno);
      continue;
    }

    int rc = ::connect(fd, reinterpret_cast<const sockaddr*>(&address.storage), address.length);
    if (rc != 0 && errno != EINPROGRESS) {
      last_error = std::strerror(errno);
      continue;
    }

    if (rc != 0) {
      auto ready = co_await wait_writable(*loop, fd, deadline);
      if (!ready.ok()) {
        result.error = ready.timeout() ? "connection timed out" : ready.message();
        co_return result;
      }
      int so_error = 0;
      socklen_t length = sizeof so_error;
      if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &length) != 0) {
        last_error = std::strerror(errno);
        continue;
      }
      if (so_error != 0) {
        last_error = std::strerror(so_error);
        continue;
      }
    }

    socket.set_nodelay();
    socket.set_keepalive();
    result.socket = std::move(socket);
    result.error.clear();
    co_return result;
  }

  result.error = last_error;
  co_return result;
}

namespace {

IoResult make_error(int error) {
  IoResult result;
  result.status = IoResult::Status::Error;
  result.error = error;
  return result;
}

}  // namespace

task<IoResult> PlainStream::read_some(std::span<std::byte> buffer, Deadline deadline) {
  if (socket_.fd() < 0) co_return make_error(EBADF);
  if (buffer.empty()) co_return IoResult{};

  EventLoop* loop = EventLoop::current();
  if (!loop) co_return make_error(EINVAL);

  for (;;) {
    ssize_t n = ::recv(socket_.fd(), buffer.data(), buffer.size(), 0);
    if (n > 0) {
      IoResult result;
      result.n = static_cast<std::size_t>(n);
      co_return result;
    }
    if (n == 0) {
      IoResult result;
      result.status = IoResult::Status::Eof;
      co_return result;
    }
    if (errno == EINTR) continue;
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      auto ready = co_await wait_readable(*loop, socket_.fd(), deadline);
      if (!ready.ok()) co_return ready;
      continue;
    }
    co_return make_error(errno);
  }
}

task<IoResult> PlainStream::write_some(std::span<const std::byte> buffer, Deadline deadline) {
  if (socket_.fd() < 0) co_return make_error(EBADF);
  if (buffer.empty()) co_return IoResult{};

  EventLoop* loop = EventLoop::current();
  if (!loop) co_return make_error(EINVAL);

  for (;;) {
    ssize_t n = ::send(socket_.fd(), buffer.data(), buffer.size(), MSG_NOSIGNAL);
    if (n > 0) {
      IoResult result;
      result.n = static_cast<std::size_t>(n);
      co_return result;
    }
    if (n == 0) co_return make_error(EPIPE);
    if (errno == EINTR) continue;
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      auto ready = co_await wait_writable(*loop, socket_.fd(), deadline);
      if (!ready.ok()) co_return ready;
      continue;
    }
    co_return make_error(errno);
  }
}

task<void> PlainStream::shutdown(Deadline) {
  if (socket_.fd() >= 0) ::shutdown(socket_.fd(), SHUT_WR);
  co_return;
}

task<IoResult> write_all(Stream& stream, std::span<const std::byte> data, Deadline deadline) {
  std::size_t offset = 0;
  while (offset < data.size()) {
    auto result = co_await stream.write_some(data.subspan(offset), deadline);
    if (!result.ok()) co_return result;
    if (result.n == 0) co_return make_error(EPIPE);
    offset += result.n;
  }
  co_return IoResult{};
}

task<IoResult> write_all(Stream& stream, std::string_view data, Deadline deadline) {
  co_return co_await write_all(stream, as_bytes(data), deadline);
}

task<IoResult> read_exact(Stream& stream, std::span<std::byte> buffer, Deadline deadline) {
  std::size_t offset = 0;
  while (offset < buffer.size()) {
    auto result = co_await stream.read_some(buffer.subspan(offset), deadline);
    if (result.status == IoResult::Status::Eof) {
      result.n = offset;
      co_return result;
    }
    if (!result.ok()) co_return result;
    if (result.n == 0) co_return make_error(ECONNRESET);
    offset += result.n;
  }
  IoResult result;
  result.n = offset;
  co_return result;
}

}  // namespace ferry
