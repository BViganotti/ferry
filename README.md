# ferry

A C++20 HTTP/1.1 client and server library built on coroutines, `std::span`,
`std::string_view` and one small event loop per thread. TLS included. No
framework, no code generation, no thread per connection.

```cpp
#include <ferry/ferry.hpp>

int main() {
  ferry::ServerConfig config;
  config.port = 8080;
  config.threads = 4;

  ferry::Server server(config);
  server.get("/hello/{name}", [](const ferry::Request& request) -> ferry::task<ferry::Response> {
    co_return ferry::Response::text("hello, " + std::string(request.param("name")) + "\n");
  });
  server.static_files("/assets", "./public");
  server.start();
  server.wait();
}
```

```cpp
return ferry::run([]() -> ferry::task<int> {
  ferry::Client client;
  auto response = co_await client.get("https://example.com/");
  std::printf("%d %zu bytes\n", response.status, response.body.view().size());
  co_return response.ok() ? 0 : 1;
});
```

## Why

The C++ HTTP library landscape forces a choice: huge frameworks, Boost
dependencies, callback spaghetti, blocking thread-per-request designs, or
libraries where TLS is an afterthought. `ferry` is the small version that does
the whole job:

- coroutine handlers, so async code reads like blocking code;
- a real event loop (`epoll`, `kqueue`, or `poll`) instead of threads per socket;
- TLS 1.2+ with certificate and hostname verification on by default;
- HTTP/1.1 keep-alive, chunked transfer coding, streaming responses, `HEAD`,
  `Range`, gzip, cookies, forms, redirects and connection pooling;
- strict, bounded request parsing that rejects the usual smuggling shapes.

## Features

**Server**

- Coroutine handlers returning `ferry::Response`; middleware chain.
- Route parameters (`/users/{id}`), wildcards (`/files/{*path}`), automatic
  `HEAD`→`GET` fallback, `405` with `Allow`, automatic `OPTIONS`.
- Streaming response bodies with chunked encoding, file bodies with `Range`
  and ETag/`If-None-Match`, static directory handler with traversal protection.
- TLS with ALPN `http/1.1`, optional client certificates, TLS 1.2 minimum.
- Keep-alive, request pipelining (processed in order), `Expect: 100-continue`,
  HTTP/1.0 fallback, gzip request and response bodies.
- Limits and timeouts for headers, bodies, connections, idle keep-alive and
  writes. Graceful shutdown drains in-flight requests.
- Multi-threaded: one acceptor distributes sockets round-robin to N reactor
  loops, each with its own `EventLoop`.

**Client**

- TLS 1.2+ with system CA store or a custom CA file, SNI and hostname checks.
- Connection pooling with idle timeouts, automatic retry of safe requests on a
  stale pooled connection, gzip decoding.
- Redirects (301/302/303/307/308) with method rewriting and cross-origin
  `Authorization` stripping.
- Buffered or streamed request bodies, file uploads, chunked requests.

**Protocol and security**

- Strict request-line/header validation, obs-fold rejection, bare-LF rejection.
- Duplicate/conflicting `Content-Length` rejected; `Transfer-Encoding` +
  `Content-Length` rejected; only `chunked` accepted.
- Bounded header size/count, request line, chunk line, trailers and body size.
- Response header and reason-phrase injection prevented.
- CR/LF/NUL rejected in header names and values.
- Path traversal (`..`, encoded `..`, backslashes) blocked in static serving.
- TLS hostname verification on by default; SIGPIPE handled.

## Build

Requirements: CMake ≥ 3.20, a C++20 compiler, OpenSSL. Optional: zlib, Ninja.

```console
$ cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
$ cmake --build build -j
$ ./build/ferry_tests
```

Or:

```console
$ make test
$ make bench
```

OpenSSL is required. zlib is optional (`-DFERRY_WITH_ZLIB=OFF` disables gzip).
On macOS with Homebrew:

```console
$ cmake -S . -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DOPENSSL_ROOT_DIR="$(brew --prefix openssl@3)"
```

## Server API

```cpp
ferry::Server server(config);

server.get("/", handler);
server.post("/users", handler);
server.put / patch / del / head / options / any;

server.use([](ferry::Request& request, ferry::Response& response,
              ferry::Next next) -> ferry::task<void> {
  auto started = std::chrono::steady_clock::now();
  co_await next();
  // response is now populated
});

server.static_files("/static", "./public", {
  .etag = true,
  .dotfiles = false,
  .index = "index.html",
  .cache_max_age = std::chrono::seconds(3600),
});

server.on_not_found([](const ferry::Request&) -> ferry::task<ferry::Response> {
  co_return ferry::Response::not_found("no such route\n");
});

server.on_error([](const ferry::Request&, int status, std::string_view message) {
  return ferry::Response(status, std::string(message) + "\n");
});

server.start();     // non-blocking, binds and spawns worker threads
server.wait();      // block until stop()
server.stop();      // graceful drain, joins workers
```

`Request` gives you `method`, `target`, `path`, `query_string`, `version`,
headers, body, `remote_address`, `secure`, `tls_version`, and helpers:

```cpp
request.header("content-type");
request.query("page");
request.param("id");
request.cookie("session");
request.form("email");
request.body;                       // std::string, buffered
request.keep_alive();
```

`Response`:

```cpp
ferry::Response::text("ok");
ferry::Response::html("<h1>ok</h1>");
ferry::Response::json(R"({"ok":true})");
ferry::Response::redirect("/login", 303);
ferry::Response::not_found();
ferry::Response::file("data/report.pdf");
ferry::Response::file_range("video.mp4", offset, length);
ferry::Response::stream("text/event-stream", producer);
```

A streaming producer is a coroutine callable:

```cpp
auto counter = std::make_shared<int>(0);
co_return ferry::Response::stream(
    "text/plain", [counter](std::string& chunk) -> ferry::task<bool> {
      if (*counter == 3) co_return false;
      chunk = "line " + std::to_string(++*counter) + "\n";
      co_return true;
    });
```

## Client API

```cpp
ferry::ClientConfig config;
config.timeout = std::chrono::seconds(10);
config.verify_tls = true;
config.ca_file = "internal-ca.pem";
config.client_certificate_file = "client.crt";   // optional mTLS
config.client_private_key_file = "client.key";
config.max_idle_per_host = 16;

ferry::Client client(config);
auto response = co_await client.get("https://api.example.com/items");
auto created = co_await client.post("https://api.example.com/items",
                                    R"({"name":"x"})", "application/json");
auto uploaded = co_await client.send(
    ferry::Method::Put, "https://api.example.com/blob",
    ferry::Body::file("payload.bin"), {});

if (response.status == 0) {
  // transport failure, details in response.error
}
```

`ferry::Client` must be used from one event-loop thread. `ferry::run()` gives
you that loop, and `ferry::spawn()` starts background work safely:

```cpp
int main() {
  return ferry::run([]() -> ferry::task<int> {
    ferry::Client client;
    ferry::spawn([client = &client]() -> ferry::task<void> {
      auto r = co_await client->get("http://127.0.0.1:8080/");
      // ...
    });
    co_await ferry::sleep_for(std::chrono::seconds(1));
    co_return 0;
  });
}
```

`spawn` copies a coroutine lambda into a heap wrapper, so temporaries are safe.
If you pass an already-created `task<T>`, the task itself must outlive the
coroutine.

## TLS

```cpp
ferry::TlsConfig tls;
tls.certificate_file = "server.crt";
tls.private_key_file = "server.key";
tls.ca_file = "client-ca.pem";          // optional
tls.require_client_certificate = false; // set true for mTLS
config.tls = tls;
```

Client verification is enabled by default. `verify_tls = false` is available
for tests but never use it in production. An IP address in the URL is verified
against the certificate's IP SAN; DNS names use SNI and hostname verification.

## Configuration

`ServerConfig` (selected defaults):

| Field | Default |
| --- | --- |
| `bind_address`, `port` | `0.0.0.0`, `8080` |
| `threads` | `1` |
| `max_header_bytes` | 64 KiB |
| `max_body_bytes` | 16 MiB |
| `max_headers` | 100 |
| `header_timeout` | 10 s |
| `body_timeout` | 30 s |
| `idle_timeout` | 60 s |
| `write_timeout` | 30 s |
| `shutdown_timeout` | 5 s |
| `gzip` | off |
| `access_log` | none |

`ClientConfig` (selected defaults):

| Field | Default |
| --- | --- |
| `connect_timeout`, `timeout` | 10 s, 30 s |
| `idle_timeout` | 60 s |
| `max_response_body` | 64 MiB |
| `follow_redirects`, `max_redirects` | on, 5 |
| `verify_tls` | on |
| `client_certificate_file` / `client_private_key_file` | empty (no mTLS) |
| `reuse_connections` | on |
| `max_idle_per_host` | 8 |

## Performance

Measured on an Apple M1 Max (macOS, loopback), release build, 5-second runs.
`wrk` was also run against a minimal Go `net/http` server on the same machine
as a control:

| Server | wrk | Requests/s |
| --- | --- | --- |
| ferry | `wrk -t4 -c256 -d5s` | **33,378** |
| Go `net/http` | `wrk -t4 -c256 -d5s` | 29,754 |
| ferry | `wrk -t8 -c512 -d5s` | **32,235** |
| Go `net/http` | `wrk -t8 -c512 -d5s` | 28,694 |

On this loopback/wrk combination the client side plateaus around 30–35k
requests/s; ferry is slightly ahead of Go and is not the bottleneck. With
`ferry_bench` (4 client threads, 256 keep-alive connections) it sustains about
32k req/s with zero failures; a single keep-alive connection does ~11.7k
req/s (85 µs round trip).

Run it yourself:

```console
$ ./build/ferry_bench 5 256 4 4
$ ./build/ferry_hello 8080 4
$ wrk -t4 -c256 -d5s http://127.0.0.1:8080/
```

## Design

- **One coroutine per connection.** The connection coroutine reads a request,
  dispatches it, writes the response and loops. While it waits, the reactor
  runs other connections. No thread is ever parked on a socket.
- **Symmetric-transfer tasks.** `task<T>` uses `std::coroutine_handle` symmetric
  transfer so awaiting does not grow the stack through wrapper coroutines.
- **Readiness reactor.** `EventLoop` wraps `epoll` (Linux), `kqueue`
  (macOS/BSD) or `poll` (fallback), with an indexed timer heap. I/O awaiters
  register one read or write interest per descriptor and resume the coroutine
  when ready, timed out or cancelled.
- **Single acceptor, N workers.** The listening socket lives on worker 0. Each
  accepted descriptor is handed to a worker loop round-robin, so request
  processing scales with `threads` without `SO_REUSEPORT` quirks.
- **No locks on the hot path.** A connection and its parser state belong to one
  loop thread. Shared state is limited to counters and startup configuration.
- **Buffered parsing with hard bounds.** Headers and bodies are parsed from a
  per-connection buffer; the parser never allocates based on an untrusted
  length before checking it against the configured limit.
- **OpenSSL behind a pimpl.** Public headers do not include OpenSSL headers.
  TLS streams still use the same `Stream` interface, so server and client code
  is identical for plain and encrypted connections.

## Limitations / roadmap

Deliberately not implemented yet:

- HTTP/2 (HPACK, framing, flow control) and HTTP/3.
- WebSocket upgrades.
- Client-side cookie jar and automatic `Authorization` refresh.
- HTTP proxy and CONNECT support.
- Multipart form parsing.
- Asynchronous DNS (`getaddrinfo` is blocking during connect).
- Streaming *request* bodies to a handler (requests are buffered up to
  `max_body_bytes`; responses can stream).
- Multi-range requests and `If-Range`, and `If-Modified-Since` (ETag works).
- Compression of streamed/file bodies (owned in-memory bodies are compressed).
- HTTP/2 prior knowledge, trailers API, and server push.

Already there: HTTP/1.1 keep-alive and pipelining, chunked request/response
bodies, 100-continue, gzip, TLS 1.2+, mTLS, client certificate verification,
redirects, pooling, range requests, static files, and graceful shutdown.

## Layout

| Path | Contents |
| --- | --- |
| `include/ferry/task.hpp` | coroutine `task<T>`, `spawn` |
| `include/ferry/event_loop.hpp` | deadline, I/O waiters, reactor |
| `include/ferry/net.hpp` | addresses, sockets, `Stream` |
| `include/ferry/tls.hpp` | TLS context and stream |
| `include/ferry/http.hpp` | headers, bodies, requests, responses |
| `include/ferry/server.hpp` | server API and configuration |
| `include/ferry/client.hpp` | client API and configuration |
| `src/*.cpp` | implementation |
| `tests/tests.cpp` | unit, protocol, TLS, concurrency tests |
| `examples/` | hello, HTTPS and client examples |
| `bench/bench.cpp` | multi-threaded throughput benchmark |

## License

MIT. See [LICENSE](LICENSE).
