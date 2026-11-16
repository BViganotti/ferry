#include <cstdio>
#include <cstdlib>
#include <string>

#include <ferry/ferry.hpp>

int main(int argc, char** argv) {
  ferry::ServerConfig config;
  config.bind_address = "127.0.0.1";
  config.port = argc > 1 ? static_cast<std::uint16_t>(std::atoi(argv[1])) : 8080;
  config.threads = argc > 2 ? std::atoi(argv[2]) : 4;
  config.gzip = true;

  ferry::Server server(config);

  server.get("/", [](const ferry::Request&) -> ferry::task<ferry::Response> {
    co_return ferry::Response::html(
        "<!doctype html><h1>Hello from ferry</h1>"
        "<p>Try <code>/hello/ada</code>, <code>/stream</code> or <code>/assets/</code>.</p>");
  });

  server.get("/hello/{name}",
             [](const ferry::Request& request) -> ferry::task<ferry::Response> {
               co_return ferry::Response::text("hello, " + std::string(request.param("name")) + "\n");
             });

  server.post("/echo", [](const ferry::Request& request) -> ferry::task<ferry::Response> {
    co_return ferry::Response(200, request.body, request.header("content-type"));
  });

  server.get("/stream", [](const ferry::Request&) -> ferry::task<ferry::Response> {
    auto counter = std::make_shared<int>(0);
    co_return ferry::Response::stream(
        "text/plain; charset=utf-8", [counter](std::string& chunk) -> ferry::task<bool> {
          if (*counter >= 5) co_return false;
          ++*counter;
          chunk = "chunk " + std::to_string(*counter) + "\n";
          co_return true;
        });
  });

  server.get("/sleep", [](const ferry::Request&) -> ferry::task<ferry::Response> {
    co_await ferry::sleep_for(std::chrono::milliseconds(100));
    co_return ferry::Response::text("awake\n");
  });

  server.use([](ferry::Request&, ferry::Response&, ferry::Next next) -> ferry::task<void> {
    co_await next();
  });

  server.on_error([](const ferry::Request&, int status, std::string_view message) {
    return ferry::Response(status, std::string(message) + "\n");
  });

  server.static_files("/assets", ".");

  server.start();
  std::printf("ferry listening on http://127.0.0.1:%u (Ctrl-C to stop)\n",
              static_cast<unsigned>(server.port()));
  server.wait();
  return 0;
}
