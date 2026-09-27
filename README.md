# High-Performance Asynchronous HTTP Server

An event-driven, non-blocking HTTP/1.1 server implemented in C for Linux environments. The architecture avoids thread-per-connection overhead by relying on single-threaded event multiplexing and asynchronous kernel primitives.

## Architectural Highlights

- **I/O Multiplexing via `epoll`:** Listens for socket read/write readiness with non-blocking file descriptors (`SOCK_NONBLOCK`, `O_NONBLOCK`) to handle high connection concurrency without multi-threading overhead.
- **Zero-Copy Static File Transfer:** Uses the Linux `sendfile()` system call to stream static assets directly from the page cache to the network socket, avoiding context switches and eliminating memory copies into user space.
- **Kernel-level Asynchronous I/O (`libaio` + `eventfd`):** Dynamic disk reads are executed asynchronously via Linux native AIO (`io_prep_pread`, `io_submit`). Readiness notifications are signaled through an `eventfd` descriptor integrated directly into the `epoll` loop, keeping file reading non-blocking.
- **State-Machine Driven Protocol Handling:** Per-connection finite state machines track HTTP parsing, partial socket writes, and chunked transfers without busy-waiting.
- **HTTP Request Parsing:** Integrates a lightweight streaming parser to extract request methods and paths incrementally.

## Project Structure

```text
.
├── Makefile
├── README.md
├── aws.c                  # Core server event loop, state machine & I/O logic
├── aws.h                  # Connection structures, state definitions & macros
├── http-parser/           # Joyent HTTP streaming parser library
│   ├── LICENSE-MIT
│   ├── http_parser.c
│   └── http_parser.h
├── utils/                 # Socket & debugging utilities
│   ├── debug.h
│   ├── sock_util.c
│   ├── sock_util.h
│   ├── util.h
│   └── w_epoll.h          # epoll wrappers
├── static/                # Static assets served via sendfile()
│   └── index.html
└── dynamic/               # Dynamic payloads served via libaio + eventfd
    └── sample.dat
```

## Prerequisites

- Linux OS (Kernel 2.6+)
- GCC & GNU Make
- `libaio-dev` (Linux native AIO library)

Install dependencies on Debian/Ubuntu:
```bash
sudo apt-get update
sudo apt-get install -y build-essential libaio-dev
```

## Compilation

Build the executable:
```bash
make
```

Clean build artifacts:
```bash
make clean
```

## Running the Server

Start the binary (binds to port `8888` by default):
```bash
./aws
```

## Verification & Testing

Verify static file delivery via zero-copy:
```bash
curl -i http://localhost:8888/static/index.html
```

Verify dynamic file transfer via kernel AIO:
```bash
curl -i http://localhost:8888/dynamic/sample.dat
```

Benchmark concurrent throughput (e.g., using ApacheBench):
```bash
ab -n 10000 -c 100 http://localhost:8888/static/index.html
```

## License

This project is licensed under the BSD 3-Clause License. Included `http-parser` components are licensed under the MIT License.