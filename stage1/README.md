# Stage 1 — Speaking RESP (the jump from pipe to protocol)

## Where stage 0 left off

Stage 0 gave you a server that echoes raw bytes and hangs up after one read. It proved the skeleton works, but it doesn't **understand** anything the client says — it's a dumb pipe. Stage 1 fixes that: the program starts parsing the bytes into commands and replying in a real protocol, so the actual `redis-cli` can talk to it.

## The core idea: a protocol is agreed structure on top of bytes

TCP has no concept of "a message" — `read()` just hands you a run of bytes with no boundaries (you saw this in stage 0). A protocol is the two sides agreeing, in advance, on how to find message boundaries and meaning inside that stream.

Redis's protocol is called RESP (REdis Serialization Protocol). Its defining trick is that everything is **length-prefixed**: the sender tells you how many bytes are coming before you read them, so you never scan for delimiters or guess where something ends. That makes it far simpler to parse than HTTP.

## What redis-cli actually sends

Every command is encoded as a RESP "array of bulk strings." When you type

```
SET foo bar
```

these exact bytes go down the socket:

```
*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar\r\n
```

Decoded piece by piece:

```
*3\r\n          an array with 3 elements
$3\r\nSET\r\n    a bulk string of length 3, contents: SET
$3\r\nfoo\r\n    bulk string, length 3: foo
$3\r\nbar\r\n    bulk string, length 3: bar
```

Notice every string is preceded by its length (`$3`). `\r\n` (carriage-return + newline) is RESP's separator between parts.

## The five RESP types

Each is identified by its first byte:

| Byte | Type | Example |
|------|------|---------|
| `+` | simple string | `+OK\r\n` |
| `-` | error | `-ERR bad thing\r\n` |
| `:` | integer | `:42\r\n` |
| `$` | bulk string | `$5\r\nhello\r\n` |
| `*` | array | `*2\r\n...` |

The client talks to you in `*` / `$` (arrays of bulk strings). You reply in `+`, `-`, and `$` (and later `:` for commands that return counts).

## The parser

Turns the incoming bytes into a `vector<string>` like `{"SET","foo","bar"}`:

```cpp
std::vector<std::string> parse_command(const std::string& input) {
    std::vector<std::string> command_parts;
    size_t cursor = 0;

    if (input.empty() || input[cursor] != '*') return command_parts;  // not an array; bail
    cursor++;                                                          // skip '*'

    size_t line_end = input.find("\r\n", cursor);
    int part_count = std::stoi(input.substr(cursor, line_end - cursor));
    cursor = line_end + 2;                                             // step past \r\n

    for (int i = 0; i < part_count; i++) {
        cursor++;                                                      // skip '$'
        line_end = input.find("\r\n", cursor);
        int part_length = std::stoi(input.substr(cursor, line_end - cursor));
        cursor = line_end + 2;
        command_parts.push_back(input.substr(cursor, part_length));    // grab exactly this many bytes
        cursor += part_length + 2;                                     // step past data + \r\n
    }
    return command_parts;
}
```

Mental model: a single moving cursor walks left-to-right through the buffer. At each step it does one of two things — find the next `\r\n` to read a **number** (the array count, or a string's length), or jump forward by a known length to grab **data**. Because everything is length-prefixed, the parser never guesses. `cursor += part_length + 2` is the payoff: once `$5` said the string is 5 bytes, take exactly 5 and skip the trailing `\r\n` without inspecting it.

## Two structural changes from stage 0

### 1. Per-connection inner loop

Stage 0 did one read then `close()`. `redis-cli` stays connected and sends many commands, so now we loop:

```cpp
char read_buffer[4096];
while (true) {                                   // serve this client until they leave
    ssize_t bytes_read = read(client_fd, read_buffer, sizeof(read_buffer));
    if (bytes_read <= 0) break;                  // 0 = client hung up, -1 = error
    // ...parse, dispatch, reply...
}
close(client_fd);                                // only after they leave
```

The `if (bytes_read <= 0) break;` matters: `read()` returning 0 is TCP's way of saying the other side closed the connection (EOF). Without it you'd spin forever on a dead socket.

### 2. Command dispatch

After parsing, uppercase the command name (Redis is case-insensitive) and branch to build a RESP reply:

| Command | Reply |
|---------|-------|
| `PING` | `+PONG\r\n` |
| `ECHO <msg>` | bulk string echoing `<msg>` |
| `COMMAND` | `+OK\r\n` (newer redis-cli probes this on connect) |
| anything else | `-ERR unknown command '<name>'\r\n` |

## The full server (stage 1)

```cpp
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <cctype>
#include <string>
#include <vector>
#include <iostream>

// Parse a RESP array-of-bulk-strings into its parts.
// Simplifying assumption: `input` holds exactly one complete command.
std::vector<std::string> parse_command(const std::string& input) {
    std::vector<std::string> command_parts;
    size_t cursor = 0;

    if (input.empty() || input[cursor] != '*') return command_parts;
    cursor++;

    size_t line_end = input.find("\r\n", cursor);
    int part_count = std::stoi(input.substr(cursor, line_end - cursor));
    cursor = line_end + 2;

    for (int i = 0; i < part_count; i++) {
        cursor++;                                    // skip '$'
        line_end = input.find("\r\n", cursor);
        int part_length = std::stoi(input.substr(cursor, line_end - cursor));
        cursor = line_end + 2;
        command_parts.push_back(input.substr(cursor, part_length));
        cursor += part_length + 2;
    }
    return command_parts;
}

int main() {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    int reuse = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(8080);

    bind(server_fd, (sockaddr*)&server_addr, sizeof(server_addr));
    listen(server_fd, 16);
    std::cout << "listening on :8080\n";

    while (true) {
        int client_fd = accept(server_fd, nullptr, nullptr);

        char read_buffer[4096];
        while (true) {                               // serve this client until they leave
            ssize_t bytes_read = read(client_fd, read_buffer, sizeof(read_buffer));
            if (bytes_read <= 0) break;              // 0 = hung up, -1 = error

            std::string request(read_buffer, bytes_read);
            std::vector<std::string> command_parts = parse_command(request);
            if (command_parts.empty()) continue;

            std::string command_name = command_parts[0];
            for (char& c : command_name) c = toupper(c);  // case-insensitive

            std::string response;
            if (command_name == "PING") {
                response = "+PONG\r\n";
            } else if (command_name == "ECHO" && command_parts.size() >= 2) {
                const std::string& message = command_parts[1];
                response = "$" + std::to_string(message.size()) + "\r\n" + message + "\r\n";
            } else if (command_name == "COMMAND") {
                response = "+OK\r\n";                 // redis-cli probes this on connect
            } else {
                response = "-ERR unknown command '" + command_parts[0] + "'\r\n";
            }

            write(client_fd, response.data(), response.size());
        }
        close(client_fd);
    }
}
```

## Naming notes (why these names)

A name should say what the value **means**, not what type it is.

| Name | What it means |
|------|---------------|
| `input` | the bytes that came off the wire |
| `cursor` | where we are while walking through them |
| `line_end` | position of the next `\r\n` |
| `part_count` | how many strings the array holds |
| `part_length` | bytes in the current string |
| `command_parts` | the parsed result |
| `client_fd` | the private line to one client (pairs with `server_fd`, the front desk) |
| `bytes_read` | at `if (bytes_read <= 0)` you instantly read it as a hangup/error check |
| `reuse` | makes the `setsockopt` line self-documenting: "enable reuse" |

## Try it with the real client

Install redis-cli if needed: `sudo apt install redis-tools` (or `brew install redis`). Run your server, then:

```
redis-cli -p 8080
127.0.0.1:8080> PING
PONG
127.0.0.1:8080> ECHO hello
"hello"
127.0.0.1:8080> FOO
(error) ERR unknown command 'FOO'
```

When the **official** Redis client connects to your ~90-line program and behaves normally, you've proven you understand the protocol end to end.

## The one honest limitation

`parse_command` assumes a single `read()` delivered exactly one complete command. Over localhost with a human typing, that's reliably true — which is why it works now. But it's the fragile spot: TCP can split one command across two reads, or glue several commands into one (pipelining). The real fix is to accumulate incoming bytes in a persistent per-connection buffer and pull out complete commands as they form. Best folded in before stage 3, since the event-loop rewrite makes buffering mandatory anyway.

## Next: stage 2

Turn it into an actual database. Add a `std::unordered_map<string,string>` and implement SET / GET / DEL. This also introduces the `:` integer reply and the null bulk string (`$-1\r\n`) for "key not found."
