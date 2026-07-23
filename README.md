# how-to-redis-cpp

Ever wondered what a "server" actually is? So did I. So I'm rebuilding Redis in C++ from scratch — sockets → protocol → key-value store — and writing down every step.

## How this repo works

Each stage is one folder holding a single `server.cpp` you can compile and run, plus a `README.md` that explains the code line by line — what's new, why it's built that way, and what it deliberately leaves broken for a later stage to fix. Start at [stage0](stage0/); every writeup ends by setting up the next one.

| Stage | What it builds | The big idea |
|-------|----------------|--------------|
| [stage0](stage0/) | an echo server | sockets: bind, listen, accept — bytes in, bytes out |
| [stage1](stage1/) | speaks RESP | a protocol is agreed structure on top of a byte stream |
| [stage2](stage2/) | SET / GET / DEL | the database is a map that outlives the command |
| [stage3](stage3/) | poll() event loop | serve every client at once — one thread, no locks |


## Running a stage

```sh
cd stage3
c++ -std=c++17 -Wall -Wextra -o server server.cpp
./server
```

Then talk to it with the official client (`brew install redis` / `apt install redis-tools`):

```
redis-cli -p 8080
127.0.0.1:8080> SET greeting hello
OK
127.0.0.1:8080> GET greeting
"hello"
127.0.0.1:8080> GET missing
(nil)
```

Stage 0 predates the protocol — talk to it with `nc localhost 8080` instead: type a line, get it echoed back.

## Where it's headed

More commands (`INCR`, `EXISTS`), expiring keys (`EXPIRE` / `TTL`), and persistence.
