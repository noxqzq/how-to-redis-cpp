# Stage 3 — Serving everyone at once (poll + the read buffer)

## Where stage 2 left off

Stage 2 is a real database — for exactly one client. Try it: open two terminals, connect `redis-cli` in both. The second one connects (the kernel completes the handshake and parks it in the backlog, remember stage 0) but every command it sends just... hangs. The server is stuck in the inner while-loop blocked on `read()` from client #1, and won't call `accept()` again until #1 disconnects. One slow client freezes the world.

## The core idea: never wait on ONE thing

The blocking calls were the problem: `accept()` waits for a connection, `read()` waits for one specific client's bytes, and while waiting, the process can do nothing else. There are two classic escapes:

1. **Threads** — one thread per client, each free to block. Works, but now the store is shared mutable state and you need locks.
2. **An event loop** — one thread that never blocks on any individual fd. Instead it hands the kernel its whole list of fds and asks one question: "which of these has something for me right now?" Then it handles exactly those, quickly, and asks again.

Redis famously chose door 2. One thread, no locks, and every command runs start-to-finish without interruption — which is also why Redis commands are atomic. This stage rebuilds `main()` around that loop.

## poll vs epoll vs kqueue (an honest correction)

Earlier stages said "epoll (stage 3)". Slight amendment: `epoll` is Linux-only, and this repo is being built on a Mac. So this stage uses `poll()` — POSIX, runs everywhere, and the **mental model is identical**. The differences are mechanical: `poll()` re-hands the kernel your whole fd list on every call and scans all of it (O(n) per wakeup); `epoll` (Linux) and `kqueue` (macOS/BSD) keep the interest-list registered in the kernel so a wakeup costs O(ready) instead of O(watched). At 10 connections you cannot tell them apart; at 100,000 you can. Real Redis abstracts over all three (its `ae.c` picks the best one at compile time). Learn `poll` first regardless — it's the concept with the least machinery around it.

## The one new call

```cpp
struct pollfd {
    int   fd;        // which descriptor to watch
    short events;    // what you care about  (you fill this)
    short revents;   // what actually happened (kernel fills this)
};

poll(poll_set.data(), poll_set.size(), -1);
```

You hand `poll()` an array of these. `events = POLLIN` means "tell me when a `read()` on this fd would **not** block." The `-1` timeout means sleep forever until at least one fd is ready. When `poll` returns, the kernel has marked each ready fd by setting bits in its `revents` ("returned events") — you scan the array and handle the marked ones. That's the whole API.

The elegant part is that the **listener** is watched the same way: `server_fd` becoming readable means "a connection is waiting in the backlog," i.e. `accept()` would not block. New client? That's just another fd — push it into `poll_set` and the kernel watches it too. Everything becomes one kind of thing: an fd that's ready or not.

## The event loop, in shape

```
while (true) {
    poll(...);                          <- the ONLY place we ever wait
    if (listener is ready)  accept, add fd to poll_set
    for (each client fd marked ready) {
        read its bytes                  <- guaranteed not to block
        append to that client's buffer
        drain complete commands, reply
    }
}
```

Compare stage 2's nested `while(true)`s: the inner loop **owned** one client until it left. Now no one owns anything — each iteration handles whichever clients happen to have bytes, a little each, round and round. Ten `redis-cli` sessions all feel served "simultaneously" even though a single thread is doing strictly one thing at a time. Concurrency without parallelism.

Bookkeeping when a client leaves: `read() <= 0` now means close the fd, erase its buffer, **and** remove it from `poll_set`. Note the `i--` after the erase — the vector shifted left, so the next element is now at the current index. Classic erase-while-iterating idiom.

## The read buffer — paying stage 1's debt

Stage 1 confessed its parser assumed one `read()` delivers exactly one complete command. In the event loop that assumption isn't just fragile, it's impossible: reads from a client arrive whenever `poll` says so, in whatever chunks TCP formed — half a command, or three commands glued together. The fix, promised back then, lands now:

```cpp
std::unordered_map<int, std::string> input_buffers;   // fd -> unparsed bytes
```

Every byte read from a client is **appended** to that client's own string. The parser is rewritten with a new contract — `try_parse_command`:

- **returns 0** — the buffer does not yet hold a complete command; keep the bytes, wait for the next read.
- **returns n > 0** — parsed one command; it occupied the first `n` bytes, so erase them and go again.

Inside, it's the same cursor walk as stage 1, but every step that used to assume bytes exist now checks: `find()` couldn't locate the `\r\n`? Not here yet — return 0. Fewer than `part_length + 2` bytes after the cursor? Return 0. Returning 0 throws away no work; the bytes stay in the buffer, and when the missing piece arrives the parse simply runs again from the top and succeeds.

Concretely: `*1\r\n$4\r\nPI` arrives → the `$4` promises 4 bytes but only 2 are here → 0, buffer keeps holding it. `NG\r\n` arrives → append, reparse → consumed 14 bytes, command `{"PING"}`.

And the drain loop:

```cpp
while ((bytes_consumed = try_parse_command(buffer, command_parts)) > 0) {
    buffer.erase(0, bytes_consumed);
    ...execute, reply...
}
```

handles the opposite case for free: if three commands arrived glued into one read, the loop extracts and answers all three. That's **pipelining** — a real Redis feature (clients batch commands to save round trips) — and it fell out of the buffer design without a line of extra code.

One aside: the buffer is fetched with `input_buffers[client_fd]` — `operator[]`, deliberately. Stage 2 warned `operator[]` creates missing entries; here that's exactly the behavior we want (first read from a new client conjures its empty buffer). Same tool, opposite situations — know which one you're in.

## What moved, what didn't

| Piece | What happened |
|-------|---------------|
| `try_parse_command` | stage 1's parser + completeness checks + "bytes consumed" return |
| `execute_command` | stage 2's dispatch chain, verbatim, moved out of `main()` into a function (main now has an event loop to run; the dispatch was crowding it) |
| `main()` | rebuilt: `poll_set`, `input_buffers`, the event loop |
| the store, the socket setup, every reply format | untouched |

The layering is starting to show its value: transport (this stage) changed radically; protocol and database logic didn't move.

## Naming notes (why these names)

| Name | Why |
|------|-----|
| `poll_set` | the set of fds the kernel watches for us |
| `revents` | kernel convention, kept: "returned events" — poll's answer, as opposed to `events`, your question |
| `input_buffers` | plural + per-fd: THE state that makes many clients possible |
| `try_parse_command` | the `try_` prefix signals it can fail benignly — failure (return 0) is a normal, expected outcome, not an error |
| `bytes_consumed` | says precisely what the return value means: how much of the buffer this command used up |
| `execute_command` | verb-first, like `encode_bulk_string`: takes parts, returns wire bytes |

## Try it

The stage-2 failure, fixed — two terminals:

```
# terminal 1                          # terminal 2
redis-cli -p 8080                     redis-cli -p 8080
127.0.0.1:8080> SET shared hello      127.0.0.1:8080> GET shared
OK                                    "hello"        <- while #1 is still connected
```

Pipelining — three commands, one packet, three replies:

```
printf '*1\r\n$4\r\nPING\r\n*1\r\n$4\r\nPING\r\n*1\r\n$4\r\nPING\r\n' | nc localhost 8080
+PONG
+PONG
+PONG
```

The split-command case needs a client that flushes mid-command — a few lines of Python:

```python
import socket, time
s = socket.create_connection(("127.0.0.1", 8080))
s.sendall(b"*1\r\n$4\r\nPI")     # half a PING...
time.sleep(0.5)
s.sendall(b"NG\r\n")             # ...the rest
print(s.recv(64))                # b'+PONG\r\n'
```

Stage 1's parser would have choked on the first fragment; now the buffer just holds it until the rest shows up.

## Honest limitations

1. **`write()` can block too.** If a client stops draining its socket and the kernel's send buffer fills, `write()` stalls the whole loop — the same disease we just cured on the read side. The full cure is per-connection **output** buffers plus asking `poll` for `POLLOUT`. Real servers do it; ours won't feel it at learning scale.
2. **Garbage wedges a connection.** Bytes that don't start with `*` make `try_parse_command` return 0 forever — the buffer sits there and that client never gets another reply. Real Redis replies `-ERR Protocol error` and closes. (Related: `std::stoi` on a malformed count still throws, unchecked, as it has since stage 1.)
3. **poll's O(n) scan** — see the epoll/kqueue section. Not a problem at this scale; the fix is a different syscall, not a different design.

## Next: stage 4

The database itself is due for growth. Natural candidates, in rough order: `EXISTS` and `INCR` (`INCR` introduces "strings that act like numbers" and the `-ERR value is not an integer` error), then `EXPIRE`/`TTL` (introduces time, lazy vs active expiry — a genuinely interesting design problem). After that, the horizon holds persistence (append-only file) and maybe the `kqueue`/`epoll` upgrade of this stage's loop.
