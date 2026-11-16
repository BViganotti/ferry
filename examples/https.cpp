#include <cstdio>
#include <string>

#include <ferry/ferry.hpp>

int main(int argc, char** argv) {
  std::string certificate = argc > 1 ? argv[1] : "tests/certs/localhost.crt";
  std::string key = argc > 2 ? argv[2] : "tests/certs/localhost.key";

  ferry::ServerConfig config;
  config.bind_address = "127.0.0.1";
  config.port = 8443;
  config.threads = 2;
  config.gzip = true;
  config.tls = ferry::TlsConfig{certificate, key};

  ferry::Server server(config);
  server.get("/", [](const ferry::Request&) -> ferry::task<ferry::Response> {
    co_return ferry::Response::text("hello over TLS\n");
  });
  server.get("/info", [](const ferry::Request& request) -> ferry::task<ferry::Response> {
    co_return ferry::Response::text("tls=" + request.tls_version + " secure=" +
                                    (request.secure ? "yes" : "no") + "\n");
  });

  server.start();
  std::printf("ferry listening on https://localhost:%u\n",
              static_cast<unsigned>(server.port()));
  server.wait();
  return 0;
}
