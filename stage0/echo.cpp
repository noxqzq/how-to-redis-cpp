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
    listen(server_fd, 16);               // 16 = backlog of pending connections (raise if bigger load)
    std::cout << "listening on :8080\n";

    while (true) {
        int conn = accept(server_fd, nullptr, nullptr);  // blocks here
        char buf[1024];
        ssize_t n = read(conn, buf, sizeof(buf));
        write(conn, buf, n);             // echo it straight back
        close(conn);
    }
}
