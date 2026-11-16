// ferry HTTP value types: headers, bodies, requests, responses, cookies.

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ferry/task.hpp"

namespace ferry {

struct MultipartPart;
struct MultipartResult;
class Stream;

enum class Method {
  Get,
  Head,
  Post,
  Put,
  Delete,
  Connect,
  Options,
  Trace,
  Patch,
  Other,
};

Method method_from_string(std::string_view name);
std::string_view method_name(Method method);
std::string_view reason_phrase(int status);

class Headers {
 public:
  using Entry = std::pair<std::string, std::string>;

  void set(std::string name, std::string value);
  void add(std::string name, std::string value);

  bool has(std::string_view name) const noexcept;
  std::string_view get(std::string_view name) const noexcept;
  std::vector<std::string_view> get_all(std::string_view name) const;
  bool remove(std::string_view name);

  std::size_t size() const noexcept { return entries_.size(); }
  bool empty() const noexcept { return entries_.empty(); }
  const std::vector<Entry>& entries() const noexcept { return entries_; }
  std::vector<Entry>& entries() noexcept { return entries_; }

  static bool equals(std::string_view a, std::string_view b) noexcept;

 private:
  std::vector<Entry> entries_;
};

class Body {
 public:
  // Return true and fill `chunk` with the next piece, or false at end.
  using ChunkProducer = std::function<task<bool>(std::string& chunk)>;

  enum class Kind { Empty, Owned, Borrowed, File, Stream };

  struct FileSpec {
    std::string path;
    std::uint64_t offset = 0;
    std::uint64_t length = 0;
    bool length_known = false;
  };

  Body() = default;
  Body(std::string text) : kind_(Kind::Owned), owned_(std::move(text)) {}
  Body(const char* text) : kind_(Kind::Owned), owned_(text ? text : "") {}

  static Body borrowed(std::string_view text);
  static Body file(std::string path);
  static Body file_range(std::string path, std::uint64_t offset, std::uint64_t length);
  static Body stream(ChunkProducer producer);

  Kind kind() const noexcept { return kind_; }
  bool empty() const noexcept;

  // Valid for Owned/Borrowed.
  std::string_view view() const noexcept;

  // Valid for Owned.
  std::string& string() noexcept { return owned_; }
  const std::string& string() const noexcept { return owned_; }

  const FileSpec* file() const noexcept;
  const ChunkProducer* producer() const noexcept;

  // Total body size if known, otherwise static_cast<uint64_t>(-1).
  std::uint64_t size() const noexcept;

  static constexpr std::uint64_t unknown_size = static_cast<std::uint64_t>(-1);

 private:
  Kind kind_ = Kind::Empty;
  std::string owned_;
  std::string_view borrowed_{};
  std::shared_ptr<FileSpec> file_;
  std::shared_ptr<ChunkProducer> producer_;
};

struct CookieOptions {
  std::string path = "/";
  std::string domain;
  bool secure = false;
  bool http_only = true;
  bool has_max_age = false;
  std::int64_t max_age_seconds = 0;
  std::string same_site = "Lax";
};

struct Request {
  Method method = Method::Get;
  std::string method_raw = "GET";
  std::string target = "/";
  std::string path = "/";
  std::string query_string;
  std::string version = "HTTP/1.1";
  Headers headers;
  std::string body;
  std::string remote_address;
  std::string local_address;
  bool secure = false;
  std::string tls_version;
  bool chunked = false;
  std::uint64_t content_length = 0;
  Headers trailers;
  std::vector<std::pair<std::string, std::string>> route_params;

  std::string_view header(std::string_view name) const noexcept { return headers.get(name); }
  bool has_header(std::string_view name) const noexcept { return headers.has(name); }

  std::string_view param(std::string_view name) const noexcept;
  std::string_view query(std::string_view name) const;
  const std::vector<std::pair<std::string, std::string>>& query_params() const;
  std::string_view cookie(std::string_view name) const;
  std::string_view form(std::string_view name) const;

  // multipart/form-data helpers (parsed lazily, cached per request).
  const MultipartResult& multipart() const;
  const MultipartPart* part(std::string_view name) const;

  bool keep_alive() const noexcept;
  bool expects_continue() const noexcept;
  std::string_view content_type() const noexcept { return headers.get("content-type"); }
  std::string_view user_agent() const noexcept { return headers.get("user-agent"); }

 private:
  mutable bool query_parsed_ = false;
  mutable std::vector<std::pair<std::string, std::string>> query_cache_;
  mutable bool cookies_parsed_ = false;
  mutable std::vector<std::pair<std::string, std::string>> cookie_cache_;
  mutable bool form_parsed_ = false;
  mutable std::vector<std::pair<std::string, std::string>> form_cache_;
  mutable std::shared_ptr<MultipartResult> multipart_cache_;

  void parse_query() const;
  void parse_cookies() const;
  void parse_form() const;
};

struct Response {
  int status = 200;
  std::string reason;
  Headers headers;
  Body body;
  Headers trailers;
  bool close_connection = false;

  // Set by protocol upgrades (currently WebSocket). The server writes the
  // response head, then hands the raw stream and unread buffer to this task.
  std::function<task<void>(Request&, Stream&, std::string&)> upgrade;

  // Client transport failures set status = 0 and fill this.
  std::string error;

  Response() = default;
  Response(int status_code, std::string text,
           std::string_view content_type = "text/plain; charset=utf-8");

  static Response json(std::string_view json, int status_code = 200);
  static Response html(std::string_view html, int status_code = 200);
  static Response text(std::string_view text, int status_code = 200);
  static Response redirect(std::string location, int status_code = 302);
  static Response not_found(std::string_view message = "not found");
  static Response bad_request(std::string_view message = "bad request");
  static Response stream(std::string content_type, Body::ChunkProducer producer);
  static Response file(std::string path);
  static Response file_range(std::string path, std::uint64_t offset, std::uint64_t length);

  Response& set(std::string name, std::string value);
  Response& add(std::string name, std::string value);
  Response& set_content_type(std::string value);
  Response& set_cookie(std::string name, std::string value, CookieOptions options = {});
  Response& set_header_only(bool value) noexcept {
    header_only_ = value;
    return *this;
  }

  bool ok() const noexcept { return status >= 200 && status < 300; }
  bool header_only() const noexcept { return header_only_; }
  std::string_view text_body() const noexcept { return body.view(); }
  std::string_view header(std::string_view name) const noexcept { return headers.get(name); }

 private:
  bool header_only_ = false;
};

// Parses "a=1&b=2" into decoded pairs. `plus_as_space` follows form encoding.
std::vector<std::pair<std::string, std::string>> parse_pairs(std::string_view text,
                                                             bool plus_as_space);

}  // namespace ferry
