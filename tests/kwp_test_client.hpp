#pragma once

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "kds/wire/handshake.hpp"
#include "kds/wire/kwp.hpp"
#include "kds/wire/kwp_types.hpp"
#include "kds/wire/row_codec.hpp"

// **A KWP/1 client over a real socket, for the cells that need one**: the
// frames a test sends and the reader it decodes the server's with, in the
// blocking style a test thread can afford. Shared by `kwp_endpoint_test.cpp`
// and `ce_streaming_reply_test.cpp`.

namespace kds::server::kwp_test {

// `rcvbuf`, when set, caps the client's receive buffer, which a cell that
// needs a slow reader uses; set before `connect`, so the window it
// advertises is the small one.
inline int Connect(std::uint16_t port, int rcvbuf = 0) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    if (rcvbuf > 0) ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    for (int attempt = 0; attempt < 50; ++attempt) {
        if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) return fd;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ::close(fd);
    return -1;
}

inline void SendFrame(int fd, wire::ClientFrameType type, std::span<const std::byte> payload) {
    const auto frame = wire::EncodeFrame(static_cast<std::uint8_t>(type), 0, payload);
    ASSERT_EQ(::write(fd, frame.data(), frame.size()), static_cast<ssize_t>(frame.size()));
}

// Reads until `decoder` yields a frame, or the peer hangs up.
inline std::optional<wire::DecodedFrame> ReadFrame(int fd, wire::FrameDecoder& decoder) {
    while (true) {
        if (auto frame = decoder.PopFrame()) return frame;
        std::byte buf[4096];
        const ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n <= 0) return std::nullopt;
        if (!decoder.Feed(std::span<const std::byte>(buf, static_cast<std::size_t>(n))).ok()) {
            return std::nullopt;
        }
    }
}

inline bool Handshake(int fd, wire::FrameDecoder& decoder) {
    wire::ClientHello hello;
    hello.client_name = "endpoint-test";
    SendFrame(fd, wire::ClientFrameType::kHello, wire::EncodeClientHello(hello));
    auto s_hello = ReadFrame(fd, decoder);
    if (!s_hello.has_value() ||
        s_hello->type != static_cast<std::uint8_t>(wire::ServerFrameType::kHello)) {
        return false;
    }
    auto ready = ReadFrame(fd, decoder);
    return ready.has_value() &&
           ready->type == static_cast<std::uint8_t>(wire::ServerFrameType::kReady);
}

inline std::vector<std::byte> ParsePayload(std::string_view name, std::string_view sql) {
    wire::PayloadWriter w;
    w.Str(name);
    w.Text(sql);
    return w.Take();
}

inline std::vector<std::byte> BindPayload(std::string_view portal, std::string_view stmt) {
    wire::PayloadWriter w;
    w.Str(portal);
    w.Str(stmt);
    auto out = w.Take();
    (void)wire::EncodeBindParams({}, out);
    return out;
}

inline std::vector<std::byte> ExecutePayload(std::string_view portal, std::uint32_t max_rows) {
    wire::PayloadWriter w;
    w.Str(portal);
    w.U32(max_rows);
    return w.Take();
}

}  // namespace kds::server::kwp_test
