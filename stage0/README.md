# Stage 0 — The Echo Server (line-by-line)

## The code

```cpp
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <cstring>
#include <iostream>

int main() {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);

    // let us reuse the port immediately after the program exits
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;   // listen on all interfaces
    addr.sin_port = htons(8080);         // htons = host-to-network byte order

    bind(server_fd, (sockaddr*)&addr, sizeof(addr));
    listen(server_fd, 16);               // 16 = backlog of pending connections
    std::cout << "listening on :8080\n";

    while (true) {
        int conn = accept(server_fd, nullptr, nullptr);  // blocks here
        char buf[1024];
        ssize_t n = read(conn, buf, sizeof(buf));
        write(conn, buf, n);             // echo it straight back
        close(conn);
    }
}
```

## The includes

Each pulls in one piece of the API.

- `<sys/socket.h>` gives you `socket`, `bind`, `listen`, `accept`, `setsockopt` — the core socket calls.
- `<netinet/in.h>` gives the internet-specific types: `sockaddr_in`, `htons`, `INADDR_ANY`.
- `<unistd.h>` gives the generic file-descriptor operations `read`, `write`, `close` (note these aren't socket-specific — a socket is just a file descriptor, so the same calls you'd use on a file work here).
- `<cstring>` is for `memset`-style helpers (not strictly needed as written, but conventional).
- `<iostream>` is just for the `std::cout` line.

## `int server_fd = socket(AF_INET, SOCK_STREAM, 0);`

This asks the kernel to create a socket and hand you back a file descriptor — a small integer that's your handle to it. The three arguments describe what kind:

- `AF_INET` = the IPv4 address family (you'll use `AF_INET6` for IPv6).
- `SOCK_STREAM` = a reliable, ordered, connection-based byte stream — which in the IPv4 family means TCP.
- `0` = "use the default protocol for this type," which resolves to TCP. (If you'd passed `SOCK_DGRAM` instead, you'd get UDP.)

On failure it returns `-1` — the code doesn't check, which is fine for learning but the first thing you'd harden.

## The setsockopt block

```cpp
int opt = 1;
setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
```

Solves a specific, annoying problem: when a server exits, the OS keeps the port in a `TIME_WAIT` state for a minute or two (to catch stray late packets from the old connection). During that window, a fresh bind to the same port fails with "Address already in use" — you've probably hit this when restarting quickly. `SO_REUSEADDR` tells the kernel "let me rebind anyway." The `1` means enable; `setsockopt` is a generic function that takes any option value, so you pass it by address (`&opt`) plus its size, rather than as a plain int. `SOL_SOCKET` says the option lives at the socket level (as opposed to, say, the TCP level).

## The address struct

```cpp
sockaddr_in addr{};
addr.sin_family = AF_INET;
addr.sin_addr.s_addr = INADDR_ANY;
addr.sin_port = htons(8080);
```

`sockaddr_in` is the struct describing an IPv4 endpoint: a family, an address, and a port.

- The `{}` zero-initializes every field — that matters because the struct has trailing padding bytes (`sin_zero`) the kernel expects to be zero.
- `sin_family = AF_INET` must match the socket you created.
- `sin_addr.s_addr = INADDR_ANY` is the numeric address `0.0.0.0`, meaning "bind to all network interfaces on this machine" — loopback, your LAN IP, everything. If you instead set it to `127.0.0.1`, only connections from the same machine could reach you.
- `htons(8080)` sets the port. The `htons` wrapper is the subtle part: networks agreed long ago to send multi-byte numbers big-endian ("network byte order"), but your CPU is probably little-endian, so `htons` ("host to network short") flips the bytes so 8080 is represented the way the wire expects. Skip it and you'd accidentally be listening on a different port.

## `bind(server_fd, (sockaddr*)&addr, sizeof(addr));`

Attaches your socket to that address+port — it's what "claims" port 8080. The cast `(sockaddr*)&addr` looks ugly and is a historical wart: `bind` was designed to accept any address family, so its parameter is the generic base type `sockaddr*`, and you cast your specific `sockaddr_in*` to it. `sizeof(addr)` tells it how many bytes the real struct is so it knows how to interpret them. Returns `-1` on failure (port taken, or trying to bind a port below 1024 without privileges).

## `listen(server_fd, 16);`

Flips the socket from a default (potentially outgoing) socket into a passive one that accepts incoming connections. The `16` is the backlog: the kernel completes TCP handshakes on its own and parks finished-but-not-yet-accepted connections in a queue; 16 caps how many can wait there before new connection attempts get refused. Under heavy load you'd raise this.

## The accept loop

This is where the server does its actual job, forever.

**`int conn = accept(server_fd, nullptr, nullptr);`**
Pulls one connection off the backlog queue and returns a brand-new file descriptor representing that one client's connection. Key mental model: `server_fd` is the "front desk" that only ever accepts; `conn` is the private line to one specific client. If the queue is empty, `accept` **blocks** — the program freezes on this line until someone connects. The two `nullptr`s are where you could ask "who connected?" (their IP/port gets filled into a struct you pass); passing null just means you don't care.

**`char buf[1024];`**
A 1 KB scratch buffer on the stack to hold incoming bytes.

**`ssize_t n = read(conn, buf, sizeof(buf));`**
Reads up to 1024 bytes from the client into `buf` and returns `n`, the number actually read. `ssize_t` is a signed size type precisely so it can return `-1` on error and `0` on EOF (client hung up). The word "up to" is important and foreshadows stage 1: TCP is a raw byte stream with no message boundaries, so one read might return part of what the client sent, or several sends glued together. You get bytes, not messages — imposing structure on them is exactly what a protocol is.

**`write(conn, buf, n);`**
Sends those same `n` bytes straight back down the connection — the "echo." (Notice it writes to `conn`, the client's fd, not `server_fd`.)

**`close(conn);`**
Hangs up on this client and releases the fd. Then the loop returns to `accept` to wait for the next one. This is why `nc` disconnects after a single line — you handle exactly one read, then close.

## Two limitations to carry forward

The later stages exist to fix these:

1. `accept` blocking means you serve strictly one client at a time — while you're mid-conversation with one, a second just waits in the backlog. That's what the event loop (stage 3) removes.
2. `read` handing you unframed bytes is what stage 1's RESP parsing addresses.

Notice too that `server_fd` is never closed — intentional, since the program runs until you kill it.
