#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "ferry/http.hpp"
#include "ferry/handler.hpp"

namespace ferry::detail {

struct RouteMatch {
  const Handler* handler = nullptr;
  std::vector<std::pair<std::string, std::string>> params;
  bool path_matched = false;
  std::vector<std::string> allowed;
};

class Router {
 public:
  void add(Method method, std::string pattern, Handler handler, bool any_method = false);

  RouteMatch match(Method method, std::string_view path) const;

  bool empty() const noexcept { return routes_.empty(); }

 private:
  struct Segment {
    enum class Kind { Literal, Parameter, Wildcard };
    Kind kind = Kind::Literal;
    std::string text;
  };

  struct Route {
    Method method = Method::Get;
    bool any_method = false;
    std::string pattern;
    std::vector<Segment> segments;
    Handler handler;
  };

  static std::vector<Segment> compile(std::string_view pattern);
  static bool match_path(const Route& route, const std::vector<std::string_view>& path,
                         std::vector<std::pair<std::string, std::string>>& params);
  static std::string route_key(Method method, std::string_view path);
  static void append_allowed(RouteMatch& match, const Route& route);

  std::vector<Route> routes_;
  std::unordered_map<std::string, std::size_t> exact_;
};

}  // namespace ferry::detail
