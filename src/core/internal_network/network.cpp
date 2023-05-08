// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

#include "common/error.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#elif YUZU_UNIX
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#else
#error "Unimplemented platform"
#endif

#include "common/assert.h"
#include "common/common_types.h"
#include "common/logging/log.h"
#include "common/settings.h"
#include "core/internal_network/network.h"
#include "core/internal_network/network_interface.h"
#include "core/internal_network/sockets.h"
#include "network/network.h"

namespace Network {

namespace {

#ifdef _WIN32

using socklen_t = int;

void Initialize() {
    WSADATA wsa_data;
    (void)WSAStartup(MAKEWORD(2, 2), &wsa_data);
}

void Finalize() {
    WSACleanup();
}

sockaddr TranslateFromSockAddrIn(SockAddrIn input) {
    sockaddr_in result;

#if YUZU_UNIX
    result.sin_len = sizeof(result);
#endif

    switch (static_cast<Domain>(input.family)) {
    case Domain::INET:
        result.sin_family = AF_INET;
        break;
    default:
        UNIMPLEMENTED_MSG("Unhandled sockaddr family={}", input.family);
        result.sin_family = AF_INET;
        break;
    }

    result.sin_port = htons(input.portno);

    auto& ip = result.sin_addr.S_un.S_un_b;
    ip.s_b1 = input.ip[0];
    ip.s_b2 = input.ip[1];
    ip.s_b3 = input.ip[2];
    ip.s_b4 = input.ip[3];

    sockaddr addr;
    std::memcpy(&addr, &result, sizeof(addr));
    return addr;
}

LINGER MakeLinger(bool enable, u32 linger_value) {
    ASSERT(linger_value <= std::numeric_limits<u_short>::max());

    LINGER value;
    value.l_onoff = enable ? 1 : 0;
    value.l_linger = static_cast<u_short>(linger_value);
    return value;
}

bool EnableNonBlock(SOCKET fd, bool enable) {
    u_long value = enable ? 1 : 0;
    return ioctlsocket(fd, FIONBIO, &value) != SOCKET_ERROR;
}

Errno TranslateNativeError(int e) {
    switch (e) {
    case WSAEBADF:
        return Errno::BADF;
    case WSAEINVAL:
        return Errno::INVAL;
    case WSAEMFILE:
        return Errno::MFILE;
    case WSAENOTCONN:
        return Errno::NOTCONN;
    case WSAEWOULDBLOCK:
        return Errno::AGAIN;
    case WSAECONNREFUSED:
        return Errno::CONNREFUSED;
    case WSAECONNRESET:
        return Errno::CONNRESET;
    case WSAEHOSTUNREACH:
        return Errno::HOSTUNREACH;
    case WSAENETDOWN:
        return Errno::NETDOWN;
    case WSAENETUNREACH:
        return Errno::NETUNREACH;
    case WSAEMSGSIZE:
        return Errno::MSGSIZE;
    case WSAETIMEDOUT:
        return Errno::TIMEDOUT;
    default:
        UNIMPLEMENTED_MSG("Unimplemented errno={}", e);
        return Errno::OTHER;
    }
}

#elif YUZU_UNIX // ^ _WIN32 v YUZU_UNIX

using SOCKET = int;
using WSAPOLLFD = pollfd;
using ULONG = u64;

constexpr SOCKET SOCKET_ERROR = -1;

constexpr int SD_RECEIVE = SHUT_RD;
constexpr int SD_SEND = SHUT_WR;
constexpr int SD_BOTH = SHUT_RDWR;

void Initialize() {}

void Finalize() {}

sockaddr TranslateFromSockAddrIn(SockAddrIn input) {
    sockaddr_in result;

    switch (static_cast<Domain>(input.family)) {
    case Domain::INET:
        result.sin_family = AF_INET;
        break;
    default:
        UNIMPLEMENTED_MSG("Unhandled sockaddr family={}", input.family);
        result.sin_family = AF_INET;
        break;
    }

    result.sin_port = htons(input.portno);

    result.sin_addr.s_addr = input.ip[0] | input.ip[1] << 8 | input.ip[2] << 16 | input.ip[3] << 24;

    sockaddr addr;
    std::memcpy(&addr, &result, sizeof(addr));
    return addr;
}

int WSAPoll(WSAPOLLFD* fds, ULONG nfds, int timeout) {
    return poll(fds, static_cast<nfds_t>(nfds), timeout);
}

int closesocket(SOCKET fd) {
    return close(fd);
}

linger MakeLinger(bool enable, u32 linger_value) {
    linger value;
    value.l_onoff = enable ? 1 : 0;
    value.l_linger = linger_value;
    return value;
}

bool EnableNonBlock(int fd, bool enable) {
    int flags = fcntl(fd, F_GETFL);
    if (flags == -1) {
        return false;
    }
    if (enable) {
        flags |= O_NONBLOCK;
    } else {
        flags &= ~O_NONBLOCK;
    }
    return fcntl(fd, F_SETFL, flags) == 0;
}

Errno TranslateNativeError(int e) {
    switch (e) {
    case EBADF:
        return Errno::BADF;
    case EINVAL:
        return Errno::INVAL;
    case EMFILE:
        return Errno::MFILE;
    case ENOTCONN:
        return Errno::NOTCONN;
    case EAGAIN:
        return Errno::AGAIN;
    case ECONNREFUSED:
        return Errno::CONNREFUSED;
    case ECONNRESET:
        return Errno::CONNRESET;
    case EHOSTUNREACH:
        return Errno::HOSTUNREACH;
    case ENETDOWN:
        return Errno::NETDOWN;
    case ENETUNREACH:
        return Errno::NETUNREACH;
    case EMSGSIZE:
        return Errno::MSGSIZE;
    case ETIMEDOUT:
        return Errno::TIMEDOUT;
    default:
        UNIMPLEMENTED_MSG("Unimplemented errno={}", e);
        return Errno::OTHER;
    }
}

#endif

Errno GetAndLogLastError() {
#ifdef _WIN32
    int e = WSAGetLastError();
#else
    int e = errno;
#endif
    const Errno err = TranslateNativeError(e);
    if (err == Errno::AGAIN || err == Errno::TIMEDOUT) {
        return err;
    }
    LOG_ERROR(Network, "Socket operation error: {}", Common::NativeErrorToString(e));
    return err;
}

int TranslateDomain(Domain domain) {
    switch (domain) {
    case Domain::INET:
        return AF_INET;
    default:
        UNIMPLEMENTED_MSG("Unimplemented domain={}", domain);
        return 0;
    }
}

int TranslateType(Type type) {
    switch (type) {
    case Type::STREAM:
        return SOCK_STREAM;
    case Type::DGRAM:
        return SOCK_DGRAM;
    default:
        UNIMPLEMENTED_MSG("Unimplemented type={}", type);
        return 0;
    }
}

int TranslateProtocol(Protocol protocol) {
    switch (protocol) {
    case Protocol::TCP:
        return IPPROTO_TCP;
    case Protocol::UDP:
        return IPPROTO_UDP;
    default:
        UNIMPLEMENTED_MSG("Unimplemented protocol={}", protocol);
        return 0;
    }
}

SockAddrIn TranslateToSockAddrIn(sockaddr input_) {
    sockaddr_in input;
    std::memcpy(&input, &input_, sizeof(input));

    SockAddrIn result;

    switch (input.sin_family) {
    case AF_INET:
        result.family = Domain::INET;
        break;
    default:
        UNIMPLEMENTED_MSG("Unhandled sockaddr family={}", input.sin_family);
        result.family = Domain::INET;
        break;
    }

    result.portno = ntohs(input.sin_port);

    result.ip = TranslateIPv4(input.sin_addr);

    return result;
}

short TranslatePollEvents(PollEvents events) {
    short result = 0;

    if (True(events & PollEvents::In)) {
        events &= ~PollEvents::In;
        result |= POLLIN;
    }
    if (True(events & PollEvents::Pri)) {
        events &= ~PollEvents::Pri;
#ifdef _WIN32
        LOG_WARNING(Service, "Winsock doesn't support POLLPRI");
#else
        result |= POLLPRI;
#endif
    }
    if (True(events & PollEvents::Out)) {
        events &= ~PollEvents::Out;
        result |= POLLOUT;
    }

    UNIMPLEMENTED_IF_MSG((u16)events != 0, "Unhandled guest events=0x{:x}", (u16)events);

    return result;
}

PollEvents TranslatePollRevents(short revents) {
    PollEvents result{};
    const auto translate = [&result, &revents](short host, PollEvents guest) {
        if ((revents & host) != 0) {
            revents &= static_cast<short>(~host);
            result |= guest;
        }
    };

    translate(POLLIN, PollEvents::In);
    translate(POLLPRI, PollEvents::Pri);
    translate(POLLOUT, PollEvents::Out);
    translate(POLLERR, PollEvents::Err);
    translate(POLLHUP, PollEvents::Hup);

    UNIMPLEMENTED_IF_MSG(revents != 0, "Unhandled host revents=0x{:x}", revents);

    return result;
}

} // Anonymous namespace

NetworkInstance::NetworkInstance() {
    Initialize();
}

NetworkInstance::~NetworkInstance() {
    Finalize();
}

std::optional<IPv4Address> GetHostIPv4Address() {
    const auto network_interface = Network::GetSelectedNetworkInterface();
    if (!network_interface.has_value()) {
        LOG_DEBUG(Network, "GetSelectedNetworkInterface returned no interface");
        return {};
    }

    std::array<char, 16> ip_addr = {};
    ASSERT(inet_ntop(AF_INET, &network_interface->ip_address, ip_addr.data(), sizeof(ip_addr)) !=
           nullptr);
    return TranslateIPv4(network_interface->ip_address);
}

std::pair<s32, Errno> Poll(std::vector<PollFD>& pollfds, s32 timeout) {
    const size_t num = pollfds.size();

    std::vector<WSAPOLLFD> host_pollfds(pollfds.size());
    std::transform(pollfds.begin(), pollfds.end(), host_pollfds.begin(), [](PollFD fd) {
        WSAPOLLFD result;
        result.fd = fd.socket->GetFD();
        result.events = TranslatePollEvents(fd.events);
        result.revents = 0;
        return result;
    });

    const int result = WSAPoll(host_pollfds.data(), static_cast<ULONG>(num), timeout);
    if (result == 0) {
        ASSERT(std::all_of(host_pollfds.begin(), host_pollfds.end(),
                           [](WSAPOLLFD fd) { return fd.revents == 0; }));
        return {0, Errno::SUCCESS};
    }

    for (size_t i = 0; i < num; ++i) {
        pollfds[i].revents = TranslatePollRevents(host_pollfds[i].revents);
    }

    if (result > 0) {
        return {result, Errno::SUCCESS};
    }

    ASSERT(result == SOCKET_ERROR);

    return {-1, GetAndLogLastError()};
}

Socket::~Socket() {
    if (fd == INVALID_SOCKET) {
        return;
    }
    (void)closesocket(fd);
    fd = INVALID_SOCKET;
}

Socket::Socket(Socket&& rhs) noexcept {
    fd = std::exchange(rhs.fd, INVALID_SOCKET);
}

template <typename T>
Errno Socket::SetSockOpt(SOCKET fd_, int option, T value) {
    const int result =
        setsockopt(fd_, SOL_SOCKET, option, reinterpret_cast<const char*>(&value), sizeof(value));
    if (result != SOCKET_ERROR) {
        return Errno::SUCCESS;
    }
    return GetAndLogLastError();
}

Errno Socket::Initialize(Domain domain, Type type, Protocol protocol) {
    fd = socket(TranslateDomain(domain), TranslateType(type), TranslateProtocol(protocol));
    if (fd != INVALID_SOCKET) {
        return Errno::SUCCESS;
    }

    return GetAndLogLastError();
}

std::pair<SocketBase::AcceptResult, Errno> Socket::Accept() {
    sockaddr addr;
    socklen_t addrlen = sizeof(addr);
    const SOCKET new_socket = accept(fd, &addr, &addrlen);

    if (new_socket == INVALID_SOCKET) {
        return {AcceptResult{}, GetAndLogLastError()};
    }

    ASSERT(addrlen == sizeof(sockaddr_in));

    AcceptResult result{
        .socket = std::make_unique<Socket>(new_socket),
        .sockaddr_in = TranslateToSockAddrIn(addr),
    };

    return {std::move(result), Errno::SUCCESS};
}

Errno Socket::Connect(SockAddrIn addr_in) {
    const sockaddr host_addr_in = TranslateFromSockAddrIn(addr_in);
    if (connect(fd, &host_addr_in, sizeof(host_addr_in)) != SOCKET_ERROR) {
        return Errno::SUCCESS;
    }

    return GetAndLogLastError();
}

std::pair<SockAddrIn, Errno> Socket::GetPeerName() {
    sockaddr addr;
    socklen_t addrlen = sizeof(addr);
    if (getpeername(fd, &addr, &addrlen) == SOCKET_ERROR) {
        return {SockAddrIn{}, GetAndLogLastError()};
    }

    ASSERT(addrlen == sizeof(sockaddr_in));
    return {TranslateToSockAddrIn(addr), Errno::SUCCESS};
}

std::pair<SockAddrIn, Errno> Socket::GetSockName() {
    sockaddr addr;
    socklen_t addrlen = sizeof(addr);
    if (getsockname(fd, &addr, &addrlen) == SOCKET_ERROR) {
        return {SockAddrIn{}, GetAndLogLastError()};
    }

    ASSERT(addrlen == sizeof(sockaddr_in));
    return {TranslateToSockAddrIn(addr), Errno::SUCCESS};
}

Errno Socket::Bind(SockAddrIn addr) {
    const sockaddr addr_in = TranslateFromSockAddrIn(addr);
    if (bind(fd, &addr_in, sizeof(addr_in)) != SOCKET_ERROR) {
        return Errno::SUCCESS;
    }

    return GetAndLogLastError();
}

Errno Socket::Listen(s32 backlog) {
    if (listen(fd, backlog) != SOCKET_ERROR) {
        return Errno::SUCCESS;
    }

    return GetAndLogLastError();
}

Errno Socket::Shutdown(ShutdownHow how) {
    int host_how = 0;
    switch (how) {
    case ShutdownHow::RD:
        host_how = SD_RECEIVE;
        break;
    case ShutdownHow::WR:
        host_how = SD_SEND;
        break;
    case ShutdownHow::RDWR:
        host_how = SD_BOTH;
        break;
    default:
        UNIMPLEMENTED_MSG("Unimplemented flag how={}", how);
        return Errno::SUCCESS;
    }
    if (shutdown(fd, host_how) != SOCKET_ERROR) {
        return Errno::SUCCESS;
    }

    return GetAndLogLastError();
}

std::pair<s32, Errno> Socket::Recv(int flags, std::vector<u8>& message) {
    ASSERT(flags == 0);
    ASSERT(message.size() < static_cast<size_t>(std::numeric_limits<int>::max()));

    const auto result =
        recv(fd, reinterpret_cast<char*>(message.data()), static_cast<int>(message.size()), 0);
    if (result != SOCKET_ERROR) {
        return {static_cast<s32>(result), Errno::SUCCESS};
    }

    return {-1, GetAndLogLastError()};
}

std::pair<s32, Errno> Socket::RecvFrom(int flags, std::vector<u8>& message, SockAddrIn* addr) {
    ASSERT(flags == 0);
    ASSERT(message.size() < static_cast<size_t>(std::numeric_limits<int>::max()));

    sockaddr addr_in{};
    socklen_t addrlen = sizeof(addr_in);
    socklen_t* const p_addrlen = addr ? &addrlen : nullptr;
    sockaddr* const p_addr_in = addr ? &addr_in : nullptr;

    const auto result = recvfrom(fd, reinterpret_cast<char*>(message.data()),
                                 static_cast<int>(message.size()), 0, p_addr_in, p_addrlen);
    if (result != SOCKET_ERROR) {
        if (addr) {
            ASSERT(addrlen == sizeof(addr_in));
            *addr = TranslateToSockAddrIn(addr_in);
        }
        return {static_cast<s32>(result), Errno::SUCCESS};
    }

    return {-1, GetAndLogLastError()};
}

std::pair<s32, Errno> Socket::Send(std::span<const u8> message, int flags) {
    ASSERT(message.size() < static_cast<size_t>(std::numeric_limits<int>::max()));
    ASSERT(flags == 0);

    const auto result = send(fd, reinterpret_cast<const char*>(message.data()),
                             static_cast<int>(message.size()), 0);
    if (result != SOCKET_ERROR) {
        return {static_cast<s32>(result), Errno::SUCCESS};
    }

    return {-1, GetAndLogLastError()};
}

std::pair<s32, Errno> Socket::SendTo(u32 flags, std::span<const u8> message,
                                     const SockAddrIn* addr) {
    ASSERT(flags == 0);

    const sockaddr* to = nullptr;
    const int to_len = addr ? sizeof(sockaddr) : 0;
    sockaddr host_addr_in;

    if (addr) {
        host_addr_in = TranslateFromSockAddrIn(*addr);
        to = &host_addr_in;
    }

    const auto result = sendto(fd, reinterpret_cast<const char*>(message.data()),
                               static_cast<int>(message.size()), 0, to, to_len);
    if (result != SOCKET_ERROR) {
        return {static_cast<s32>(result), Errno::SUCCESS};
    }

    return {-1, GetAndLogLastError()};
}

Errno Socket::Close() {
    [[maybe_unused]] const int result = closesocket(fd);
    ASSERT(result == 0);
    fd = INVALID_SOCKET;

    return Errno::SUCCESS;
}

Errno Socket::SetLinger(bool enable, u32 linger) {
    return SetSockOpt(fd, SO_LINGER, MakeLinger(enable, linger));
}

Errno Socket::SetReuseAddr(bool enable) {
    return SetSockOpt<u32>(fd, SO_REUSEADDR, enable ? 1 : 0);
}

Errno Socket::SetKeepAlive(bool enable) {
    return SetSockOpt<u32>(fd, SO_KEEPALIVE, enable ? 1 : 0);
}

Errno Socket::SetBroadcast(bool enable) {
    return SetSockOpt<u32>(fd, SO_BROADCAST, enable ? 1 : 0);
}

Errno Socket::SetSndBuf(u32 value) {
    return SetSockOpt(fd, SO_SNDBUF, value);
}

Errno Socket::SetRcvBuf(u32 value) {
    return SetSockOpt(fd, SO_RCVBUF, value);
}

Errno Socket::SetSndTimeo(u32 value) {
    return SetSockOpt(fd, SO_SNDTIMEO, value);
}

Errno Socket::SetRcvTimeo(u32 value) {
    return SetSockOpt(fd, SO_RCVTIMEO, value);
}

Errno Socket::SetNonBlock(bool enable) {
    if (EnableNonBlock(fd, enable)) {
        return Errno::SUCCESS;
    }
    return GetAndLogLastError();
}

bool Socket::IsOpened() const {
    return fd != INVALID_SOCKET;
}

void Socket::HandleProxyPacket(const ProxyPacket& packet) {
    LOG_WARNING(Network, "ProxyPacket received, but not in Proxy mode!");
}

} // namespace Network
