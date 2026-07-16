#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <cctype>
#include <string>
#include <vector>
#include <unordered_map>
#include <iostream>

// Parse a RESP array-of-bulk-strings into its parts.
// Simplifying assumption (still): `input` holds exactly one complete command.
std::vector<std::string> parse_command(const std::string& input) {
    std::vector<std::string> command_parts;
    size_t cursor = 0;

    if (input.empty() || input[cursor] != '*') {
        return command_parts; // not an arr: bail
    }
    cursor++; // skip "*"

    size_t line_end = input.find("\r\n", cursor);
    int part_count = std::stoi(input.substr(cursor, line_end - cursor));
    cursor = line_end + 2;

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

std::string encode_bulk_string(const std::string& value) {
    return "$" + std::to_string(value.size()) + "\r\n" + value + "\r\n";
}

std::string wrong_argument_count(const std::string& command_name) {
    return "-ERR wrong number of arguments for '" + command_name + "' command\r\n";
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

    // The entire database. Lives outside both loops, so it survives across
    // commands and across client connections — but dies with the process.
    std::unordered_map<std::string, std::string> store;

    while (true) {
        int client_fd = accept(server_fd, nullptr, nullptr);

        char read_buffer[4096];
        while (true) {                                       // serve this client until they leave
            ssize_t bytes_read = read(client_fd, read_buffer, sizeof(read_buffer));
            if (bytes_read <= 0) break;                      // 0 = client hung up, -1 = error

            std::string request(read_buffer, bytes_read);
            std::vector<std::string> command_parts = parse_command(request);
            if (command_parts.empty()) continue;

            std::string command_name = command_parts[0];
            for (char& c : command_name) c = toupper(c);     // Redis commands are case-insensitive

            std::string response;
            if (command_name == "PING") {
                response = "+PONG\r\n";
            } else if (command_name == "ECHO") {
                if (command_parts.size() == 2) {
                    response = encode_bulk_string(command_parts[1]);
                } else {
                    response = wrong_argument_count("echo");
                }
            } else if (command_name == "SET") {
                if (command_parts.size() == 3) {
                    store[command_parts[1]] = command_parts[2];      // insert or overwrite
                    response = "+OK\r\n";
                } else {
                    response = wrong_argument_count("set");
                }
            } else if (command_name == "GET") {
                if (command_parts.size() == 2) {
                    auto entry = store.find(command_parts[1]);       // find, NOT operator[]
                    if (entry != store.end()) {
                        response = encode_bulk_string(entry->second);
                    } else {
                        response = "$-1\r\n";                        // null bulk string: no such key
                    }
                } else {
                    response = wrong_argument_count("get");
                }
            } else if (command_name == "DEL") {
                if (command_parts.size() >= 2) {                     // variadic: DEL a b c is legal
                    int deleted_count = 0;
                    for (size_t i = 1; i < command_parts.size(); i++) {
                        deleted_count += store.erase(command_parts[i]);  // erase returns 0 or 1
                    }
                    response = ":" + std::to_string(deleted_count) + "\r\n";
                } else {
                    response = wrong_argument_count("del");
                }
            } else if (command_name == "COMMAND") {
                response = "+OK\r\n";                        // redis-cli probes this on connect
            } else {
                response = "-ERR unknown command '" + command_parts[0] + "'\r\n";
            }

            write(client_fd, response.data(), response.size());
        }
        close(client_fd);
    }
}
