#include "ferry/multipart.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "codec.hpp"

namespace ferry {

namespace {

std::string_view trim(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
  return text;
}

bool starts_with_ci(std::string_view text, std::string_view prefix) {
  if (text.size() < prefix.size()) return false;
  return Headers::equals(text.substr(0, prefix.size()), prefix);
}

struct Parameter {
  std::string key;
  std::string value;
};

std::vector<Parameter> parse_parameters(std::string_view text) {
  std::vector<Parameter> parameters;
  // Skip the media type / disposition token.
  auto first_semicolon = text.find(';');
  if (first_semicolon == std::string_view::npos) return parameters;
  text.remove_prefix(first_semicolon + 1);

  while (!text.empty()) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == ';')) {
      text.remove_prefix(1);
    }
    if (text.empty()) break;

    auto equals = text.find('=');
    if (equals == std::string_view::npos) break;
    Parameter parameter;
    parameter.key = std::string(trim(text.substr(0, equals)));
    text.remove_prefix(equals + 1);

    if (!text.empty() && text.front() == '"') {
      text.remove_prefix(1);
      std::string value;
      bool closed = false;
      while (!text.empty()) {
        char c = text.front();
        text.remove_prefix(1);
        if (c == '\\' && !text.empty()) {
          value.push_back(text.front());
          text.remove_prefix(1);
        } else if (c == '"') {
          closed = true;
          break;
        } else {
          value.push_back(c);
        }
      }
      if (!closed) break;
      parameter.value = std::move(value);
      auto semicolon = text.find(';');
      text = semicolon == std::string_view::npos ? std::string_view() : text.substr(semicolon + 1);
    } else {
      auto semicolon = text.find(';');
      parameter.value =
          std::string(trim(semicolon == std::string_view::npos ? text : text.substr(0, semicolon)));
      text = semicolon == std::string_view::npos ? std::string_view() : text.substr(semicolon + 1);
    }
    parameters.push_back(std::move(parameter));
  }
  return parameters;
}

std::string parameter_value(const std::vector<Parameter>& parameters, std::string_view key) {
  for (const auto& parameter : parameters) {
    if (Headers::equals(parameter.key, key)) return parameter.value;
  }
  return {};
}

}  // namespace

std::string multipart_boundary(std::string_view content_type) {
  if (!starts_with_ci(content_type, "multipart/")) return {};
  auto parameters = parse_parameters(content_type);
  std::string boundary = parameter_value(parameters, "boundary");
  if (boundary.empty() || boundary.size() > 70) return {};
  for (unsigned char c : boundary) {
    if (c < 0x20 || c == 0x7f) return {};
  }
  return boundary;
}

MultipartResult parse_multipart(std::string_view body, std::string_view content_type,
                                std::size_t max_parts, std::size_t max_header_bytes) {
  MultipartResult result;
  if (!starts_with_ci(content_type, "multipart/")) {
    result.error = "content-type is not multipart";
    return result;
  }

  std::string boundary = multipart_boundary(content_type);
  if (boundary.empty()) {
    result.error = "missing or invalid multipart boundary";
    return result;
  }

  const std::string delimiter = "--" + boundary;
  std::size_t cursor = body.find(delimiter);
  if (cursor == std::string_view::npos) {
    result.error = "multipart boundary not found";
    return result;
  }

  for (;;) {
    cursor += delimiter.size();
    if (cursor + 2 <= body.size() && body.substr(cursor, 2) == "--") {
      return result;
    }
    if (cursor + 2 > body.size() || body.substr(cursor, 2) != "\r\n") {
      result.error = "malformed multipart delimiter";
      return result;
    }
    cursor += 2;

    auto header_end = body.find("\r\n\r\n", cursor);
    if (header_end == std::string_view::npos) {
      result.error = "unterminated multipart headers";
      return result;
    }
    if (header_end - cursor > max_header_bytes) {
      result.error = "multipart headers too large";
      return result;
    }

    if (result.parts.size() >= max_parts) {
      result.error = "too many multipart parts";
      return result;
    }

    MultipartPart part;
    std::string_view header_block = body.substr(cursor, header_end - cursor);
    while (!header_block.empty()) {
      auto crlf = header_block.find("\r\n");
      auto line = crlf == std::string_view::npos ? header_block : header_block.substr(0, crlf);
      header_block = crlf == std::string_view::npos ? std::string_view()
                                                    : header_block.substr(crlf + 2);
      if (line.empty()) break;
      auto colon = line.find(':');
      if (colon == std::string_view::npos || colon == 0) {
        result.error = "malformed multipart header";
        result.parts.clear();
        return result;
      }
      auto name = line.substr(0, colon);
      if (!detail::is_token(name)) {
        result.error = "invalid multipart header name";
        result.parts.clear();
        return result;
      }
      auto value = trim(line.substr(colon + 1));
      if (!detail::is_valid_header_value(value)) {
        result.error = "invalid multipart header value";
        result.parts.clear();
        return result;
      }
      try {
        part.headers.add(std::string(name), std::string(value));
      } catch (const std::exception&) {
        result.error = "invalid multipart header";
        result.parts.clear();
        return result;
      }
    }

    auto disposition = part.headers.get("content-disposition");
    if (!disposition.empty()) {
      auto parameters = parse_parameters(disposition);
      part.name = parameter_value(parameters, "name");
      part.filename = parameter_value(parameters, "filename");
    }
    part.content_type = std::string(part.headers.get("content-type"));

    auto data_start = header_end + 4;
    std::string needle = "\r\n" + delimiter;
    auto next = body.find(needle, data_start);
    if (next == std::string_view::npos) {
      result.error = "unterminated multipart part";
      result.parts.clear();
      return result;
    }
    part.body = std::string(body.substr(data_start, next - data_start));
    result.parts.push_back(std::move(part));
    cursor = next + 2;
  }
}

}  // namespace ferry
