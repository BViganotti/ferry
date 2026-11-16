// Internal HTTP/1.x wire codec shared by the client and server.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "ferry/http.hpp"
#include "ferry/net.hpp"

namespace ferry::detail {

struct Limits {
  std::size_t max_header_bytes = 64 * 1024;
  std::size_t max_body_bytes = 16 * 1024 * 1024;
  std::size_t max_headers = 100;
  std::size_t max_request_line = 8 * 1024;
  std::size_t max_header_line = 8 * 1024;
  std::size_t max_chunk_line = 4 * 1024;
  std::size_t max_trailer_bytes = 8 * 1024;
};

enum class ReadStatus {
  Ok,
  Eof,
  Timeout,
  Cancelled,
  Error,
  TooLarge,
  Bad,
  Unsupported,
};

struct HeadResult {
  ReadStatus status = ReadStatus::Ok;
  std::string message;
};

// Reads until "\r\n\r\n" appears, appending to buffer. Leaves any bytes after
// the terminator in buffer.
task<HeadResult> read_head(Stream& stream, std::string& buffer, std::size_t max_header_bytes,
                           Deadline deadline);

struct ParseResult {
  bool ok = false;
  int http_status = 400;
  std::string message;
  std::size_t head_length = 0;
  bool keep_alive = true;
};

ParseResult parse_request_head(std::string_view head, Request& request, const Limits& limits);
ParseResult parse_response_head(std::string_view head, Response& response, const Limits& limits);

struct BodyResult {
  ReadStatus status = ReadStatus::Ok;
  std::string message;
};

// Consumes the head from buffer, then reads/dechunks the body. Remaining
// pipelined bytes stay in buffer.
task<BodyResult> read_request_body(Stream& stream, std::string& buffer, std::size_t head_length,
                                   Request& request, const Limits& limits, Deadline deadline);

task<BodyResult> read_response_body(Stream& stream, std::string& buffer, std::size_t head_length,
                                    Response& response, const Limits& limits, Deadline deadline,
                                    bool request_was_head, bool& close_after);

bool is_token(std::string_view text) noexcept;
bool is_valid_header_value(std::string_view text) noexcept;

}  // namespace ferry::detail
