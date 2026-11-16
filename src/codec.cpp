#include "codec.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <limits>
#include <string>
#include <system_error>
#include <utility>

#include "ferry/url.hpp"

namespace ferry::detail {

namespace {

bool is_tchar(unsigned char c) {
  if (std::isalnum(c)) return true;
  switch (c) {
    case '!':
    case '#':
    case '$':
    case '%':
    case '&':
    case '\'':
    case '*':
    case '+':
    case '-':
    case '.':
    case '^':
    case '_':
    case '`':
    case '|':
    case '~':
      return true;
    default:
      return false;
  }
}

std::string_view trim_ows(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
  return text;
}

bool contains_token(std::string_view list, std::string_view token) {
  while (!list.empty()) {
    auto comma = list.find(',');
    auto item = trim_ows(comma == std::string_view::npos ? list : list.substr(0, comma));
    if (Headers::equals(item, token)) return true;
    if (comma == std::string_view::npos) break;
    list.remove_prefix(comma + 1);
  }
  return false;
}

bool valid_target(std::string_view target) {
  for (unsigned char c : target) {
    if (c <= 0x20 || c == 0x7f) return false;
  }
  return !target.empty() && target.find('#') == std::string_view::npos;
}

struct Framing {
  enum class Kind { None, Length, Chunked } kind = Kind::None;
  std::uint64_t length = 0;
  int status = 200;
  std::string message;
  bool ok = true;
};

Framing analyze_framing(const Headers& headers, bool request, std::uint64_t max_body) {
  Framing framing;

  auto transfer_encoding = headers.get_all("transfer-encoding");
  auto content_lengths = headers.get_all("content-length");

  if (!transfer_encoding.empty() && !content_lengths.empty()) {
    framing.ok = false;
    framing.status = 400;
    framing.message = "both Transfer-Encoding and Content-Length present";
    return framing;
  }

  if (!transfer_encoding.empty()) {
    if (transfer_encoding.size() != 1) {
      framing.ok = false;
      framing.status = 400;
      framing.message = "multiple Transfer-Encoding headers";
      return framing;
    }
    auto value = trim_ows(transfer_encoding.front());
    if (Headers::equals(value, "chunked")) {
      framing.kind = Framing::Kind::Chunked;
      return framing;
    }
    framing.ok = false;
    framing.status = 501;
    framing.message = "unsupported Transfer-Encoding";
    return framing;
  }

  if (!content_lengths.empty()) {
    std::uint64_t length = 0;
    bool first = true;
    for (auto raw : content_lengths) {
      auto value = trim_ows(raw);
      if (value.empty() || value.find(',') != std::string_view::npos) {
        framing.ok = false;
        framing.status = 400;
        framing.message = "invalid Content-Length";
        return framing;
      }
      std::uint64_t parsed = 0;
      auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), parsed);
      if (ec != std::errc() || ptr != value.data() + value.size()) {
        framing.ok = false;
        framing.status = 400;
        framing.message = "invalid Content-Length";
        return framing;
      }
      if (!first && parsed != length) {
        framing.ok = false;
        framing.status = 400;
        framing.message = "conflicting Content-Length values";
        return framing;
      }
      length = parsed;
      first = false;
    }
    if (length > max_body) {
      framing.ok = false;
      framing.status = 413;
      framing.message = "request body too large";
      return framing;
    }
    framing.kind = Framing::Kind::Length;
    framing.length = length;
  }

  if (request) {
    // No framing means no body. There is no reason to require Content-Length.
  }
  return framing;
}

ParseResult parse_header_lines(std::string_view text, Headers& headers, const Limits& limits,
                               int& header_count) {
  ParseResult result;
  while (!text.empty()) {
    auto crlf = text.find("\r\n");
    if (crlf == std::string_view::npos) {
      result.http_status = 400;
      result.message = "malformed header line";
      return result;
    }
    auto line = text.substr(0, crlf);
    text.remove_prefix(crlf + 2);

    if (line.empty()) {
      result.ok = true;
      result.head_length = 0;
      return result;
    }
    if (line.size() > limits.max_header_line) {
      result.http_status = 431;
      result.message = "header line too long";
      return result;
    }
    if (line.front() == ' ' || line.front() == '\t') {
      result.http_status = 400;
      result.message = "obsolete header folding is not accepted";
      return result;
    }
    auto colon = line.find(':');
    if (colon == std::string_view::npos || colon == 0) {
      result.http_status = 400;
      result.message = "malformed header";
      return result;
    }
    auto name = line.substr(0, colon);
    if (!is_token(name)) {
      result.http_status = 400;
      result.message = "invalid header name";
      return result;
    }
    auto value = trim_ows(line.substr(colon + 1));
    if (!is_valid_header_value(value)) {
      result.http_status = 400;
      result.message = "invalid header value";
      return result;
    }
    if (++header_count > static_cast<int>(limits.max_headers)) {
      result.http_status = 431;
      result.message = "too many headers";
      return result;
    }
    try {
      headers.add(std::string(name), std::string(value));
    } catch (const std::exception&) {
      result.http_status = 400;
      result.message = "invalid header";
      return result;
    }
  }
  result.http_status = 400;
  result.message = "incomplete headers";
  return result;
}

ReadStatus map_status(const IoResult& result) {
  switch (result.status) {
    case IoResult::Status::Ok:
      return ReadStatus::Ok;
    case IoResult::Status::Eof:
      return ReadStatus::Eof;
    case IoResult::Status::Timeout:
      return ReadStatus::Timeout;
    case IoResult::Status::Cancelled:
      return ReadStatus::Cancelled;
    case IoResult::Status::Error:
      return ReadStatus::Error;
  }
  return ReadStatus::Error;
}

task<ReadStatus> read_more(Stream& stream, std::string& buffer, Deadline deadline) {
  char raw[16384];
  auto result = co_await stream.read_some(as_bytes(std::span<char>(raw, sizeof raw)), deadline);
  if (result.ok()) {
    if (result.n > 0) buffer.append(raw, result.n);
    co_return ReadStatus::Ok;
  }
  co_return map_status(result);
}

class ChunkedDecoder {
 public:
  enum class Result { NeedMore, Done, Bad, TooLarge };

  Result feed(std::string_view data, std::string& out, std::size_t max_body,
              const Limits& limits, std::size_t& consumed) {
    std::size_t index = 0;
    for (;;) {
      switch (state_) {
        case State::SizeLine: {
          while (index < data.size()) {
            char c = data[index++];
            if (c == '\n') {
              if (line_.empty() || line_.back() != '\r') return fail(Result::Bad);
              line_.pop_back();
              auto semicolon = line_.find(';');
              auto hex = std::string_view(line_).substr(0, semicolon);
              if (hex.empty()) return fail(Result::Bad);
              std::uint64_t value = 0;
              for (char digit : hex) {
                int nibble = -1;
                if (digit >= '0' && digit <= '9') {
                  nibble = digit - '0';
                } else if (digit >= 'a' && digit <= 'f') {
                  nibble = digit - 'a' + 10;
                } else if (digit >= 'A' && digit <= 'F') {
                  nibble = digit - 'A' + 10;
                } else {
                  return fail(Result::Bad);
                }
                if (value > (std::numeric_limits<std::uint64_t>::max() -
                             static_cast<std::uint64_t>(nibble)) /
                                16) {
                  return fail(Result::Bad);
                }
                value = value * 16 + static_cast<std::uint64_t>(nibble);
              }
              remaining_ = value;
              line_.clear();
              state_ = value == 0 ? State::Trailers : State::Data;
              break;
            }
            line_.push_back(c);
            if (line_.size() > limits.max_chunk_line) return fail(Result::Bad);
          }
          if (index >= data.size()) {
            consumed = index;
            return Result::NeedMore;
          }
          break;
        }
        case State::Data: {
          if (remaining_ == 0) {
            state_ = State::DataCrlf;
            break;
          }
          std::size_t available = data.size() - index;
          if (available == 0) {
            consumed = index;
            return Result::NeedMore;
          }
          std::size_t take = static_cast<std::size_t>(
              std::min<std::uint64_t>(remaining_, static_cast<std::uint64_t>(available)));
          if (out.size() + take > max_body) return fail(Result::TooLarge);
          out.append(data.substr(index, take));
          index += take;
          remaining_ -= take;
          if (remaining_ == 0) state_ = State::DataCrlf;
          break;
        }
        case State::DataCrlf: {
          if (data.size() - index < 2) {
            consumed = index;
            return Result::NeedMore;
          }
          if (data[index] != '\r' || data[index + 1] != '\n') return fail(Result::Bad);
          index += 2;
          state_ = State::SizeLine;
          break;
        }
        case State::Trailers: {
          while (index < data.size()) {
            char c = data[index++];
            if (c == '\n') {
              if (line_.empty() || line_.back() != '\r') return fail(Result::Bad);
              line_.pop_back();
              bool empty_line = line_.empty();
              trailer_bytes_ += line_.size() + 2;
              line_.clear();
              if (trailer_bytes_ > limits.max_trailer_bytes) return fail(Result::Bad);
              if (empty_line) {
                state_ = State::Done;
                consumed = index;
                return Result::Done;
              }
              if (line_.size() > limits.max_chunk_line) return fail(Result::Bad);
            } else {
              line_.push_back(c);
              if (line_.size() > limits.max_chunk_line) return fail(Result::Bad);
            }
          }
          consumed = index;
          return Result::NeedMore;
        }
        case State::Done:
          consumed = index;
          return Result::Done;
        case State::Failed:
          return Result::Bad;
      }
    }
  }

 private:
  enum class State { SizeLine, Data, DataCrlf, Trailers, Done, Failed };

  Result fail(Result result) {
    state_ = State::Failed;
    return result;
  }

  State state_ = State::SizeLine;
  std::uint64_t remaining_ = 0;
  std::size_t trailer_bytes_ = 0;
  std::string line_;
};

}  // namespace

bool is_token(std::string_view text) noexcept {
  if (text.empty()) return false;
  for (unsigned char c : text) {
    if (!is_tchar(c)) return false;
  }
  return true;
}

bool is_valid_header_value(std::string_view text) noexcept {
  for (unsigned char c : text) {
    if (c == '\r' || c == '\n' || c == '\0' || c == 0x7f) return false;
    if (c < 0x20 && c != '\t') return false;
  }
  return true;
}

task<HeadResult> read_head(Stream& stream, std::string& buffer, std::size_t max_header_bytes,
                           Deadline deadline) {
  for (;;) {
    auto terminator = buffer.find("\r\n\r\n");
    if (terminator != std::string::npos) {
      if (terminator + 4 > max_header_bytes) {
        co_return HeadResult{ReadStatus::TooLarge, "header section too large"};
      }
      // Reject bare LF / CR corruption in the head.
      for (std::size_t i = 0; i < terminator; ++i) {
        if (buffer[i] == '\n' && (i == 0 || buffer[i - 1] != '\r')) {
          co_return HeadResult{ReadStatus::Bad, "bare LF in headers"};
        }
      }
      co_return HeadResult{ReadStatus::Ok, {}};
    }
    if (buffer.size() > max_header_bytes) {
      co_return HeadResult{ReadStatus::TooLarge, "header section too large"};
    }

    auto status = co_await read_more(stream, buffer, deadline);
    if (status == ReadStatus::Ok) continue;
    if (status == ReadStatus::Eof) {
      if (buffer.empty()) co_return HeadResult{ReadStatus::Eof, "connection closed"};
      co_return HeadResult{ReadStatus::Bad, "incomplete request head"};
    }
    co_return HeadResult{status, status == ReadStatus::Timeout ? "header timeout" : "read error"};
  }
}

ParseResult parse_request_head(std::string_view head, Request& request, const Limits& limits) {
  ParseResult result;
  if (head.size() > limits.max_header_bytes) {
    result.http_status = 431;
    result.message = "header section too large";
    return result;
  }
  auto crlf = head.find("\r\n");
  if (crlf == std::string_view::npos || crlf > limits.max_request_line) {
    result.http_status = 414;
    result.message = "request line too long";
    return result;
  }
  auto request_line = head.substr(0, crlf);
  auto rest = head.substr(crlf + 2);

  auto first_space = request_line.find(' ');
  if (first_space == std::string_view::npos) {
    result.http_status = 400;
    result.message = "malformed request line";
    return result;
  }
  auto second_space = request_line.find(' ', first_space + 1);
  if (second_space == std::string_view::npos ||
      request_line.find(' ', second_space + 1) != std::string_view::npos) {
    result.http_status = 400;
    result.message = "malformed request line";
    return result;
  }

  auto method = request_line.substr(0, first_space);
  auto target = request_line.substr(first_space + 1, second_space - first_space - 1);
  auto version = request_line.substr(second_space + 1);

  if (!is_token(method) || method.size() > 32) {
    result.http_status = 400;
    result.message = "invalid method";
    return result;
  }
  if (!valid_target(target) || target.size() > limits.max_request_line) {
    result.http_status = 400;
    result.message = "invalid request target";
    return result;
  }
  if (version != "HTTP/1.1" && version != "HTTP/1.0") {
    result.http_status = 505;
    result.message = "unsupported HTTP version";
    return result;
  }

  request.method = method_from_string(method);
  request.method_raw = std::string(method);
  request.target = std::string(target);
  request.version = std::string(version);

  int header_count = 0;
  auto headers_result = parse_header_lines(rest, request.headers, limits, header_count);
  if (!headers_result.ok) {
    result.http_status = headers_result.http_status;
    result.message = headers_result.message;
    return result;
  }

  if (request.version == "HTTP/1.1") {
    auto hosts = request.headers.get_all("host");
    if (hosts.size() != 1 || hosts.front().empty()) {
      result.http_status = 400;
      result.message = "HTTP/1.1 requires exactly one Host header";
      return result;
    }
  }

  auto expect = trim_ows(request.headers.get("expect"));
  if (!expect.empty() && !Headers::equals(expect, "100-continue")) {
    result.http_status = 417;
    result.message = "unsupported Expect header";
    return result;
  }

  // Request target forms.
  if (target == "*") {
    request.path = "*";
    request.query_string.clear();
  } else if (target.front() == '/') {
    auto question = target.find('?');
    request.path = std::string(target.substr(0, question));
    if (question != std::string_view::npos) {
      request.query_string = std::string(target.substr(question + 1));
    }
  } else if (target.starts_with("http://") || target.starts_with("https://")) {
    auto parsed = Url::parse(target);
    if (!parsed) {
      result.http_status = 400;
      result.message = "invalid absolute request target";
      return result;
    }
    request.path = parsed->path;
    request.query_string = parsed->query;
    if (!request.headers.has("host")) {
      request.headers.set("host", parsed->host_header());
    }
  } else if (request.method == Method::Connect) {
    request.path = std::string(target);
  } else {
    result.http_status = 400;
    result.message = "unsupported request target form";
    return result;
  }

  auto framing = analyze_framing(request.headers, true, limits.max_body_bytes);
  if (!framing.ok) {
    result.http_status = framing.status;
    result.message = framing.message;
    return result;
  }
  if (framing.kind == Framing::Kind::Chunked && request.version == "HTTP/1.0") {
    result.http_status = 400;
    result.message = "chunked request body requires HTTP/1.1";
    return result;
  }
  request.chunked = framing.kind == Framing::Kind::Chunked;
  request.content_length = framing.length;

  result.ok = true;
  result.http_status = 200;
  result.head_length = head.size();
  result.keep_alive = request.keep_alive();
  return result;
}

ParseResult parse_response_head(std::string_view head, Response& response, const Limits& limits) {
  ParseResult result;
  if (head.size() > limits.max_header_bytes) {
    result.http_status = 502;
    result.message = "response header section too large";
    return result;
  }
  auto crlf = head.find("\r\n");
  if (crlf == std::string_view::npos) {
    result.http_status = 502;
    result.message = "malformed status line";
    return result;
  }
  auto status_line = head.substr(0, crlf);
  auto rest = head.substr(crlf + 2);

  auto first_space = status_line.find(' ');
  if (first_space == std::string_view::npos) {
    result.http_status = 502;
    result.message = "malformed status line";
    return result;
  }
  auto version = status_line.substr(0, first_space);
  if (version != "HTTP/1.1" && version != "HTTP/1.0") {
    result.http_status = 502;
    result.message = "unsupported HTTP version";
    return result;
  }

  auto after = status_line.substr(first_space + 1);
  auto second_space = after.find(' ');
  auto status_text = second_space == std::string_view::npos ? after : after.substr(0, second_space);
  if (status_text.size() != 3) {
    result.http_status = 502;
    result.message = "invalid status code";
    return result;
  }
  int status = 0;
  for (char c : status_text) {
    if (c < '0' || c > '9') {
      result.http_status = 502;
      result.message = "invalid status code";
      return result;
    }
    status = status * 10 + (c - '0');
  }
  if (status < 100 || status > 599) {
    result.http_status = 502;
    result.message = "invalid status code";
    return result;
  }
  response.status = status;
  if (second_space != std::string_view::npos) {
    response.reason = std::string(after.substr(second_space + 1));
  } else {
    response.reason = std::string(reason_phrase(status));
  }

  int header_count = 0;
  auto headers_result = parse_header_lines(rest, response.headers, limits, header_count);
  if (!headers_result.ok) {
    result.http_status = 502;
    result.message = headers_result.message;
    return result;
  }

  auto framing = analyze_framing(response.headers, false, limits.max_body_bytes);
  if (!framing.ok) {
    result.http_status = 502;
    result.message = framing.message;
    return result;
  }
  bool keep_alive = version == "HTTP/1.1"
                        ? !contains_token(response.headers.get("connection"), "close")
                        : contains_token(response.headers.get("connection"), "keep-alive");
  response.close_connection = !keep_alive;

  result.ok = true;
  result.http_status = 200;
  result.head_length = head.size();
  result.keep_alive = keep_alive;
  return result;
}

task<BodyResult> read_request_body(Stream& stream, std::string& buffer, std::size_t head_length,
                                   Request& request, const Limits& limits, Deadline deadline) {
  BodyResult result;
  if (head_length > buffer.size()) {
    result.status = ReadStatus::Bad;
    result.message = "internal buffer error";
    co_return result;
  }
  buffer.erase(0, head_length);
  request.body.clear();

  if (request.chunked) {
    ChunkedDecoder decoder;
    for (;;) {
      std::size_t consumed = 0;
      auto chunk = decoder.feed(buffer, request.body, limits.max_body_bytes, limits, consumed);
      buffer.erase(0, consumed);
      if (chunk == ChunkedDecoder::Result::Done) co_return result;
      if (chunk == ChunkedDecoder::Result::Bad) {
        result.status = ReadStatus::Bad;
        result.message = "malformed chunked body";
        co_return result;
      }
      if (chunk == ChunkedDecoder::Result::TooLarge) {
        result.status = ReadStatus::TooLarge;
        result.message = "request body too large";
        co_return result;
      }
      auto status = co_await read_more(stream, buffer, deadline);
      if (status != ReadStatus::Ok) {
        result.status = status == ReadStatus::Eof ? ReadStatus::Bad : status;
        result.message = status == ReadStatus::Eof ? "truncated chunked body" : "body read error";
        co_return result;
      }
    }
  }

  if (request.content_length > limits.max_body_bytes) {
    result.status = ReadStatus::TooLarge;
    result.message = "request body too large";
    co_return result;
  }

  while (request.body.size() < request.content_length) {
    if (buffer.empty()) {
      auto status = co_await read_more(stream, buffer, deadline);
      if (status == ReadStatus::Eof) {
        result.status = ReadStatus::Bad;
        result.message = "truncated request body";
        co_return result;
      }
      if (status != ReadStatus::Ok) {
        result.status = status;
        result.message = "body read error";
        co_return result;
      }
      continue;
    }
    auto wanted = static_cast<std::size_t>(request.content_length - request.body.size());
    auto take = std::min(wanted, buffer.size());
    request.body.append(buffer, 0, take);
    buffer.erase(0, take);
  }

  co_return result;
}

task<BodyResult> read_response_body(Stream& stream, std::string& buffer, std::size_t head_length,
                                    Response& response, const Limits& limits, Deadline deadline,
                                    bool request_was_head, bool& close_after) {
  BodyResult result;
  if (head_length > buffer.size()) {
    result.status = ReadStatus::Bad;
    result.message = "internal buffer error";
    co_return result;
  }
  buffer.erase(0, head_length);
  close_after = response.close_connection;

  std::string body;
  if (request_was_head || response.status == 204 || response.status == 304 ||
      (response.status >= 100 && response.status < 200)) {
    response.body = Body(std::string());
    co_return result;
  }

  auto transfer_encoding = response.headers.get("transfer-encoding");
  bool chunked = Headers::equals(trim_ows(transfer_encoding), "chunked");
  auto content_length_header = response.headers.get("content-length");

  if (chunked) {
    ChunkedDecoder decoder;
    for (;;) {
      std::size_t consumed = 0;
      auto chunk = decoder.feed(buffer, body, limits.max_body_bytes, limits, consumed);
      buffer.erase(0, consumed);
      if (chunk == ChunkedDecoder::Result::Done) {
        response.body = Body(std::move(body));
        co_return result;
      }
      if (chunk == ChunkedDecoder::Result::Bad) {
        result.status = ReadStatus::Bad;
        result.message = "malformed chunked response body";
        close_after = true;
        co_return result;
      }
      if (chunk == ChunkedDecoder::Result::TooLarge) {
        result.status = ReadStatus::TooLarge;
        result.message = "response body too large";
        close_after = true;
        co_return result;
      }
      auto status = co_await read_more(stream, buffer, deadline);
      if (status != ReadStatus::Ok) {
        result.status = status == ReadStatus::Eof ? ReadStatus::Bad : status;
        result.message = status == ReadStatus::Eof ? "truncated chunked response" : "body read error";
        close_after = true;
        co_return result;
      }
    }
  }

  if (!content_length_header.empty()) {
    std::uint64_t length = 0;
    auto value = trim_ows(content_length_header);
    auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), length);
    if (ec != std::errc() || ptr != value.data() + value.size() || length > limits.max_body_bytes) {
      result.status = ReadStatus::TooLarge;
      result.message = "invalid or oversized Content-Length";
      close_after = true;
      co_return result;
    }
    body.reserve(static_cast<std::size_t>(length));
    while (body.size() < length) {
      if (buffer.empty()) {
        auto status = co_await read_more(stream, buffer, deadline);
        if (status != ReadStatus::Ok) {
          result.status = status == ReadStatus::Eof ? ReadStatus::Bad : status;
          result.message = "truncated response body";
          close_after = true;
          co_return result;
        }
        continue;
      }
      auto wanted = static_cast<std::size_t>(length - body.size());
      auto take = std::min(wanted, buffer.size());
      body.append(buffer, 0, take);
      buffer.erase(0, take);
    }
    response.body = Body(std::move(body));
    co_return result;
  }

  // No framing: read until EOF, and the connection cannot be reused.
  close_after = true;
  for (;;) {
    if (body.size() + buffer.size() > limits.max_body_bytes) {
      result.status = ReadStatus::TooLarge;
      result.message = "response body too large";
      co_return result;
    }
    body += buffer;
    buffer.clear();
    auto status = co_await read_more(stream, buffer, deadline);
    if (status == ReadStatus::Eof) break;
    if (status != ReadStatus::Ok) {
      result.status = status;
      result.message = "body read error";
      co_return result;
    }
  }
  response.body = Body(std::move(body));
  co_return result;
}

}  // namespace ferry::detail
