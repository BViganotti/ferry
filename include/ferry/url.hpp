// ferry::Url — RFC 3986 parsing and reference resolution (http/https focus).

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace ferry {

struct Url {
  std::string scheme;
  std::string userinfo;
  std::string host;
  uint16_t port = 0;
  std::string path = "/";
  std::string query;
  std::string fragment;

  static std::optional<Url> parse(std::string_view text);

  bool valid() const noexcept { return !scheme.empty() && !host.empty(); }
  bool is_tls() const noexcept { return scheme == "https"; }
  uint16_t effective_port() const noexcept;
  std::string origin() const;
  std::string target() const;  // path + "?query"
  std::string host_header() const;
  std::string to_string() const;
};

// Percent-decoding. Invalid escapes are left untouched.
std::string percent_decode(std::string_view text, bool plus_as_space = false);

// Percent-encodes everything except unreserved characters (RFC 3986).
std::string percent_encode(std::string_view text);

// Resolves a Location-style reference against a base URL. Returns an absolute
// URL string, or an empty string if it cannot be resolved.
std::string resolve_url(const Url& base, std::string_view reference);

}  // namespace ferry
