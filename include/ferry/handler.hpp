#pragma once

#include <functional>
#include <string_view>

#include "ferry/http.hpp"

namespace ferry {

using Handler = std::function<task<Response>(const Request&)>;
using Next = std::function<task<void>()>;
using Middleware = std::function<task<void>(Request&, Response&, Next)>;
using ErrorHandler = std::function<Response(const Request&, int status, std::string_view message)>;

}  // namespace ferry
