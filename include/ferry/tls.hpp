// ferry::TlsContext / TlsStream — OpenSSL-backed TLS for the Stream interface.
// OpenSSL types are hidden behind a pimpl so users of the library do not need
// OpenSSL headers on their include path.

#pragma once

#include <memory>
#include <string>
#include <string_view>

#include "ferry/net.hpp"

namespace ferry {

struct TlsConfig {
  std::string certificate_file;
  std::string private_key_file;
  std::string ca_file;  // for client-certificate verification
  bool require_client_certificate = false;
  std::string alpn = "http/1.1";
  int minimum_version = 12;  // 12 or 13
};

class TlsContext {
 public:
  ~TlsContext();
  TlsContext(const TlsContext&) = delete;
  TlsContext& operator=(const TlsContext&) = delete;

  struct Impl;

  static std::shared_ptr<TlsContext> make_server(const TlsConfig& config, std::string& error);
  static std::shared_ptr<TlsContext> make_client(bool verify, std::string_view ca_file,
                                                 std::string& error,
                                                 std::string_view certificate_file = {},
                                                 std::string_view private_key_file = {});

 private:
  explicit TlsContext(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;

  friend class TlsStream;
};

struct TlsResult {
  std::unique_ptr<Stream> stream;
  std::string error;
  bool ok() const noexcept { return stream != nullptr; }
};

class TlsStream final : public Stream {
 public:
  ~TlsStream() override;

  task<IoResult> read_some(std::span<std::byte> buffer, Deadline deadline) override;
  task<IoResult> write_some(std::span<const std::byte> buffer, Deadline deadline) override;
  task<void> shutdown(Deadline deadline) override;

  int fd() const noexcept override { return socket_.fd(); }
  bool is_tls() const noexcept override { return true; }
  std::string tls_version() const override;
  std::string peer_certificate_subject() const override;

  static task<TlsResult> accept(Socket socket, std::shared_ptr<TlsContext> context,
                                Deadline deadline);
  static task<TlsResult> connect(Socket socket, std::string host,
                                 std::shared_ptr<TlsContext> context, bool verify_hostname,
                                 Deadline deadline);

 private:
  TlsStream(Socket socket, std::shared_ptr<TlsContext> context, void* ssl);

  task<std::string> handshake(bool server, bool verify_hostname, const std::string& host,
                              Deadline deadline);

  Socket socket_;
  std::shared_ptr<TlsContext> context_;
  void* ssl_ = nullptr;
};

}  // namespace ferry
