// ferry::CookieJar — RFC 6265-ish client cookie storage.

#pragma once

#include <cstddef>
#include <cstdint>
#include <ctime>
#include <string>
#include <string_view>
#include <vector>

#include "ferry/url.hpp"

namespace ferry {

struct Cookie {
  std::string name;
  std::string value;
  std::string domain;
  std::string path;
  bool host_only = true;
  bool secure = false;
  bool http_only = false;
  bool has_expiry = false;
  std::time_t expires = 0;
  std::uint64_t creation = 0;
};

class CookieJar {
 public:
  // Stores every Set-Cookie value for the URL. Invalid or cross-domain cookies
  // are ignored. Max-Age <= 0 and Expires in the past remove a cookie.
  void store(const Url& url, std::string_view set_cookie);

  // Returns a Cookie header value for the URL (sorted longest-path first).
  std::string header_for(const Url& url);

  void clear();
  std::size_t size();

 private:
  void purge_expired();
  void erase_matching(const std::string& name, const std::string& domain,
                      const std::string& path);

  std::vector<Cookie> cookies_;
  std::uint64_t next_creation_ = 1;
};

}  // namespace ferry
