#include <cstdio>
#include <string>

#include <ferry/ferry.hpp>

int main(int argc, char** argv) {
  std::string url = argc > 1 ? argv[1] : "http://127.0.0.1:8080/";

  return ferry::run([&]() -> ferry::task<int> {
    ferry::Client client;
    auto response = co_await client.get(url);
    if (response.status == 0) {
      std::fprintf(stderr, "request failed: %s\n", response.error.c_str());
      co_return 1;
    }
    std::printf("HTTP %d %s\n", response.status, response.reason.c_str());
    for (const auto& [name, value] : response.headers.entries()) {
      std::printf("%s: %s\n", name.c_str(), value.c_str());
    }
    std::printf("\n%.*s\n", static_cast<int>(response.body.view().size()),
                response.body.view().data());
    co_return response.ok() ? 0 : 2;
  });
}
