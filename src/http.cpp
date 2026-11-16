#include "ferry/http.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

#include "ferry/multipart.hpp"
#include "ferry/url.hpp"

namespace ferry {

namespace {

bool is_tchar(unsigned char c) {
  if (std::isalnum(c)) return true;
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
      return true;
    default:
      return false;
  }
}

void validate_header_name(std::string_view name) {
  if (name.empty()) throw std::invalid_argument("ferry: empty header name");
  for (unsigned char c : name) {
    if (!is_tchar(c)) {
      throw std::invalid_argument("ferry: invalid header name");
    }
  }
}

void validate_header_value(std::string_view value) {
  for (unsigned char c : value) {
    if (c == '\r' || c == '\n' || c == '\0') {
      throw std::invalid_argument("ferry: invalid character in header value");
    }
  }
}

std::string_view trim_ows(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
  return text;
}

bool contains_token(std::string_view list, std::string_view token) {
  while (!list.empty()) {
    auto comma = list.find(',');
    auto item = trim_ows(comma == std::string_view::npos ? list : list.substr(0, comma));
    if (Headers::equals(item, token)) return true;
    if (comma == std::string_view::npos) break;
    list.remove_prefix(comma + 1);
  }
  return false;
}

std::string percent_decode_view(std::string_view text, bool plus) {
  return percent_decode(text, plus);
}

}  // namespace

bool Headers::equals(std::string_view a, std::string_view b) noexcept {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    unsigned char ca = static_cast<unsigned char>(a[i]);
    unsigned char cb = static_cast<unsigned char>(b[i]);
    if (std::tolower(ca) != std::tolower(cb)) return false;
  }
  return true;
}

void Headers::set(std::string name, std::string value) {
  validate_header_name(name);
  validate_header_value(value);
  bool replaced = false;
  for (auto it = entries_.begin(); it != entries_.end();) {
    if (equals(it->first, name)) {
      if (!replaced) {
        it->first = name;
        it->second = value;
        replaced = true;
        ++it;
      } else {
        it = entries_.erase(it);
      }
    } else {
      ++it;
    }
  }
  if (!replaced) entries_.emplace_back(std::move(name), std::move(value));
}

void Headers::add(std::string name, std::string value) {
  validate_header_name(name);
  validate_header_value(value);
  entries_.emplace_back(std::move(name), std::move(value));
}

bool Headers::has(std::string_view name) const noexcept {
  for (const auto& entry : entries_) {
    if (equals(entry.first, name)) return true;
  }
  return false;
}

std::string_view Headers::get(std::string_view name) const noexcept {
  for (const auto& entry : entries_) {
    if (equals(entry.first, name)) return entry.second;
  }
  return {};
}

std::vector<std::string_view> Headers::get_all(std::string_view name) const {
  std::vector<std::string_view> values;
  for (const auto& entry : entries_) {
    if (equals(entry.first, name)) values.push_back(entry.second);
  }
  return values;
}

bool Headers::remove(std::string_view name) {
  bool removed = false;
  for (auto it = entries_.begin(); it != entries_.end();) {
    if (equals(it->first, name)) {
      it = entries_.erase(it);
      removed = true;
    } else {
      ++it;
    }
  }
  return removed;
}

Method method_from_string(std::string_view name) {
  if (name == "GET") return Method::Get;
  if (name == "HEAD") return Method::Head;
  if (name == "POST") return Method::Post;
  if (name == "PUT") return Method::Put;
  if (name == "DELETE") return Method::Delete;
  if (name == "CONNECT") return Method::Connect;
  if (name == "OPTIONS") return Method::Options;
  if (name == "TRACE") return Method::Trace;
  if (name == "PATCH") return Method::Patch;
  return Method::Other;
}

std::string_view method_name(Method method) {
  switch (method) {
    case Method::Get:
      return "GET";
    case Method::Head:
      return "HEAD";
    case Method::Post:
      return "POST";
    case Method::Put:
      return "PUT";
    case Method::Delete:
      return "DELETE";
    case Method::Connect:
      return "CONNECT";
    case Method::Options:
      return "OPTIONS";
    case Method::Trace:
      return "TRACE";
    case Method::Patch:
      return "PATCH";
    case Method::Other:
      return "OTHER";
  }
  return "OTHER";
}

std::string_view reason_phrase(int status) {
  switch (status) {
    case 100:
      return "Continue";
    case 101:
      return "Switching Protocols";
    case 200:
      return "OK";
    case 201:
      return "Created";
    case 202:
      return "Accepted";
    case 203:
      return "Non-Authoritative Information";
    case 204:
      return "No Content";
    case 205:
      return "Reset Content";
    case 206:
      return "Partial Content";
    case 300:
      return "Multiple Choices";
    case 301:
      return "Moved Permanently";
    case 302:
      return "Found";
    case 303:
      return "See Other";
    case 304:
      return "Not Modified";
    case 307:
      return "Temporary Redirect";
    case 308:
      return "Permanent Redirect";
    case 400:
      return "Bad Request";
    case 401:
      return "Unauthorized";
    case 403:
      return "Forbidden";
    case 404:
      return "Not Found";
    case 405:
      return "Method Not Allowed";
    case 406:
      return "Not Acceptable";
    case 408:
      return "Request Timeout";
    case 409:
      return "Conflict";
    case 411:
      return "Length Required";
    case 412:
      return "Precondition Failed";
    case 413:
      return "Payload Too Large";
    case 414:
      return "URI Too Long";
    case 415:
      return "Unsupported Media Type";
    case 416:
      return "Range Not Satisfiable";
    case 417:
      return "Expectation Failed";
    case 429:
      return "Too Many Requests";
    case 431:
      return "Request Header Fields Too Large";
    case 500:
      return "Internal Server Error";
    case 501:
      return "Not Implemented";
    case 502:
      return "Bad Gateway";
    case 503:
      return "Service Unavailable";
    case 504:
      return "Gateway Timeout";
    case 505:
      return "HTTP Version Not Supported";
    default:
      return "Status";
  }
}

Body Body::borrowed(std::string_view text) {
  Body body;
  body.kind_ = Kind::Borrowed;
  body.borrowed_ = text;
  return body;
}

Body Body::file(std::string path) {
  Body body;
  body.kind_ = Kind::File;
  body.file_ = std::make_shared<FileSpec>();
  body.file_->path = std::move(path);
  body.file_->length_known = false;
  return body;
}

Body Body::file_range(std::string path, std::uint64_t offset, std::uint64_t length) {
  Body body;
  body.kind_ = Kind::File;
  body.file_ = std::make_shared<FileSpec>();
  body.file_->path = std::move(path);
  body.file_->offset = offset;
  body.file_->length = length;
  body.file_->length_known = true;
  return body;
}

Body Body::stream(ChunkProducer producer) {
  Body body;
  body.kind_ = Kind::Stream;
  body.producer_ = std::make_shared<ChunkProducer>(std::move(producer));
  return body;
}

bool Body::empty() const noexcept {
  switch (kind_) {
    case Kind::Empty:
      return true;
    case Kind::Owned:
      return owned_.empty();
    case Kind::Borrowed:
      return borrowed_.empty();
    case Kind::File:
      return file_ && file_->length_known && file_->length == 0;
    case Kind::Stream:
      return false;
  }
  return true;
}

std::string_view Body::view() const noexcept {
  if (kind_ == Kind::Owned) return owned_;
  if (kind_ == Kind::Borrowed) return borrowed_;
  return {};
}

const Body::FileSpec* Body::file() const noexcept {
  return kind_ == Kind::File ? file_.get() : nullptr;
}

const Body::ChunkProducer* Body::producer() const noexcept {
  return kind_ == Kind::Stream ? producer_.get() : nullptr;
}

std::uint64_t Body::size() const noexcept {
  switch (kind_) {
    case Kind::Empty:
      return 0;
    case Kind::Owned:
      return owned_.size();
    case Kind::Borrowed:
      return borrowed_.size();
    case Kind::File:
      return file_ && file_->length_known ? file_->length : unknown_size;
    case Kind::Stream:
      return unknown_size;
  }
  return unknown_size;
}

void Request::parse_query() const {
  query_cache_ = parse_pairs(query_string, false);
  query_parsed_ = true;
}

std::string_view Request::query(std::string_view name) const {
  if (!query_parsed_) parse_query();
  for (const auto& [key, value] : query_cache_) {
    if (key == name) return value;
  }
  return {};
}

const std::vector<std::pair<std::string, std::string>>& Request::query_params() const {
  if (!query_parsed_) parse_query();
  return query_cache_;
}

void Request::parse_cookies() const {
  cookie_cache_.clear();
  auto header = headers.get("cookie");
  while (!header.empty()) {
    auto semicolon = header.find(';');
    auto item = trim_ows(semicolon == std::string_view::npos ? header : header.substr(0, semicolon));
    if (!item.empty()) {
      auto equals = item.find('=');
      if (equals != std::string_view::npos) {
        auto key = trim_ows(item.substr(0, equals));
        auto value = trim_ows(item.substr(equals + 1));
        if (!key.empty()) cookie_cache_.emplace_back(std::string(key), std::string(value));
      }
    }
    if (semicolon == std::string_view::npos) break;
    header.remove_prefix(semicolon + 1);
  }
  cookies_parsed_ = true;
}

std::string_view Request::cookie(std::string_view name) const {
  if (!cookies_parsed_) parse_cookies();
  for (const auto& [key, value] : cookie_cache_) {
    if (key == name) return value;
  }
  return {};
}

void Request::parse_form() const {
  form_cache_.clear();
  auto type = headers.get("content-type");
  if (Headers::equals(trim_ows(type.substr(0, type.find(';'))),
                      "application/x-www-form-urlencoded")) {
    form_cache_ = parse_pairs(body, true);
  }
  form_parsed_ = true;
}

std::string_view Request::form(std::string_view name) const {
  if (!form_parsed_) parse_form();
  for (const auto& [key, value] : form_cache_) {
    if (key == name) return value;
  }
  return {};
}

std::string_view Request::param(std::string_view name) const noexcept {
  for (const auto& [key, value] : route_params) {
    if (key == name) return value;
  }
  return {};
}

bool Request::keep_alive() const noexcept {
  auto connection = headers.get("connection");
  if (version == "HTTP/1.1") {
    return !contains_token(connection, "close");
  }
  return contains_token(connection, "keep-alive");
}

const MultipartResult& Request::multipart() const {
  if (!multipart_cache_) {
    multipart_cache_ = std::make_shared<MultipartResult>(
        parse_multipart(body, headers.get("content-type")));
  }
  return *multipart_cache_;
}

const MultipartPart* Request::part(std::string_view name) const {
  return multipart().find(name);
}

bool Request::expects_continue() const noexcept {
  return Headers::equals(trim_ows(headers.get("expect")), "100-continue");
}

Response::Response(int status_code, std::string text, std::string_view content_type)
    : status(status_code) {
  reason = std::string(reason_phrase(status));
  body = Body(std::move(text));
  if (!content_type.empty()) headers.set("content-type", std::string(content_type));
}

Response Response::json(std::string_view json_text, int status_code) {
  Response response(status_code, std::string(json_text), "application/json");
  return response;
}

Response Response::html(std::string_view html_text, int status_code) {
  Response response(status_code, std::string(html_text), "text/html; charset=utf-8");
  return response;
}

Response Response::text(std::string_view text_body, int status_code) {
  Response response(status_code, std::string(text_body), "text/plain; charset=utf-8");
  return response;
}

Response Response::redirect(std::string location, int status_code) {
  Response response(status_code, std::string(), "text/plain; charset=utf-8");
  response.headers.set("location", std::move(location));
  return response;
}

Response Response::not_found(std::string_view message) {
  return Response(404, std::string(message), "text/plain; charset=utf-8");
}

Response Response::bad_request(std::string_view message) {
  return Response(400, std::string(message), "text/plain; charset=utf-8");
}

Response Response::stream(std::string content_type, Body::ChunkProducer producer) {
  Response response;
  response.status = 200;
  response.reason = "OK";
  response.body = Body::stream(std::move(producer));
  if (!content_type.empty()) response.headers.set("content-type", std::move(content_type));
  return response;
}

Response Response::file(std::string path) {
  Response response;
  response.reason = "OK";
  response.body = Body::file(std::move(path));
  return response;
}

Response Response::file_range(std::string path, std::uint64_t offset, std::uint64_t length) {
  Response response;
  response.status = 206;
  response.reason = "Partial Content";
  response.body = Body::file_range(std::move(path), offset, length);
  return response;
}

Response& Response::set(std::string name, std::string value) {
  headers.set(std::move(name), std::move(value));
  return *this;
}

Response& Response::add(std::string name, std::string value) {
  headers.add(std::move(name), std::move(value));
  return *this;
}

Response& Response::set_content_type(std::string value) {
  headers.set("content-type", std::move(value));
  return *this;
}

Response& Response::set_cookie(std::string name, std::string value, CookieOptions options) {
  validate_header_name(name);
  bool invalid = value.find(';') != std::string::npos || value.find('\r') != std::string::npos ||
                 value.find('\n') != std::string::npos ||
                 value.find('\0') != std::string::npos;
  if (invalid) throw std::invalid_argument("ferry: invalid cookie value");
  std::string cookie = name + '=' + value;
  if (!options.path.empty()) cookie += "; Path=" + options.path;
  if (!options.domain.empty()) cookie += "; Domain=" + options.domain;
  if (options.has_max_age) cookie += "; Max-Age=" + std::to_string(options.max_age_seconds);
  if (options.secure) cookie += "; Secure";
  if (options.http_only) cookie += "; HttpOnly";
  if (!options.same_site.empty()) cookie += "; SameSite=" + options.same_site;
  headers.add("set-cookie", std::move(cookie));
  return *this;
}

std::vector<std::pair<std::string, std::string>> parse_pairs(std::string_view text, bool plus_as_space) {
  std::vector<std::pair<std::string, std::string>> result;
  while (!text.empty()) {
    auto ampersand = text.find('&');
    auto item = ampersand == std::string_view::npos ? text : text.substr(0, ampersand);
    if (!item.empty()) {
      auto equals = item.find('=');
      auto key = equals == std::string_view::npos ? item : item.substr(0, equals);
      auto value = equals == std::string_view::npos ? std::string_view() : item.substr(equals + 1);
      result.emplace_back(percent_decode_view(key, plus_as_space),
                          percent_decode_view(value, plus_as_space));
    }
    if (ampersand == std::string_view::npos) break;
    text.remove_prefix(ampersand + 1);
  }
  return result;
}

}  // namespace ferry
