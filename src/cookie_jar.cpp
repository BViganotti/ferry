#include "ferry/cookie_jar.hpp"

#include "ferry/http.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

namespace ferry {

namespace {

std::string_view trim(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
  return text;
}

std::string lower(std::string_view text) {
  std::string result(text);
  for (char& c : result) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return result;
}

bool is_token(std::string_view text) {
  if (text.empty()) return false;
  for (unsigned char c : text) {
    if (std::isalnum(c)) continue;
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
        break;
      default:
        return false;
    }
  }
  return true;
}

bool valid_value(std::string_view value) {
  for (unsigned char c : value) {
    if (c == ';' || c == '\r' || c == '\n' || c == '\0') return false;
    if (c < 0x20) return false;
  }
  return true;
}

bool domain_matches(std::string_view host, std::string_view domain, bool host_only) {
  if (host_only) return host == domain;
  if (host == domain) return true;
  if (host.size() <= domain.size()) return false;
  auto suffix = host.substr(host.size() - domain.size());
  return suffix == domain && host[host.size() - domain.size() - 1] == '.';
}

bool path_matches(std::string_view request_path, std::string_view cookie_path) {
  if (request_path == cookie_path) return true;
  if (!request_path.starts_with(cookie_path)) return false;
  if (cookie_path.empty()) return true;
  if (cookie_path.back() == '/') return true;
  return request_path.size() > cookie_path.size() && request_path[cookie_path.size()] == '/';
}

std::string default_path(std::string_view request_path) {
  if (request_path.empty() || request_path.front() != '/') return "/";
  auto slash = request_path.rfind('/');
  if (slash == std::string_view::npos || slash == 0) return "/";
  return std::string(request_path.substr(0, slash));
}

std::time_t parse_http_date(std::string_view text) {
  std::string copy(text);
  char weekday[32]{};
  char month[8]{};
  int day = 0;
  int year = 0;
  int hour = 0;
  int minute = 0;
  int second = 0;
  if (std::sscanf(copy.c_str(), "%31[^,], %d %7s %d %d:%d:%d", weekday, &day, month, &year, &hour,
                  &minute, &second) != 7) {
    return 0;
  }
  static const char* months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                 "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  int month_index = -1;
  for (int i = 0; i < 12; ++i) {
    if (std::strcmp(month, months[i]) == 0) {
      month_index = i;
      break;
    }
  }
  if (month_index < 0) return 0;
  std::tm tm{};
  tm.tm_mday = day;
  tm.tm_mon = month_index;
  tm.tm_year = year - 1900;
  tm.tm_hour = hour;
  tm.tm_min = minute;
  tm.tm_sec = second;
  return ::timegm(&tm);
}

}  // namespace

void CookieJar::purge_expired() {
  auto now = std::time(nullptr);
  cookies_.erase(std::remove_if(cookies_.begin(), cookies_.end(),
                                [now](const Cookie& cookie) {
                                  return cookie.has_expiry && cookie.expires <= now;
                                }),
                 cookies_.end());
}

void CookieJar::erase_matching(const std::string& name, const std::string& domain,
                               const std::string& path) {
  cookies_.erase(std::remove_if(cookies_.begin(), cookies_.end(),
                                [&](const Cookie& cookie) {
                                  return cookie.name == name && cookie.domain == domain &&
                                         cookie.path == path;
                                }),
                 cookies_.end());
}

void CookieJar::store(const Url& url, std::string_view set_cookie) {
  purge_expired();

  auto semicolon = set_cookie.find(';');
  auto pair = trim(semicolon == std::string_view::npos ? set_cookie
                                                       : set_cookie.substr(0, semicolon));
  auto equals = pair.find('=');
  if (equals == std::string_view::npos) return;
  auto name = trim(pair.substr(0, equals));
  auto value = trim(pair.substr(equals + 1));
  if (!is_token(name) || !valid_value(value)) return;

  std::string host = lower(url.host);
  std::string domain = host;
  bool host_only = true;
  std::string path = default_path(url.path);
  bool secure = false;
  bool http_only = false;
  bool has_expiry = false;
  std::time_t expires = 0;
  std::optional<std::int64_t> max_age;

  std::string_view attributes =
      semicolon == std::string_view::npos ? std::string_view() : set_cookie.substr(semicolon + 1);
  while (!attributes.empty()) {
    auto next = attributes.find(';');
    auto attribute = trim(next == std::string_view::npos ? attributes : attributes.substr(0, next));
    auto attribute_equals = attribute.find('=');
    auto key = trim(attribute.substr(0, attribute_equals));
    auto attribute_value =
        attribute_equals == std::string_view::npos ? std::string_view()
                                                   : trim(attribute.substr(attribute_equals + 1));

    if (Headers::equals(key, "Domain")) {
      auto candidate = lower(attribute_value);
      if (!candidate.empty() && candidate.front() == '.') candidate.erase(candidate.begin());
      if (!candidate.empty() && domain_matches(host, candidate, false)) {
        domain = std::move(candidate);
        host_only = false;
      }
    } else if (Headers::equals(key, "Path")) {
      if (!attribute_value.empty() && attribute_value.front() == '/') {
        path = std::string(attribute_value);
      }
    } else if (Headers::equals(key, "Secure")) {
      secure = true;
    } else if (Headers::equals(key, "HttpOnly")) {
      http_only = true;
    } else if (Headers::equals(key, "Max-Age")) {
      std::int64_t seconds = 0;
      auto [ptr, ec] = std::from_chars(attribute_value.data(),
                                       attribute_value.data() + attribute_value.size(), seconds);
      if (ec == std::errc() && ptr == attribute_value.data() + attribute_value.size()) {
        max_age = seconds;
        has_expiry = true;
      }
    } else if (Headers::equals(key, "Expires") && !max_age.has_value()) {
      auto parsed = parse_http_date(attribute_value);
      if (parsed != 0) {
        expires = parsed;
        has_expiry = true;
      }
    }

    if (next == std::string_view::npos) break;
    attributes.remove_prefix(next + 1);
  }

  if (max_age.has_value()) {
    if (*max_age <= 0) {
      expires = std::time(nullptr) - 1;
    } else {
      expires = std::time(nullptr) + static_cast<std::time_t>(*max_age);
    }
    has_expiry = true;
  }

  if (value.empty() || (has_expiry && expires <= std::time(nullptr))) {
    erase_matching(std::string(name), domain, path);
    return;
  }

  erase_matching(std::string(name), domain, path);
  Cookie cookie;
  cookie.name = std::string(name);
  cookie.value = std::string(value);
  cookie.domain = domain;
  cookie.path = path;
  cookie.host_only = host_only;
  cookie.secure = secure;
  cookie.http_only = http_only;
  cookie.has_expiry = has_expiry;
  cookie.expires = expires;
  cookie.creation = next_creation_++;
  cookies_.push_back(std::move(cookie));

  // Bound memory even when a server keeps setting unique cookies.
  constexpr std::size_t kMaxCookies = 3000;
  if (cookies_.size() > kMaxCookies) {
    auto oldest = std::min_element(cookies_.begin(), cookies_.end(),
                                   [](const Cookie& a, const Cookie& b) {
                                     return a.creation < b.creation;
                                   });
    if (oldest != cookies_.end()) cookies_.erase(oldest);
  }
}

std::string CookieJar::header_for(const Url& url) {
  purge_expired();
  if (url.host.empty()) return {};

  std::string host = lower(url.host);
  bool secure_request = url.is_tls();
  std::vector<const Cookie*> matching;
  for (const auto& cookie : cookies_) {
    if (cookie.secure && !secure_request) continue;
    if (!domain_matches(host, cookie.domain, cookie.host_only)) continue;
    if (!path_matches(url.path, cookie.path)) continue;
    matching.push_back(&cookie);
  }

  std::sort(matching.begin(), matching.end(), [](const Cookie* a, const Cookie* b) {
    if (a->path.size() != b->path.size()) return a->path.size() > b->path.size();
    return a->creation < b->creation;
  });

  std::string result;
  for (const auto* cookie : matching) {
    if (!result.empty()) result += "; ";
    result += cookie->name;
    result += '=';
    result += cookie->value;
  }
  return result;
}

void CookieJar::clear() { cookies_.clear(); }

std::size_t CookieJar::size() {
  purge_expired();
  return cookies_.size();
}

}  // namespace ferry
