#include "kds/server/tcp_server.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <string>
#include <string_view>

namespace kds::server {

StatusOr<TcpServer> TcpServer::Listen(std::uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return Status::IoError(std::string("socket() failed: ") + std::strerror(errno));
    }

    int reuse = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);

    // The reinterpret_cast below is the standard POSIX sockaddr_in* ->
    // sockaddr* idiom the socket API itself requires - not the on-disk
    // struct-over-buffer cast rules.md #2 forbids (that rule is about our
    // own persisted formats, not interop with an OS ABI we don't control).
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        Status s = Status::IoError(std::string("bind() failed: ") + std::strerror(errno));
        ::close(fd);
        return s;
    }
    if (::listen(fd, 16) < 0) {
        Status s = Status::IoError(std::string("listen() failed: ") + std::strerror(errno));
        ::close(fd);
        return s;
    }

    return TcpServer(fd);
}

TcpServer::TcpServer(TcpServer&& other) noexcept : listen_fd_(other.listen_fd_) {
    other.listen_fd_ = -1;
}

TcpServer& TcpServer::operator=(TcpServer&& other) noexcept {
    if (this != &other) {
        CloseIfOpen();
        listen_fd_ = other.listen_fd_;
        other.listen_fd_ = -1;
    }
    return *this;
}

TcpServer::~TcpServer() { CloseIfOpen(); }

void TcpServer::CloseIfOpen() noexcept {
    if (listen_fd_ >= 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
}

void TcpServer::Serve(CommandDispatcher& dispatcher) {
    while (true) {
        int client_fd = ::accept(listen_fd_, nullptr, nullptr);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            break;  // listening socket is broken; nothing more we can do
        }

        std::string buffer;
        bool client_done = false;
        bool server_should_stop = false;

        while (!client_done) {
            char chunk[4096];
            ssize_t n = ::read(client_fd, chunk, sizeof(chunk));
            if (n <= 0) {
                break;  // client closed the connection, or a read error
            }
            buffer.append(chunk, static_cast<std::size_t>(n));

            std::size_t nl;
            while ((nl = buffer.find('\n')) != std::string::npos) {
                std::string_view line(buffer.data(), nl);
                if (!line.empty() && line.back() == '\r') {
                    line.remove_suffix(1);  // tolerate CRLF clients
                }

                DispatchOutcome outcome = dispatcher.Dispatch(line);
                std::string reply = outcome.response + "\n";
                ::write(client_fd, reply.data(), reply.size());

                buffer.erase(0, nl + 1);

                if (outcome.should_stop) {
                    server_should_stop = true;
                    client_done = true;
                    break;
                }
            }
        }

        ::close(client_fd);
        if (server_should_stop) break;
    }
}

}  // namespace kds::server
