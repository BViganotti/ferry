#include "ferry/url.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdio>
#include <string>
#include <system_error>
#include <vector>

namespace ferry {

namespace {

bool is_scheme_char(char c) {
  return std::isalnum(static_cast<unsigned char>(c)) || c == '+' || c == '-' || c == '.';
}

bool has_control(std::string_view text) {
  for (char c : text) {
    unsigned char u = static_cast<unsigned char>(c);
    if (u <= 0x20 || u == 0x7f) return true;
  }
  return false;
}

std::string remove_dot_segments(std::string path) {
  std::vector<std::string> output;
  std::size_t index = 0;
  while (index < path.size()) {
    std::size_t next = path.find('/', index);
    if (next == std::string::npos) next = path.size();
    std::string segment = path.substr(index, next - index);
    if (segment == "..") {
      if (!output.empty()) output.pop_back();
    } else if (segment != "." && !segment.empty()) {
      output.push_back(std::move(segment));
    }
    index = next == path.size() ? path.size() : next + 1;
    if (next == path.size()) break;
  }
  std::string result;
  for (const auto& segment : output) {
    result += '/';
    result += segment;
  }
  if (path == "/" || (!path.empty() && path.back() == '/')) {
    if (result.empty() || result.back() != '/') result += '/';
  }
  if (result.empty()) result = "/";
  return result;
}

std::string merge_path(const std::string& base_path, std::string_view reference) {
  if (reference.empty()) {
    return base_path.empty() ? "/" : base_path;
  }
  if (reference.front() == '/') return std::string(reference);
  auto slash = base_path.rfind('/');
  if (slash == std::string::npos) return "/" + std::string(reference);
  return base_path.substr(0, slash + 1) + std::string(reference);
}

}  // namespace

std::optional<Url> Url::parse(std::string_view text) {
  if (text.empty() || has_control(text)) return std::nullopt;

  auto colon = text.find(':');
  if (colon == std::string_view::npos || colon == 0) return std::nullopt;
  for (std::size_t i = 0; i < colon; ++i) {
    if (!is_scheme_char(text[i])) return std::nullopt;
  }
  if (!std::isalpha(static_cast<unsigned char>(text[0]))) return std::nullopt;

  Url url;
  url.scheme = std::string(text.substr(0, colon));
  std::transform(url.scheme.begin(), url.scheme.end(), url.scheme.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

  std::size_t cursor = colon + 1;
  if (cursor + 1 >= text.size() || text[cursor] != '/' || text[cursor + 1] != '/') {
    return std::nullopt;
  }
  cursor += 2;

  std::size_t authority_end = text.find_first_of("/?#", cursor);
  if (authority_end == std::string_view::npos) authority_end = text.size();
  std::string_view authority = text.substr(cursor, authority_end - cursor);
  if (authority.empty()) return std::nullopt;

  auto at = authority.rfind('@');
  if (at != std::string_view::npos) {
    url.userinfo = std::string(authority.substr(0, at));
    authority.remove_prefix(at + 1);
  }
  if (authority.empty()) return std::nullopt;

  std::string_view host_part;
  std::string_view port_part;
  if (authority.front() == '[') {
    auto bracket = authority.find(']');
    if (bracket == std::string_view::npos) return std::nullopt;
    host_part = authority.substr(0, bracket + 1);
    if (bracket + 1 < authority.size()) {
      if (authority[bracket + 1] != ':') return std::nullopt;
      port_part = authority.substr(bracket + 2);
    }
    url.host = std::string(host_part.substr(1, host_part.size() - 2));
  } else {
    auto port_colon = authority.rfind(':');
    if (port_colon != std::string_view::npos) {
      host_part = authority.substr(0, port_colon);
      port_part = authority.substr(port_colon + 1);
    } else {
      host_part = authority;
    }
    url.host = std::string(host_part);
  }
  if (url.host.empty()) return std::nullopt;

  if (!port_part.empty()) {
    unsigned int value = 0;
    auto [ptr, ec] = std::from_chars(port_part.data(), port_part.data() + port_part.size(), value);
    if (ec != std::errc() || ptr != port_part.data() + port_part.size() || value == 0 ||
        value > 65535) {
      return std::nullopt;
    }
    url.port = static_cast<uint16_t>(value);
  }

  std::size_t path_end = text.find_first_of("?#", authority_end);
  if (path_end == std::string_view::npos) path_end = text.size();
  if (authority_end < text.size() && text[authority_end] == '/') {
    url.path = std::string(text.substr(authority_end, path_end - authority_end));
  } else {
    url.path = "/";
  }

  std::size_t query_start = text.find('?', path_end);
  std::size_t fragment_start = text.find('#', path_end);
  if (query_start != std::string_view::npos &&
      (fragment_start == std::string_view::npos || query_start < fragment_start)) {
    std::size_t query_end = fragment_start == std::string_view::npos ? text.size() : fragment_start;
    url.query = std::string(text.substr(query_start + 1, query_end - query_start - 1));
  }
  if (fragment_start != std::string_view::npos) {
    url.fragment = std::string(text.substr(fragment_start + 1));
  }

  return url;
}

uint16_t Url::effective_port() const noexcept {
  if (port != 0) return port;
  if (scheme == "https") return 443;
  if (scheme == "http") return 80;
  return 0;
}

std::string Url::origin() const {
  std::string result = scheme + "://";
  if (host.find(':') != std::string::npos) {
    result += '[' + host + ']';
  } else {
    result += host;
  }
  auto default_port = scheme == "https" ? 443 : (scheme == "http" ? 80 : 0);
  if (port != 0 && port != default_port) {
    result += ':' + std::to_string(port);
  }
  return result;
}

std::string Url::target() const {
  std::string result = path.empty() ? "/" : path;
  if (!query.empty()) {
    result += '?';
    result += query;
  }
  return result;
}

std::string Url::host_header() const {
  std::string result;
  if (host.find(':') != std::string::npos) {
    result = '[' + host + ']';
  } else {
    result = host;
  }
  auto default_port = scheme == "https" ? 443 : (scheme == "http" ? 80 : 0);
  if (port != 0 && port != default_port) {
    result += ':' + std::to_string(port);
  }
  return result;
}

std::string Url::to_string() const {
  std::string result = origin();
  result += path.empty() ? "/" : path;
  if (!query.empty()) {
    result += '?';
    result += query;
  }
  if (!fragment.empty()) {
    result += '#';
    result += fragment;
  }
  return result;
}

std::string percent_decode(std::string_view text, bool plus_as_space) {
  std::string result;
  result.reserve(text.size());
  for (std::size_t i = 0; i < text.size(); ++i) {
    char c = text[i];
    if (c == '%' && i + 2 < text.size()) {
      auto hex = [](char h) -> int {
        if (h >= '0' && h <= '9') return h - '0';
        if (h >= 'a' && h <= 'f') return h - 'a' + 10;
        if (h >= 'A' && h <= 'F') return h - 'A' + 10;
        return -1;
      };
      int high = hex(text[i + 1]);
      int low = hex(text[i + 2]);
      if (high >= 0 && low >= 0) {
        result.push_back(static_cast<char>((high << 4) | low));
        i += 2;
        continue;
      }
    }
    if (plus_as_space && c == '+') {
      result.push_back(' ');
    } else {
      result.push_back(c);
    }
  }
  return result;
}

std::string percent_encode(std::string_view text) {
  static const char* hex = "0123456789ABCDEF";
  std::string result;
  result.reserve(text.size());
  for (unsigned char c : text) {
    if (std::isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~') {
      result.push_back(static_cast<char>(c));
    } else {
      result.push_back('%');
      result.push_back(hex[c >> 4]);
      result.push_back(hex[c & 0x0f]);
    }
  }
  return result;
}

std::string resolve_url(const Url& base, std::string_view reference) {
  if (reference.empty()) {
    Url copy = base;
    copy.fragment.clear();
    return copy.to_string();
  }

  auto hash = reference.find('#');
  std::string_view without_fragment =
      hash == std::string_view::npos ? reference : reference.substr(0, hash);
  std::string fragment = hash == std::string_view::npos
                             ? std::string()
                             : std::string(reference.substr(hash + 1));

  if (without_fragment.find(':') != std::string_view::npos) {
    auto colon = without_fragment.find(':');
    bool looks_like_scheme = colon > 0 && std::isalpha(static_cast<unsigned char>(without_fragment[0]));
    for (std::size_t i = 1; i < colon && looks_like_scheme; ++i) {
      if (!is_scheme_char(without_fragment[i])) looks_like_scheme = false;
    }
    if (looks_like_scheme) {
      auto parsed = Url::parse(without_fragment);
      if (!parsed) return {};
      parsed->fragment = fragment;
      return parsed->to_string();
    }
  }

  Url result = base;
  result.fragment = fragment;

  if (without_fragment.starts_with("//")) {
    auto parsed = Url::parse(base.scheme + ":" + std::string(without_fragment));
    if (!parsed) return {};
    parsed->fragment = fragment;
    return parsed->to_string();
  }

  if (without_fragment.empty() || without_fragment.front() == '#') {
    result.fragment = fragment;
    return result.to_string();
  }

  auto query = without_fragment.find('?');
  std::string_view path_part =
      query == std::string_view::npos ? without_fragment : without_fragment.substr(0, query);
  std::string query_part =
      query == std::string_view::npos ? std::string() : std::string(without_fragment.substr(query + 1));

  if (!path_part.empty() && path_part.front() == '/') {
    result.path = remove_dot_segments(std::string(path_part));
    result.query = std::move(query_part);
  } else if (path_part.empty()) {
    result.query = query_part.empty() ? base.query : std::move(query_part);
  } else {
    result.path = remove_dot_segments(merge_path(base.path, path_part));
    result.query = std::move(query_part);
  }

  return result.to_string();
}

}  // namespace ferry
