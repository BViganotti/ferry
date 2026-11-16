# Changelog

## 0.1.0 — 2026-11-16

First complete cut.

- C++20 coroutine runtime: `ferry::task<T>`, `ferry::run`, safe `ferry::spawn`
  for callables, symmetric transfer, cancellation, timer heap.
- Reactor: `epoll`, `kqueue`, `poll` backends; non-blocking sockets; deadline
  aware I/O awaiters.
- HTTP/1.1 server: routing with parameters and wildcards, middleware, `HEAD`
  fallback, automatic `OPTIONS`, 405 + `Allow`.
- Streaming responses (chunked), file responses, Range/ETag/304, static file
  handler with traversal protection, gzip.
- TLS server and client via OpenSSL, TLS 1.2+, ALPN, SNI, hostname
  verification on by default; optional client certificates and mTLS.
- Client: keep-alive pool, redirects, gzip, chunked, file/stream uploads,
  cross-origin authorization stripping, safe-method retry.
- Client cookie jar, multipart form-data parsing, forward proxy + CONNECT,
  background DNS resolver with deadlines.
- WebSocket server routes and client handshake, fragmentation, ping/pong,
  close, masked frames.
- Request/response trailers, static `If-Modified-Since`, `If-Range`, and
  multipart byte ranges.
- Hardened parsing: bounded headers/body/chunks/trailers, smuggling rejection,
  header injection prevention, strict line endings.
- Graceful shutdown, connection limits, timeouts, access log hook.
- Tests: 19 suites covering codec, routing, plain/TLS/mTLS client-server
  flows, static files, raw protocol attacks, timeouts and concurrency.
- ASan/UBSan and TSan clean; benchmark tool plus wrk comparison against Go
  `net/http`.
- MIT license, CMake install rules, examples and docs.
