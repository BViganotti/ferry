#include "ferry/websocket.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <random>
#include <stdexcept>

#include <openssl/rand.h>
#include <string>
#include <utility>
#include <vector>

#include "codec.hpp"
#include "ferry/event_loop.hpp"
#include "ferry/tls.hpp"
#include "ferry/url.hpp"

namespace ferry {

namespace {

std::string_view trim(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
  return text;
}

uint32_t rotate_left(uint32_t value, int bits) {
  return (value << bits) | (value >> (32 - bits));
}

std::array<unsigned char, 20> sha1(std::string_view data) {
  std::vector<unsigned char> message(data.begin(), data.end());
  std::uint64_t bit_length = static_cast<std::uint64_t>(data.size()) * 8;
  message.push_back(0x80);
  while (message.size() % 64 != 56) message.push_back(0);
  for (int i = 7; i >= 0; --i) message.push_back(static_cast<unsigned char>(bit_length >> (i * 8)));

  std::uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
  for (std::size_t offset = 0; offset < message.size(); offset += 64) {
    std::uint32_t w[80]{};
    for (int i = 0; i < 16; ++i) {
      w[i] = (static_cast<std::uint32_t>(message[offset + i * 4]) << 24) |
             (static_cast<std::uint32_t>(message[offset + i * 4 + 1]) << 16) |
             (static_cast<std::uint32_t>(message[offset + i * 4 + 2]) << 8) |
             static_cast<std::uint32_t>(message[offset + i * 4 + 3]);
    }
    for (int i = 16; i < 80; ++i) {
      w[i] = rotate_left(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }
    std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; ++i) {
      std::uint32_t f = 0;
      std::uint32_t k = 0;
      if (i < 20) {
        f = (b & c) | ((~b) & d);
        k = 0x5A827999;
      } else if (i < 40) {
        f = b ^ c ^ d;
        k = 0x6ED9EBA1;
      } else if (i < 60) {
        f = (b & c) | (b & d) | (c & d);
        k = 0x8F1BBCDC;
      } else {
        f = b ^ c ^ d;
        k = 0xCA62C1D6;
      }
      std::uint32_t temp = rotate_left(a, 5) + f + e + k + w[i];
      e = d;
      d = c;
      c = rotate_left(b, 30);
      b = a;
      a = temp;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
  }

  std::array<unsigned char, 20> result{};
  for (int i = 0; i < 5; ++i) {
    result[i * 4] = static_cast<unsigned char>(h[i] >> 24);
    result[i * 4 + 1] = static_cast<unsigned char>(h[i] >> 16);
    result[i * 4 + 2] = static_cast<unsigned char>(h[i] >> 8);
    result[i * 4 + 3] = static_cast<unsigned char>(h[i]);
  }
  return result;
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

std::string random_bytes(std::size_t count) {
  std::string result(count, '\0');
  if (RAND_bytes(reinterpret_cast<unsigned char*>(result.data()),
                 static_cast<int>(count)) == 1) {
    return result;
  }
  static thread_local std::mt19937_64 generator{std::random_device{}()};
  for (std::size_t i = 0; i < count; ++i) {
    result[i] = static_cast<char>(generator() & 0xff);
  }
  return result;
}


}  // namespace

std::string websocket_accept_key(std::string_view key) {
  std::string source(key);
  source += "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
  auto digest = sha1(source);
  return base64_encode(std::string_view(reinterpret_cast<const char*>(digest.data()), digest.size()));
}

WebSocket::WebSocket(Stream& stream, std::string& buffer, bool client_side)
    : stream_(&stream), buffer_(&buffer), client_side_(client_side) {}

WebSocket::WebSocket(std::unique_ptr<Stream> stream, std::string buffer, bool client_side)
    : stream_(stream.get()),
      owned_(std::move(stream)),
      owned_buffer_(std::move(buffer)),
      client_side_(client_side) {
  buffer_ = &owned_buffer_;
}

task<bool> WebSocket::ensure(std::size_t size) {
  while (buffer_->size() < size) {
    char temporary[16384];
    auto result = co_await stream_->read_some(as_bytes(std::span<char>(temporary, sizeof temporary)),
                                              Deadline::after(idle_timeout));
    if (result.eof()) co_return false;
    if (!result.ok()) co_return false;
    buffer_->append(temporary, result.n);
  }
  co_return true;
}

task<void> WebSocket::send_frame(WsFrame::Opcode opcode, std::string_view payload, bool fin) {
  if (!stream_) co_return;
  std::string frame;
  frame.reserve(payload.size() + 14);
  frame.push_back(static_cast<char>((fin ? 0x80 : 0x00) | static_cast<int>(opcode)));
  std::size_t length = payload.size();
  unsigned char length_byte = client_side_ ? 0x80 : 0x00;
  if (length < 126) {
    frame.push_back(static_cast<char>(length_byte | static_cast<unsigned char>(length)));
  } else if (length <= 0xffff) {
    frame.push_back(static_cast<char>(length_byte | 126));
    frame.push_back(static_cast<char>(length >> 8));
    frame.push_back(static_cast<char>(length));
  } else {
    frame.push_back(static_cast<char>(length_byte | 127));
    for (int shift = 56; shift >= 0; shift -= 8) {
      frame.push_back(static_cast<char>(length >> shift));
    }
  }

  std::string body(payload);
  if (client_side_) {
    std::string mask = random_bytes(4);
    frame.append(mask);
    for (std::size_t i = 0; i < body.size(); ++i) body[i] ^= mask[i % 4];
  }
  frame.append(body);
  auto result = co_await write_all(*stream_, frame, Deadline::after(idle_timeout));
  (void)result;
  co_return;
}

task<void> WebSocket::send_text(std::string_view text) {
  co_await send_frame(WsFrame::Opcode::Text, text);
}

task<void> WebSocket::send_binary(std::span<const std::byte> data) {
  co_await send_frame(WsFrame::Opcode::Binary,
                      std::string_view(reinterpret_cast<const char*>(data.data()), data.size()));
}

task<void> WebSocket::ping(std::string_view payload) {
  co_await send_frame(WsFrame::Opcode::Ping, payload);
}

task<void> WebSocket::pong(std::string_view payload) {
  co_await send_frame(WsFrame::Opcode::Pong, payload);
}

task<void> WebSocket::protocol_close(std::uint16_t code, std::string_view reason) {
  std::string payload;
  payload.push_back(static_cast<char>(code >> 8));
  payload.push_back(static_cast<char>(code));
  payload.append(reason.substr(0, 123));
  co_await send_frame(WsFrame::Opcode::Close, payload);
}

task<void> WebSocket::close(std::uint16_t code, std::string_view reason) {
  co_await protocol_close(code, reason);
}

task<std::optional<WsFrame>> WebSocket::read() {
  std::string fragments;
  int fragment_opcode = 0;

  for (;;) {
    if (!co_await ensure(2)) co_return std::nullopt;
    auto view = std::string_view(*buffer_);
    unsigned char first = static_cast<unsigned char>(view[0]);
    unsigned char second = static_cast<unsigned char>(view[1]);
    bool fin = (first & 0x80) != 0;
    int opcode = first & 0x0f;
    bool masked = (second & 0x80) != 0;
    std::uint64_t length = second & 0x7f;
    std::size_t header = 2;

    if ((first & 0x70) != 0) {
      co_await protocol_close(1002, "RSV bits set");
      co_return std::nullopt;
    }
    if (length == 126) {
      if (!co_await ensure(4)) co_return std::nullopt;
      view = std::string_view(*buffer_);
      length = (static_cast<unsigned char>(view[2]) << 8) |
               static_cast<unsigned char>(view[3]);
      header = 4;
    } else if (length == 127) {
      if (!co_await ensure(10)) co_return std::nullopt;
      view = std::string_view(*buffer_);
      length = 0;
      for (int i = 0; i < 8; ++i) {
        length = (length << 8) | static_cast<unsigned char>(view[2 + i]);
      }
      header = 10;
    }

    if (length > max_message_size) {
      co_await protocol_close(1009, "message too large");
      co_return std::nullopt;
    }
    bool control = opcode >= 8;
    if (control && (!fin || length > 125)) {
      co_await protocol_close(1002, "invalid control frame");
      co_return std::nullopt;
    }
    if (masked == client_side_) {
      co_await protocol_close(1002, "incorrect masking");
      co_return std::nullopt;
    }

    std::string mask;
    if (masked) {
      if (!co_await ensure(header + 4 + length)) co_return std::nullopt;
      view = std::string_view(*buffer_);
      mask.assign(view.data() + header, 4);
      header += 4;
    } else if (!co_await ensure(header + length)) {
      co_return std::nullopt;
    }

    view = std::string_view(*buffer_);
    std::string payload(view.data() + header, static_cast<std::size_t>(length));
    if (masked) {
      for (std::size_t i = 0; i < payload.size(); ++i) payload[i] ^= mask[i % 4];
    }
    buffer_->erase(0, header + static_cast<std::size_t>(length));

    if (opcode == 0x8) {
      WsFrame frame;
      frame.opcode = WsFrame::Opcode::Close;
      frame.payload = payload;
      co_await send_frame(WsFrame::Opcode::Close, payload);
      co_return frame;
    }
    if (opcode == 0x9) {
      co_await send_frame(WsFrame::Opcode::Pong, payload);
      continue;
    }
    if (opcode == 0xA) {
      WsFrame frame;
      frame.opcode = WsFrame::Opcode::Pong;
      frame.payload = std::move(payload);
      co_return frame;
    }
    if (opcode == 0x1 || opcode == 0x2) {
      if (!fin) {
        if (!fragments.empty()) {
          co_await protocol_close(1002, "nested fragmented message");
          co_return std::nullopt;
        }
        fragment_opcode = opcode;
        fragments = std::move(payload);
        continue;
      }
      WsFrame frame;
      frame.opcode = opcode == 0x1 ? WsFrame::Opcode::Text : WsFrame::Opcode::Binary;
      frame.payload = std::move(payload);
      co_return frame;
    }
    if (opcode == 0x0) {
      if (fragment_opcode == 0) {
        co_await protocol_close(1002, "unexpected continuation");
        co_return std::nullopt;
      }
      if (fragments.size() + payload.size() > max_message_size) {
        co_await protocol_close(1009, "message too large");
        co_return std::nullopt;
      }
      fragments += payload;
      if (fin) {
        WsFrame frame;
        frame.opcode = fragment_opcode == 0x1 ? WsFrame::Opcode::Text : WsFrame::Opcode::Binary;
        frame.payload = std::move(fragments);
        fragment_opcode = 0;
        co_return frame;
      }
      continue;
    }

    co_await protocol_close(1002, "unknown opcode");
    co_return std::nullopt;
  }
}

task<std::unique_ptr<WebSocket>> websocket_connect(std::string_view url, Headers headers,
                                                   WsClientOptions options) {
  EventLoop* loop = EventLoop::current();
  if (!loop) co_return nullptr;
  auto parsed = Url::parse(url);
  if (!parsed || (parsed->scheme != "ws" && parsed->scheme != "wss") || parsed->host.empty()) {
    co_return nullptr;
  }

  bool secure = parsed->scheme == "wss";
  std::uint16_t port = parsed->port != 0 ? parsed->port : (secure ? 443 : 80);
  auto deadline = Deadline::after(options.timeout);
  auto connected = co_await connect_tcp(parsed->host, port, deadline);
  if (!connected.socket.valid()) co_return nullptr;

  std::unique_ptr<Stream> stream;
  if (secure) {
    std::string error;
    auto context = TlsContext::make_client(options.verify_tls, options.ca_file, error);
    if (!context) co_return nullptr;
    auto result =
        co_await TlsStream::connect(std::move(connected.socket), parsed->host, context,
                                    options.verify_tls, deadline);
    if (!result.ok()) co_return nullptr;
    stream = std::move(result.stream);
  } else {
    stream = std::make_unique<PlainStream>(std::move(connected.socket));
  }

  std::string key = base64_encode(random_bytes(16));
  Headers request_headers = std::move(headers);
  request_headers.set("host", parsed->host_header());
  request_headers.set("upgrade", "websocket");
  request_headers.set("connection", "Upgrade");
  request_headers.set("sec-websocket-key", key);
  request_headers.set("sec-websocket-version", "13");

  std::string head = "GET " + parsed->target() + " HTTP/1.1\r\n";
  for (const auto& [name, value] : request_headers.entries()) {
    head += name + ": " + value + "\r\n";
  }
  head += "\r\n";
  auto written = co_await write_all(*stream, head, deadline);
  if (!written.ok()) co_return nullptr;

  std::string buffer;
  auto response_head = co_await detail::read_head(*stream, buffer, 64 * 1024, deadline);
  if (response_head.status != detail::ReadStatus::Ok) co_return nullptr;
  auto line_end = buffer.find("\r\n");
  if (line_end == std::string::npos || !buffer.starts_with("HTTP/1.1 101")) co_return nullptr;
  std::string_view header_block(buffer.data() + line_end + 2, buffer.find("\r\n\r\n") - line_end - 2);
  Headers response_headers;
  while (!header_block.empty()) {
    auto crlf = header_block.find("\r\n");
    auto line = header_block.substr(0, crlf);
    header_block = crlf == std::string_view::npos ? std::string_view()
                                                  : header_block.substr(crlf + 2);
    auto colon = line.find(':');
    if (colon == std::string_view::npos) continue;
    response_headers.add(std::string(trim(line.substr(0, colon))),
                         std::string(trim(line.substr(colon + 1))));
  }
  if (!Headers::equals(response_headers.get("upgrade"), "websocket") ||
      !Headers::equals(response_headers.get("sec-websocket-accept"),
                       websocket_accept_key(key))) {
    co_return nullptr;
  }

  auto buffer_end = buffer.find("\r\n\r\n");
  std::string leftover =
      buffer_end == std::string::npos ? std::string() : buffer.substr(buffer_end + 4);
  std::unique_ptr<WebSocket> socket(
      new WebSocket(std::move(stream), std::move(leftover), true));
  socket->idle_timeout = options.idle_timeout;
  co_return socket;
}

}  // namespace ferry
