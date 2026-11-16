#include "router.hpp"

#include <algorithm>
#include <string>
#include <vector>

#include "ferry/url.hpp"

namespace ferry::detail {

namespace {

std::vector<std::string_view> split_path(std::string_view path) {
  std::vector<std::string_view> segments;
  if (path.empty() || path == "/") return segments;
  std::size_t index = path.front() == '/' ? 1 : 0;
  while (index <= path.size()) {
    auto slash = path.find('/', index);
    if (slash == std::string_view::npos) {
      segments.push_back(path.substr(index));
      break;
    }
    segments.push_back(path.substr(index, slash - index));
    index = slash + 1;
  }
  return segments;
}

bool is_parameter(std::string_view segment) {
  return segment.size() >= 3 && segment.front() == '{' && segment.back() == '}' &&
         !(segment[1] == '*');
}

bool is_wildcard(std::string_view segment) {
  return segment.size() >= 4 && segment.starts_with("{*") && segment.back() == '}';
}

std::string parameter_name(std::string_view segment, bool wildcard) {
  if (wildcard) return std::string(segment.substr(2, segment.size() - 3));
  return std::string(segment.substr(1, segment.size() - 2));
}

}  // namespace

std::vector<Router::Segment> Router::compile(std::string_view pattern) {
  std::vector<Segment> segments;
  for (auto part : split_path(pattern)) {
    Segment segment;
    if (is_wildcard(part)) {
      segment.kind = Segment::Kind::Wildcard;
      segment.text = parameter_name(part, true);
    } else if (is_parameter(part)) {
      segment.kind = Segment::Kind::Parameter;
      segment.text = parameter_name(part, false);
    } else {
      segment.kind = Segment::Kind::Literal;
      segment.text = std::string(part);
    }
    segments.push_back(std::move(segment));
  }
  return segments;
}

std::string Router::route_key(Method method, std::string_view path) {
  std::string key;
  key.reserve(path.size() + 4);
  key += static_cast<char>('A' + static_cast<int>(method));
  key += ':';
  key.append(path.data(), path.size());
  return key;
}

void Router::append_allowed(RouteMatch& match, const Route& route) {
  auto add = [&](std::string_view name) {
    for (const auto& item : match.allowed) {
      if (item == name) return;
    }
    match.allowed.emplace_back(name);
  };
  if (route.any_method) {
    add("*");
    return;
  }
  add(method_name(route.method));
  if (route.method == Method::Get) add("HEAD");
}

void Router::add(Method method, std::string pattern, Handler handler, bool any_method) {
  if (pattern.empty() || pattern.front() != '/') pattern.insert(pattern.begin(), '/');
  Route route;
  route.method = method;
  route.any_method = any_method;
  route.pattern = pattern;
  route.segments = compile(pattern);
  route.handler = std::move(handler);

  bool has_parameter = false;
  for (const auto& segment : route.segments) {
    if (segment.kind != Segment::Kind::Literal) {
      has_parameter = true;
      break;
    }
  }
  if (!has_parameter && !any_method) {
    exact_[route_key(method, pattern)] = routes_.size();
  }
  routes_.push_back(std::move(route));
}

bool Router::match_path(const Route& route, const std::vector<std::string_view>& path,
                        std::vector<std::pair<std::string, std::string>>& params) {
  params.clear();
  std::size_t index = 0;
  for (std::size_t segment_index = 0; segment_index < route.segments.size(); ++segment_index) {
    const auto& pattern = route.segments[segment_index];
    if (pattern.kind == Segment::Kind::Wildcard) {
      std::string rest;
      for (std::size_t i = index; i < path.size(); ++i) {
        if (!rest.empty()) rest += '/';
        rest.append(path[i].data(), path[i].size());
      }
      params.emplace_back(pattern.text, percent_decode(rest));
      return true;
    }
    if (index >= path.size()) return false;
    auto value = path[index++];
    if (pattern.kind == Segment::Kind::Parameter) {
      if (value.empty()) return false;
      params.emplace_back(pattern.text, percent_decode(value));
    } else if (value != pattern.text) {
      return false;
    }
  }
  return index == path.size();
}

RouteMatch Router::match(Method method, std::string_view path) const {
  RouteMatch result;
  if (path.empty() || path.front() != '/') return result;
  auto path_segments = split_path(path);

  auto consider = [&](const Route& route) -> bool {
    if (!match_path(route, path_segments, result.params)) return false;
    result.path_matched = true;
    append_allowed(result, route);
    result.handler = &route.handler;
    return true;
  };

  if (method == Method::Other) {
    // Unknown methods only match explicit "any" routes.
    for (const auto& route : routes_) {
      if (route.any_method && consider(route)) return result;
    }
    return result;
  }

  // Fast path: literal route for the exact method.
  auto exact = exact_.find(route_key(method, path));
  if (exact != exact_.end()) {
    const auto& route = routes_[exact->second];
    result.path_matched = true;
    append_allowed(result, route);
    result.handler = &route.handler;
    return result;
  }

  // Explicit matches, including any-method routes, in registration order.
  for (const auto& route : routes_) {
    if (!route.any_method && route.method != method) {
      if (match_path(route, path_segments, result.params)) {
        result.path_matched = true;
        append_allowed(result, route);
      }
      continue;
    }
    if (consider(route)) return result;
  }

  // HEAD falls back to a GET route, as required by HTTP semantics.
  if (method == Method::Head) {
    auto head_exact = exact_.find(route_key(Method::Get, path));
    if (head_exact != exact_.end()) {
      const auto& route = routes_[head_exact->second];
      result.path_matched = true;
      append_allowed(result, route);
      result.handler = &route.handler;
      return result;
    }
    for (const auto& route : routes_) {
      if (route.any_method || route.method != Method::Get) continue;
      if (consider(route)) return result;
    }
  }

  return result;
}

}  // namespace ferry::detail
