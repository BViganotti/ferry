// ferry multipart/form-data parsing.

#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "ferry/http.hpp"

namespace ferry {

struct MultipartPart {
  Headers headers;
  std::string name;
  std::string filename;
  std::string content_type;
  std::string body;

  std::string_view header(std::string_view key) const { return headers.get(key); }
};

struct MultipartResult {
  std::vector<MultipartPart> parts;
  std::string error;

  bool ok() const noexcept { return error.empty(); }
  bool empty() const noexcept { return parts.empty(); }

  const MultipartPart* find(std::string_view name) const {
    for (const auto& part : parts) {
      if (part.name == name) return &part;
    }
    return nullptr;
  }
};

// Parses multipart/form-data (or any multipart/*). Header values are validated;
// part bodies are copied. max_parts and max_header_bytes bound attacker input
// beyond the already-enforced request body limit.
MultipartResult parse_multipart(std::string_view body, std::string_view content_type,
                                std::size_t max_parts = 1024,
                                std::size_t max_header_bytes = 16 * 1024);

// Extracts the boundary parameter from a multipart Content-Type, if present.
std::string multipart_boundary(std::string_view content_type);

}  // namespace ferry
