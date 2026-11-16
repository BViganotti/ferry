#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace ferry::detail {

bool compression_available() noexcept;
std::string gzip_compress(std::string_view data);
std::optional<std::string> gzip_decompress(std::string_view data, std::size_t max_size);

}  // namespace ferry::detail
