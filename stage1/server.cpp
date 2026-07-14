#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <cctype>
#include <string>
#include <vector>
#include <iostream>

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
            } else if (command_name == "ECHO" && command_parts.size() >= 2) {
                const std::string& message = command_parts[1];
                response = "$" + std::to_string(message.size()) + "\r\n" + message + "\r\n";
            } else if (command_name == "COMMAND") {
                response = "+OK\r\n";                         // redis-cli probes this on connect
            } else {
                response = "-ERR unknown command '" + command_parts[0] + "'\r\n";
            }

            write(client_fd, response.data(), response.size());
        }
        close(client_fd);
    }
}