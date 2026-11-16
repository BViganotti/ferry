// ferry::WebSocket — RFC 6455 server and client.

#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ferry/http.hpp"
#include "ferry/net.hpp"

namespace ferry {

struct WsFrame {
  enum class Opcode { Text = 1, Binary = 2, Close = 8, Ping = 9, Pong = 10 };
  Opcode opcode = Opcode::Text;
  std::string payload;
  bool fin = true;

  bool text() const noexcept { return opcode == Opcode::Text; }
  bool binary() const noexcept { return opcode == Opcode::Binary; }
};

struct WsClientOptions {
  bool verify_tls = true;
  std::string ca_file;
  std::chrono::milliseconds timeout{30'000};
  std::chrono::milliseconds idle_timeout{60'000};
};

class WebSocket {
 public:
  // Server-side: borrows the connection stream and its parser buffer.
  WebSocket(Stream& stream, std::string& buffer, bool client_side = false);

  task<std::optional<WsFrame>> read();
  task<void> send_text(std::string_view text);
  task<void> send_binary(std::span<const std::byte> data);
  task<void> ping(std::string_view payload = {});
  task<void> pong(std::string_view payload = {});
  task<void> close(std::uint16_t code = 1000, std::string_view reason = {});

  std::size_t max_message_size = 16 * 1024 * 1024;
  std::chrono::milliseconds idle_timeout{60'000};

 private:
  friend task<std::unique_ptr<WebSocket>> websocket_connect(
      std::string_view url, Headers headers, WsClientOptions options);

  WebSocket(std::unique_ptr<Stream> stream, std::string buffer, bool client_side);

  task<bool> ensure(std::size_t size);
  task<void> send_frame(WsFrame::Opcode opcode, std::string_view payload, bool fin = true);
  task<void> protocol_close(std::uint16_t code, std::string_view reason);

  Stream* stream_ = nullptr;
  std::unique_ptr<Stream> owned_;
  std::string* buffer_ = nullptr;
  std::string owned_buffer_;
  bool client_side_ = false;
};

using WsHandler = std::function<task<void>(WebSocket&)>;

// Performs the client handshake over ws:// or wss:// and returns a live socket.
task<std::unique_ptr<WebSocket>> websocket_connect(std::string_view url,
                                                   Headers headers = Headers{},
                                                   WsClientOptions options = {});

// Computes Sec-WebSocket-Accept (used by the server route helper).
std::string websocket_accept_key(std::string_view key);

}  // namespace ferry
