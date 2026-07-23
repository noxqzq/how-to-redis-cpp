#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <poll.h>
#include <cctype>
#include <string>
#include <vector>
#include <unordered_map>
#include <iostream>

// Try to parse ONE complete RESP command from the front of `buffer`.
// Returns the number of bytes it consumed, or 0 if the buffer doesn't hold a
// complete command yet (the rest is still in flight — try again after the
// next read). This is where stage 1's "one read = one command" assumption dies.
size_t try_parse_command(const std::string& buffer, std::vector<std::string>& command_parts) {
    command_parts.clear();
    size_t cursor = 0;

    if (buffer.empty() || buffer[0] != '*') return 0;   // no array header (yet)
    cursor++; // skip "*"

    size_t line_end = buffer.find("\r\n", cursor);
    if (line_end == std::string::npos) return 0;        // count line still incomplete
    int part_count = std::stoi(buffer.substr(cursor, line_end - cursor));
    cursor = line_end + 2;

    for (int i = 0; i < part_count; i++) {
        if (cursor >= buffer.size()) return 0;          // '$' hasn't arrived yet
        cursor++;                                       // skip '$'
        line_end = buffer.find("\r\n", cursor);
        if (line_end == std::string::npos) return 0;    // length line incomplete
        int part_length = std::stoi(buffer.substr(cursor, line_end - cursor));
        cursor = line_end + 2;
        if (cursor + part_length + 2 > buffer.size()) return 0;  // data not all here
        command_parts.push_back(buffer.substr(cursor, part_length));
        cursor += part_length + 2;                      // step past data + \r\n
    }
    return cursor;                                      // bytes this command occupied
}

std::string encode_bulk_string(const std::string& value) {
    return "$" + std::to_string(value.size()) + "\r\n" + value + "\r\n";
}

std::string wrong_argument_count(const std::string& command_name) {
    return "-ERR wrong number of arguments for '" + command_name + "' command\r\n";
}

// Stage 2's dispatch chain, unchanged — just moved out of main, which now has
// an event loop to run.
std::string execute_command(const std::vector<std::string>& command_parts,
                            std::unordered_map<std::string, std::string>& store) {
    std::string command_name = command_parts[0];
    for (char& c : command_name) c = toupper(c);         // Redis commands are case-insensitive

    if (command_name == "PING") {
        return "+PONG\r\n";
    } else if (command_name == "ECHO") {
        if (command_parts.size() == 2) {
            return encode_bulk_string(command_parts[1]);
        }
        return wrong_argument_count("echo");
    } else if (command_name == "SET") {
        if (command_parts.size() == 3) {
            store[command_parts[1]] = command_parts[2];  // insert or overwrite
            return "+OK\r\n";
        }
        return wrong_argument_count("set");
    } else if (command_name == "GET") {
        if (command_parts.size() == 2) {
            auto entry = store.find(command_parts[1]);   // find, NOT operator[]
            if (entry != store.end()) {
                return encode_bulk_string(entry->second);
            }
            return "$-1\r\n";                            // null bulk string: no such key
        }
        return wrong_argument_count("get");
    } else if (command_name == "DEL") {
        if (command_parts.size() >= 2) {                 // variadic: DEL a b c is legal
            int deleted_count = 0;
            for (size_t i = 1; i < command_parts.size(); i++) {
                deleted_count += store.erase(command_parts[i]);  // erase returns 0 or 1
            }
            return ":" + std::to_string(deleted_count) + "\r\n";
        }
        return wrong_argument_count("del");
    } else if (command_name == "COMMAND") {
        return "+OK\r\n";                                // redis-cli probes this on connect
    }
    return "-ERR unknown command '" + command_parts[0] + "'\r\n";
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

    std::unordered_map<std::string, std::string> store;

    // fd -> bytes received from that client but not yet parsed into a command.
    // THE per-connection state that makes serving many clients possible.
    std::unordered_map<int, std::string> input_buffers;

    // Every fd we want the kernel to watch. Slot 0 is always the listener.
    std::vector<pollfd> poll_set;
    poll_set.push_back({server_fd, POLLIN, 0});

    while (true) {
        poll(poll_set.data(), poll_set.size(), -1);      // sleep until ANY fd is ready

        if (poll_set[0].revents & POLLIN) {              // listener readable = connection waiting
            int client_fd = accept(server_fd, nullptr, nullptr);   // won't block: poll said so
            poll_set.push_back({client_fd, POLLIN, 0});
        }

        for (size_t i = 1; i < poll_set.size(); i++) {
            if (poll_set[i].revents == 0) continue;      // this client had nothing for us
            int client_fd = poll_set[i].fd;

            char read_buffer[4096];
            ssize_t bytes_read = read(client_fd, read_buffer, sizeof(read_buffer));
            if (bytes_read <= 0) {                       // hung up or error: forget this client
                close(client_fd);
                input_buffers.erase(client_fd);
                poll_set.erase(poll_set.begin() + i);
                i--;                                     // the vector shifted left under us
                continue;
            }

            std::string& buffer = input_buffers[client_fd];  // creates it on first read
            buffer.append(read_buffer, bytes_read);

            // Drain every complete command the buffer holds (there may be
            // several — pipelining — or none — a command still arriving).
            std::vector<std::string> command_parts;
            size_t bytes_consumed;
            while ((bytes_consumed = try_parse_command(buffer, command_parts)) > 0) {
                buffer.erase(0, bytes_consumed);
                if (command_parts.empty()) continue;     // "*0\r\n" — legal, means nothing
                std::string response = execute_command(command_parts, store);
                write(client_fd, response.data(), response.size());
            }
        }
    }
}
