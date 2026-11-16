#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include <ferry/ferry.hpp>

namespace {

using namespace std::chrono_literals;

ferry::task<void> worker(ferry::Client& client, std::string url,
                         std::chrono::steady_clock::time_point end,
                         std::atomic<long long>& completed, std::atomic<long long>& failed,
                         std::atomic<int>& active) {
  while (std::chrono::steady_clock::now() < end) {
    auto response = co_await client.get(url);
    if (response.status == 200 && response.body.view() == "hello world") {
      completed.fetch_add(1, std::memory_order_relaxed);
    } else {
      failed.fetch_add(1, std::memory_order_relaxed);
    }
  }
  active.fetch_sub(1, std::memory_order_relaxed);
}

}  // namespace

int main(int argc, char** argv) {
  int seconds = argc > 1 ? std::atoi(argv[1]) : 3;
  int connections = argc > 2 ? std::atoi(argv[2]) : 256;
  int server_threads = argc > 3 ? std::atoi(argv[3])
                                : static_cast<int>(std::thread::hardware_concurrency());
  int client_threads = argc > 4 ? std::atoi(argv[4]) : 4;
  if (seconds <= 0) seconds = 3;
  if (connections <= 0) connections = 256;
  if (server_threads <= 0) server_threads = 4;
  if (client_threads <= 0) client_threads = 1;

  ferry::ServerConfig config;
  config.bind_address = "127.0.0.1";
  config.port = 0;
  config.threads = server_threads;

  ferry::Server server(config);
  server.get("/", [](const ferry::Request&) -> ferry::task<ferry::Response> {
    co_return ferry::Response::text("hello world");
  });
  server.start();

  auto url = "http://127.0.0.1:" + std::to_string(server.port()) + "/";
  int per_thread = connections / client_threads;
  if (per_thread < 1) per_thread = 1;

  std::printf(
      "ferry bench: %d s, %d connections (%d client threads), %d server threads, %s\n", seconds,
      per_thread * client_threads, client_threads, server_threads, url.c_str());

  std::atomic<long long> completed{0};
  std::atomic<long long> failed{0};
  auto start = std::chrono::steady_clock::now();
  auto end = start + std::chrono::seconds(seconds);

  std::vector<std::thread> clients;
  clients.reserve(static_cast<std::size_t>(client_threads));
  for (int i = 0; i < client_threads; ++i) {
    clients.emplace_back([&, i] {
      std::string thread_url = url;
      int thread_connections = per_thread;
      ferry::run([&, thread_url, thread_connections]() -> ferry::task<void> {
        ferry::ClientConfig client_config;
        client_config.max_idle_per_host = static_cast<std::size_t>(thread_connections);
        client_config.timeout = 10s;
        ferry::Client client(client_config);

        std::atomic<int> active{thread_connections};
        for (int c = 0; c < thread_connections; ++c) {
          ferry::spawn(worker(client, thread_url, end, completed, failed, active));
        }
        while (active.load(std::memory_order_relaxed) > 0) {
          co_await ferry::sleep_for(20ms);
        }
      });
    });
  }
  for (auto& client : clients) client.join();

  auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  auto total = completed.load();
  std::printf("requests: %lld  failures: %lld  elapsed: %.2fs  throughput: %.0f req/s\n", total,
              failed.load(), elapsed, static_cast<double>(total) / elapsed);

  server.stop();
  return failed.load() == 0 ? 0 : 1;
}
