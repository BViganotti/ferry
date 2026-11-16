#include "ferry/tls.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <mutex>
#include <string>

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

namespace ferry {

namespace {

void init_openssl() {
  static std::once_flag once;
  std::call_once(once, [] {
    OPENSSL_init_ssl(0, nullptr);
    // OpenSSL writes through a plain socket on some platforms, bypassing
    // MSG_NOSIGNAL. Ignoring SIGPIPE is what most network libraries do; the
    // send paths still return EPIPE and are reported as errors.
    std::signal(SIGPIPE, SIG_IGN);
  });
}

std::string ssl_error_queue() {
  std::string result;
  unsigned long code = 0;
  char buffer[256];
  while ((code = ERR_get_error()) != 0) {
    ERR_error_string_n(code, buffer, sizeof buffer);
    if (!result.empty()) result += "; ";
    result += buffer;
  }
  return result;
}

std::string ssl_error_with(const char* what) {
  auto detail = ssl_error_queue();
  if (detail.empty()) return what;
  return std::string(what) + ": " + detail;
}

std::string errno_text(int error) {
  if (error == 0) return {};
  return std::strerror(error);
}

int minimum_version(int configured) {
  switch (configured) {
    case 13:
      return TLS1_3_VERSION;
    case 12:
    default:
      return TLS1_2_VERSION;
  }
}

bool is_ip_address(const std::string& host) {
  unsigned char buffer[16];
  return ::inet_pton(AF_INET, host.c_str(), buffer) == 1 ||
         ::inet_pton(AF_INET6, host.c_str(), buffer) == 1;
}

int alpn_select_callback(SSL*, const unsigned char** out, unsigned char* out_len,
                         const unsigned char* in, unsigned int in_len, void* arg) {
  const auto* wanted = static_cast<const std::string*>(arg);
  if (!wanted || wanted->empty()) return SSL_TLSEXT_ERR_NOACK;

  unsigned int index = 0;
  while (index < in_len) {
    unsigned int length = in[index];
    if (index + 1 + length > in_len) break;
    std::string_view offered(reinterpret_cast<const char*>(in + index + 1), length);
    if (offered == *wanted) {
      *out = in + index + 1;
      *out_len = static_cast<unsigned char>(length);
      return SSL_TLSEXT_ERR_OK;
    }
    index += 1 + length;
  }
  return SSL_TLSEXT_ERR_NOACK;
}

IoResult tls_io_error(int ssl_error, const char* operation) {
  IoResult result;
  result.status = IoResult::Status::Error;
  result.error = EIO;
  switch (ssl_error) {
    case SSL_ERROR_ZERO_RETURN:
      result.status = IoResult::Status::Eof;
      result.error = 0;
      result.text = "TLS connection closed";
      break;
    case SSL_ERROR_SYSCALL:
      if (errno != 0) {
        result.error = errno;
        result.text = std::string(operation) + ": " + errno_text(errno);
      } else {
        result.status = IoResult::Status::Eof;
        result.error = 0;
        result.text = "TLS peer closed the connection";
      }
      break;
    case SSL_ERROR_SSL:
      result.text = ssl_error_with(operation);
      break;
    default:
      result.text = std::string(operation) + ": " + std::to_string(ssl_error);
      break;
  }
  return result;
}

}  // namespace

struct TlsContext::Impl {
  SSL_CTX* context = nullptr;
  std::string alpn;
  bool server = false;

  ~Impl() {
    if (context) SSL_CTX_free(context);
  }
};

TlsContext::~TlsContext() = default;

TlsContext::TlsContext(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

std::shared_ptr<TlsContext> TlsContext::make_server(const TlsConfig& config, std::string& error) {
  init_openssl();
  error.clear();
  std::unique_ptr<Impl> impl = std::make_unique<Impl>();
  impl->server = true;
  impl->alpn = config.alpn;
  impl->context = SSL_CTX_new(TLS_server_method());
  if (!impl->context) {
    error = ssl_error_with("cannot create TLS server context");
    return nullptr;
  }

  SSL_CTX_set_min_proto_version(impl->context, minimum_version(config.minimum_version));
  SSL_CTX_set_options(impl->context, SSL_OP_NO_COMPRESSION);
#ifdef SSL_OP_NO_RENEGOTIATION
  SSL_CTX_set_options(impl->context, SSL_OP_NO_RENEGOTIATION);
#endif
  SSL_CTX_set_mode(impl->context, SSL_MODE_ENABLE_PARTIAL_WRITE |
                                      SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER |
                                      SSL_MODE_RELEASE_BUFFERS);
  SSL_CTX_set_session_cache_mode(impl->context, SSL_SESS_CACHE_SERVER);

  if (config.certificate_file.empty() || config.private_key_file.empty()) {
    error = "TLS server requires certificate_file and private_key_file";
    return nullptr;
  }

  if (SSL_CTX_use_certificate_chain_file(impl->context, config.certificate_file.c_str()) != 1) {
    error = ssl_error_with("cannot load TLS certificate");
    return nullptr;
  }
  if (SSL_CTX_use_PrivateKey_file(impl->context, config.private_key_file.c_str(),
                                  SSL_FILETYPE_PEM) != 1) {
    error = ssl_error_with("cannot load TLS private key");
    return nullptr;
  }
  if (SSL_CTX_check_private_key(impl->context) != 1) {
    error = ssl_error_with("TLS certificate and private key do not match");
    return nullptr;
  }

  if (!config.alpn.empty()) {
    SSL_CTX_set_alpn_select_cb(impl->context, alpn_select_callback, &impl->alpn);
  }

  if (config.require_client_certificate) {
    if (!config.ca_file.empty()) {
      if (SSL_CTX_load_verify_locations(impl->context, config.ca_file.c_str(), nullptr) != 1) {
        error = ssl_error_with("cannot load client CA file");
        return nullptr;
      }
    } else if (SSL_CTX_set_default_verify_paths(impl->context) != 1) {
      error = ssl_error_with("cannot load system CA store");
      return nullptr;
    }
    SSL_CTX_set_verify(impl->context,
                       SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, nullptr);
  } else {
    SSL_CTX_set_verify(impl->context, SSL_VERIFY_NONE, nullptr);
  }

  return std::shared_ptr<TlsContext>(new TlsContext(std::move(impl)));
}

std::shared_ptr<TlsContext> TlsContext::make_client(bool verify, std::string_view ca_file,
                                                    std::string& error,
                                                    std::string_view certificate_file,
                                                    std::string_view private_key_file) {
  init_openssl();
  error.clear();
  std::unique_ptr<Impl> impl = std::make_unique<Impl>();
  impl->context = SSL_CTX_new(TLS_client_method());
  if (!impl->context) {
    error = ssl_error_with("cannot create TLS client context");
    return nullptr;
  }

  SSL_CTX_set_min_proto_version(impl->context, TLS1_2_VERSION);
  SSL_CTX_set_options(impl->context, SSL_OP_NO_COMPRESSION);
#ifdef SSL_OP_NO_RENEGOTIATION
  SSL_CTX_set_options(impl->context, SSL_OP_NO_RENEGOTIATION);
#endif
  SSL_CTX_set_mode(impl->context, SSL_MODE_ENABLE_PARTIAL_WRITE |
                                      SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER |
                                      SSL_MODE_RELEASE_BUFFERS);

  if (verify) {
    SSL_CTX_set_verify(impl->context, SSL_VERIFY_PEER, nullptr);
    if (!ca_file.empty()) {
      if (SSL_CTX_load_verify_locations(impl->context, std::string(ca_file).c_str(), nullptr) != 1) {
        error = ssl_error_with("cannot load CA file");
        return nullptr;
      }
    } else if (SSL_CTX_set_default_verify_paths(impl->context) != 1) {
      error = ssl_error_with("cannot load system CA store");
      return nullptr;
    }
  } else {
    SSL_CTX_set_verify(impl->context, SSL_VERIFY_NONE, nullptr);
  }

  if (!certificate_file.empty() || !private_key_file.empty()) {
    if (certificate_file.empty() || private_key_file.empty()) {
      error = "TLS client certificate requires both certificate_file and private_key_file";
      return nullptr;
    }
    if (SSL_CTX_use_certificate_chain_file(impl->context,
                                           std::string(certificate_file).c_str()) != 1) {
      error = ssl_error_with("cannot load client certificate");
      return nullptr;
    }
    if (SSL_CTX_use_PrivateKey_file(impl->context, std::string(private_key_file).c_str(),
                                    SSL_FILETYPE_PEM) != 1) {
      error = ssl_error_with("cannot load client private key");
      return nullptr;
    }
    if (SSL_CTX_check_private_key(impl->context) != 1) {
      error = ssl_error_with("client certificate and private key do not match");
      return nullptr;
    }
  }

  static const unsigned char alpn[] = {8, 'h', 't', 't', 'p', '/', '1', '.', '1'};
  SSL_CTX_set_alpn_protos(impl->context, alpn, sizeof alpn);

  return std::shared_ptr<TlsContext>(new TlsContext(std::move(impl)));
}

TlsStream::TlsStream(Socket socket, std::shared_ptr<TlsContext> context, void* ssl)
    : socket_(std::move(socket)), context_(std::move(context)), ssl_(ssl) {}

TlsStream::~TlsStream() {
  if (ssl_) {
    SSL_free(static_cast<SSL*>(ssl_));
    ssl_ = nullptr;
  }
}

task<TlsResult> TlsStream::accept(Socket socket, std::shared_ptr<TlsContext> context,
                                  Deadline deadline) {
  SSL* ssl = SSL_new(context->impl_->context);
  if (!ssl) {
    co_return TlsResult{nullptr, ssl_error_with("SSL_new failed")};
  }
  SSL_set_mode(ssl, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
  if (SSL_set_fd(ssl, socket.fd()) != 1) {
    SSL_free(ssl);
    co_return TlsResult{nullptr, ssl_error_with("SSL_set_fd failed")};
  }
  auto self = std::unique_ptr<TlsStream>(new TlsStream(std::move(socket), std::move(context), ssl));
  auto error = co_await std::move(self->handshake(true, false, {}, deadline));
  if (!error.empty()) {
    co_return TlsResult{nullptr, std::move(error)};
  }
  co_return TlsResult{std::move(self), {}};
}

task<TlsResult> TlsStream::connect(Socket socket, std::string host,
                                   std::shared_ptr<TlsContext> context, bool verify_hostname,
                                   Deadline deadline) {
  SSL* ssl = SSL_new(context->impl_->context);
  if (!ssl) {
    co_return TlsResult{nullptr, ssl_error_with("SSL_new failed")};
  }
  SSL_set_mode(ssl, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
  if (SSL_set_fd(ssl, socket.fd()) != 1) {
    SSL_free(ssl);
    co_return TlsResult{nullptr, ssl_error_with("SSL_set_fd failed")};
  }

  const bool ip = is_ip_address(host);
  if (!ip) {
    SSL_set_tlsext_host_name(ssl, host.c_str());
  }
  if (verify_hostname) {
    X509_VERIFY_PARAM* param = SSL_get0_param(ssl);
    X509_VERIFY_PARAM_set_hostflags(param, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
    if (ip) {
      if (X509_VERIFY_PARAM_set1_ip_asc(param, host.c_str()) != 1) {
        SSL_free(ssl);
        co_return TlsResult{nullptr, "invalid IP address for TLS verification"};
      }
    } else if (X509_VERIFY_PARAM_set1_host(param, host.c_str(), 0) != 1) {
      SSL_free(ssl);
      co_return TlsResult{nullptr, "cannot configure TLS hostname verification"};
    }
  }

  auto self = std::unique_ptr<TlsStream>(new TlsStream(std::move(socket), std::move(context), ssl));
  auto error = co_await std::move(self->handshake(false, verify_hostname, host, deadline));
  if (!error.empty()) {
    co_return TlsResult{nullptr, std::move(error)};
  }
  co_return TlsResult{std::move(self), {}};
}

task<std::string> TlsStream::handshake(bool server, bool verify_hostname, const std::string& host,
                                       Deadline deadline) {
  (void)host;
  EventLoop* loop = EventLoop::current();
  if (!loop) co_return "TLS handshake requires an event loop";
  SSL* ssl = static_cast<SSL*>(ssl_);

  if (server) {
    SSL_set_accept_state(ssl);
  } else {
    SSL_set_connect_state(ssl);
  }

  for (;;) {
    ERR_clear_error();
    int result = SSL_do_handshake(ssl);
    if (result == 1) {
      if (!server && verify_hostname) {
        long verify = SSL_get_verify_result(ssl);
        if (verify != X509_V_OK) {
          co_return std::string("TLS certificate verification failed: ") +
                   X509_verify_cert_error_string(verify);
        }
      }
      co_return {};
    }

    int error = SSL_get_error(ssl, result);
    if (error == SSL_ERROR_WANT_READ) {
      auto ready = co_await wait_readable(*loop, socket_.fd(), deadline);
      if (!ready.ok()) co_return ready.message();
      continue;
    }
    if (error == SSL_ERROR_WANT_WRITE) {
      auto ready = co_await wait_writable(*loop, socket_.fd(), deadline);
      if (!ready.ok()) co_return ready.message();
      continue;
    }
    if (error == SSL_ERROR_ZERO_RETURN) {
      co_return "TLS peer closed during handshake";
    }
    co_return ssl_error_with("TLS handshake failed");
  }
}

task<IoResult> TlsStream::read_some(std::span<std::byte> buffer, Deadline deadline) {
  SSL* ssl = static_cast<SSL*>(ssl_);
  if (!ssl) {
    IoResult result;
    result.status = IoResult::Status::Error;
    result.error = EBADF;
    co_return result;
  }
  if (buffer.empty()) co_return IoResult{};

  EventLoop* loop = EventLoop::current();
  if (!loop) {
    IoResult result;
    result.status = IoResult::Status::Error;
    result.error = EINVAL;
    co_return result;
  }

  for (;;) {
    ERR_clear_error();
    int n = SSL_read(ssl, buffer.data(), static_cast<int>(buffer.size()));
    if (n > 0) {
      IoResult result;
      result.n = static_cast<std::size_t>(n);
      co_return result;
    }

    int error = SSL_get_error(ssl, n);
    if (error == SSL_ERROR_WANT_READ) {
      auto ready = co_await wait_readable(*loop, socket_.fd(), deadline);
      if (!ready.ok()) co_return ready;
      continue;
    }
    if (error == SSL_ERROR_WANT_WRITE) {
      auto ready = co_await wait_writable(*loop, socket_.fd(), deadline);
      if (!ready.ok()) co_return ready;
      continue;
    }
    co_return tls_io_error(error, "TLS read failed");
  }
}

task<IoResult> TlsStream::write_some(std::span<const std::byte> buffer, Deadline deadline) {
  SSL* ssl = static_cast<SSL*>(ssl_);
  if (!ssl) {
    IoResult result;
    result.status = IoResult::Status::Error;
    result.error = EBADF;
    co_return result;
  }
  if (buffer.empty()) co_return IoResult{};

  EventLoop* loop = EventLoop::current();
  if (!loop) {
    IoResult result;
    result.status = IoResult::Status::Error;
    result.error = EINVAL;
    co_return result;
  }

  for (;;) {
    ERR_clear_error();
    int n = SSL_write(ssl, buffer.data(), static_cast<int>(buffer.size()));
    if (n > 0) {
      IoResult result;
      result.n = static_cast<std::size_t>(n);
      co_return result;
    }

    int error = SSL_get_error(ssl, n);
    if (error == SSL_ERROR_WANT_READ) {
      auto ready = co_await wait_readable(*loop, socket_.fd(), deadline);
      if (!ready.ok()) co_return ready;
      continue;
    }
    if (error == SSL_ERROR_WANT_WRITE) {
      auto ready = co_await wait_writable(*loop, socket_.fd(), deadline);
      if (!ready.ok()) co_return ready;
      continue;
    }
    co_return tls_io_error(error, "TLS write failed");
  }
}

task<void> TlsStream::shutdown(Deadline) {
  SSL* ssl = static_cast<SSL*>(ssl_);
  if (ssl) {
    ERR_clear_error();
    SSL_shutdown(ssl);
  }
  if (socket_.fd() >= 0) ::shutdown(socket_.fd(), SHUT_WR);
  co_return;
}

std::string TlsStream::tls_version() const {
  if (!ssl_) return {};
  const char* version = SSL_get_version(static_cast<SSL*>(ssl_));
  return version ? version : std::string();
}

std::string TlsStream::peer_certificate_subject() const {
  if (!ssl_) return {};
  X509* certificate = SSL_get1_peer_certificate(static_cast<SSL*>(ssl_));
  if (!certificate) return {};
  char buffer[512];
  X509_NAME_oneline(X509_get_subject_name(certificate), buffer, sizeof buffer);
  X509_free(certificate);
  return buffer;
}

}  // namespace ferry
