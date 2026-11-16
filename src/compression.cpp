#include "compression.hpp"

#include <cstring>

#if defined(FERRY_HAS_ZLIB)
#include <zlib.h>
#endif

namespace ferry::detail {

#if defined(FERRY_HAS_ZLIB)

namespace {
constexpr int kGzipWindowBits = 15 + 16;
constexpr std::size_t kBufferSize = 64 * 1024;
}  // namespace

bool compression_available() noexcept { return true; }

std::string gzip_compress(std::string_view data) {
  z_stream stream{};
  if (deflateInit2(&stream, 6, Z_DEFLATED, kGzipWindowBits, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
    return {};
  }

  stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.data()));
  stream.avail_in = static_cast<uInt>(data.size());

  std::string output;
  char buffer[kBufferSize];
  int result = Z_OK;
  do {
    stream.next_out = reinterpret_cast<Bytef*>(buffer);
    stream.avail_out = sizeof buffer;
    result = deflate(&stream, Z_FINISH);
    if (result != Z_OK && result != Z_STREAM_END) {
      deflateEnd(&stream);
      return {};
    }
    output.append(buffer, sizeof buffer - stream.avail_out);
  } while (result != Z_STREAM_END);

  deflateEnd(&stream);
  if (output.size() >= data.size()) return {};
  return output;
}

std::optional<std::string> gzip_decompress(std::string_view data, std::size_t max_size) {
  z_stream stream{};
  if (inflateInit2(&stream, kGzipWindowBits) != Z_OK) return std::nullopt;

  stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.data()));
  stream.avail_in = static_cast<uInt>(data.size());

  std::string output;
  char buffer[kBufferSize];
  int result = Z_OK;
  while (result != Z_STREAM_END) {
    stream.next_out = reinterpret_cast<Bytef*>(buffer);
    stream.avail_out = sizeof buffer;
    result = inflate(&stream, Z_NO_FLUSH);
    if (result != Z_OK && result != Z_STREAM_END) {
      inflateEnd(&stream);
      return std::nullopt;
    }
    std::size_t produced = sizeof buffer - stream.avail_out;
    if (output.size() + produced > max_size) {
      inflateEnd(&stream);
      return std::nullopt;
    }
    output.append(buffer, produced);
    if (result == Z_BUF_ERROR && produced == 0) {
      inflateEnd(&stream);
      return std::nullopt;
    }
  }
  inflateEnd(&stream);
  return output;
}

#else

bool compression_available() noexcept { return false; }

std::string gzip_compress(std::string_view) { return {}; }

std::optional<std::string> gzip_decompress(std::string_view, std::size_t) {
  return std::nullopt;
}

#endif

}  // namespace ferry::detail
