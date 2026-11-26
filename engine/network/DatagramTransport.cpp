#include "engine/network/DatagramTransport.h"
#include "engine/network/SteamTransport.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <cerrno>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <system_error>
#include <thread>
#include <utility>

namespace engine::net {
namespace {

using Clock = std::chrono::steady_clock;
constexpr auto ReceivePollInterval = std::chrono::milliseconds(2);
constexpr auto FlushTimeout = std::chrono::milliseconds(25);
constexpr auto MaxEmulatedLatencyMs = std::uint16_t{2000};
constexpr std::size_t QueueCapacity = 512;
constexpr std::size_t PacketBudget = 64;

#ifdef _WIN32
using Socket = SOCKET;
using AddressLength = int;
constexpr Socket InvalidSocket = INVALID_SOCKET;

int socketError() { return WSAGetLastError(); }
bool wouldBlock(int error) { return error == WSAEWOULDBLOCK; }
bool oversized(int error) { return error == WSAEMSGSIZE; }
void closeSocket(Socket socket) { closesocket(socket); }

bool makeNonblocking(Socket socket) {
    u_long enabled = 1;
    return ioctlsocket(socket, FIONBIO, &enabled) == 0;
}
#else
using Socket = int;
using AddressLength = socklen_t;
constexpr Socket InvalidSocket = -1;

int socketError() { return errno; }
bool wouldBlock(int error) { return error == EAGAIN || error == EWOULDBLOCK; }
bool oversized(int error) { return error == EMSGSIZE; }
void closeSocket(Socket socket) { close(socket); }

bool makeNonblocking(Socket socket) {
    int enabled = 1;
    return ioctl(socket, FIONBIO, &enabled) == 0;
}
#endif

sockaddr_in socketAddress(Endpoint endpoint) {
    sockaddr_in result{};
    result.sin_family = AF_INET;
    result.sin_addr.s_addr = htonl(endpoint.address);
    result.sin_port = htons(endpoint.port);
    return result;
}

Endpoint endpointFrom(const sockaddr_in& address) {
    return {ntohl(address.sin_addr.s_addr), ntohs(address.sin_port)};
}

std::string describeSocketError(std::string_view operation, int error) {
    const std::error_code code(error, std::system_category());
    return std::string(operation) + " (" + std::to_string(error) + "): " + code.message();
}

std::optional<unsigned> decimal(std::string_view text, unsigned limit) {
    if (text.empty()) return std::nullopt;
    unsigned value{};
    const auto* end = text.data() + text.size();
    const auto [position, error] = std::from_chars(text.data(), end, value);
    if (error != std::errc{} || position != end || value > limit) return std::nullopt;
    return value;
}

void fillDatagram(Datagram& destination, Endpoint peer, std::span<const std::uint8_t> data) {
    destination.peer = peer;
    destination.size = data.size();
    std::copy(data.begin(), data.end(), destination.data.begin());
}

NetworkEmulation normalizeEmulation(NetworkEmulation settings) {
    settings.latencyMs = std::min(settings.latencyMs, MaxEmulatedLatencyMs);
    settings.lossPercent = std::min<std::uint8_t>(settings.lossPercent, 100);
    return settings;
}

}

std::optional<Endpoint> parseEndpoint(std::string_view text, std::uint16_t defaultPort) {
    Endpoint endpoint{0x7f000001, defaultPort};
    const auto colon = text.find(':');
    if (colon != std::string_view::npos) {
        const auto port = decimal(text.substr(colon + 1), 65535);
        if (!port) return std::nullopt;
        endpoint.port = static_cast<std::uint16_t>(*port);
        text = text.substr(0, colon);
    }
    if (text == "localhost") return endpoint;

    endpoint.address = 0;
    for (unsigned part = 0; part < 4; ++part) {
        const auto dot = text.find('.');
        if ((dot == std::string_view::npos) != (part == 3)) return std::nullopt;
        const auto value = decimal(text.substr(0, dot), 255);
        if (!value) return std::nullopt;
        endpoint.address = (endpoint.address << 8) | *value;
        if (part != 3) text.remove_prefix(dot + 1);
    }
    return endpoint;
}

std::optional<Endpoint> resolveConnectEndpoint(std::string_view text, std::uint16_t defaultPort,
                                               std::string& error) {
    error.clear();
    const auto isUnicast = [](Endpoint endpoint) {
        const auto firstOctet = endpoint.address >> 24;
        return endpoint.port && firstOctet > 0 && firstOctet < 224;
    };
    if (const auto endpoint = parseEndpoint(text, defaultPort)) {
        if (isUnicast(*endpoint)) return endpoint;
        error = "Use a unicast IPv4 address and a port from 1 to 65535.";
        return std::nullopt;
    }

    auto port = static_cast<unsigned>(defaultPort);
    const auto colon = text.find(':');
    if (colon != std::string_view::npos) {
        port = decimal(text.substr(colon + 1), 65535).value_or(0);
        text = text.substr(0, colon);
    }
    constexpr std::string_view HostnameChars = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-.";
    if (text.empty() || text.size() > 253 || !port || text.find_first_not_of(HostnameChars) != text.npos ||
        text.find_first_not_of("0123456789.") == text.npos) {
        error = "Use a DNS hostname or IPv4 address followed by an optional :port.";
        return std::nullopt;
    }

#ifdef _WIN32
    WSADATA winsock{};
    if (const int status = WSAStartup(MAKEWORD(2, 2), &winsock); status != 0) {
        error = describeSocketError("Cannot initialize Winsock for DNS", status);
        return std::nullopt;
    }
    struct WinsockScope {
        ~WinsockScope() { WSACleanup(); }
    } winsockScope;
#endif
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    hints.ai_flags = AI_NUMERICSERV;
    const std::string hostname(text);
    const auto service = std::to_string(port);
    addrinfo* results{};
    const int status = getaddrinfo(hostname.c_str(), service.c_str(), &hints, &results);
    if (status == 0) {
        const std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> addresses(results, freeaddrinfo);
        for (const auto* result = addresses.get(); result; result = result->ai_next) {
            if (!result->ai_addr || result->ai_addrlen < sizeof(sockaddr_in) || result->ai_family != AF_INET) continue;
            const auto endpoint = endpointFrom(*reinterpret_cast<const sockaddr_in*>(result->ai_addr));
            if (isUnicast(endpoint)) return endpoint;
        }
    }
    error = status == 0 ? "No unicast IPv4 address returned for " + hostname + "."
        : "Could not resolve " + hostname + " (DNS status " + std::to_string(status) + ").";
    return std::nullopt;
}

std::string formatEndpoint(Endpoint endpoint) {
    if (endpoint.steamId) return "SteamID " + std::to_string(endpoint.steamId);
    const auto address = endpoint.address;
    return std::to_string(address >> 24) + "." + std::to_string((address >> 16) & 255)
        + "." + std::to_string((address >> 8) & 255) + "." + std::to_string(address & 255)
        + ":" + std::to_string(endpoint.port);
}

struct DatagramTransport::Impl {
    struct Pending {
        Datagram datagram;
        Clock::time_point due;
        std::uint64_t order{};
    };

    static bool later(const Pending& lhs, const Pending& rhs) {
        return lhs.due > rhs.due || (lhs.due == rhs.due && lhs.order > rhs.order);
    }

    Socket socket{InvalidSocket};
    std::unique_ptr<SteamTransport> steam;
#ifdef _WIN32
    bool winsockStarted{};
#endif
    Endpoint local;
    std::thread worker;
    mutable std::mutex mutex;
    std::condition_variable wake;
    std::condition_variable flushed;
    bool stopping{};
    bool flushing{};

    std::array<Pending, QueueCapacity> pending;
    std::size_t pendingCount{};
    std::uint64_t nextOrder{};
    std::optional<Clock::time_point> sending;
    std::array<Datagram, QueueCapacity> received;
    std::size_t receiveHead{};
    std::size_t receiveCount{};

    static constexpr std::uint32_t LossSeed = 0x8798da74u;
    std::uint32_t lossState{LossSeed};
    NetworkEmulation emulation;
    TransportStats counters;

    void releaseSocket() {
        if (socket != InvalidSocket) {
            closeSocket(socket);
            socket = InvalidSocket;
        }
#ifdef _WIN32
        if (winsockStarted) {
            WSACleanup();
            winsockStarted = false;
        }
#endif
    }

    bool dropByEmulation(std::uint8_t lossPercent) {
        if (lossPercent == 0) return false;
        lossState ^= lossState << 13;
        lossState ^= lossState >> 17;
        lossState ^= lossState << 5;
        return lossState % 100 < lossPercent;
    }

    bool readPacket() {
        std::array<std::uint8_t, MaxDatagramBytes + 1> buffer;
        sockaddr_in address{};
        AddressLength addressLength = sizeof(address);
        const auto bytes = recvfrom(socket, reinterpret_cast<char*>(buffer.data()),
            static_cast<int>(buffer.size()), 0, reinterpret_cast<sockaddr*>(&address), &addressLength);
        if (bytes < 0) {
            const int error = socketError();
            if (wouldBlock(error)) return false;
            std::lock_guard lock(mutex);
            if (oversized(error)) {
                ++counters.oversizedPackets;
                return true;
            }
            ++counters.socketErrors;
            return false;
        }

        const auto size = static_cast<std::size_t>(bytes);
        std::lock_guard lock(mutex);
        if (size > MaxDatagramBytes) {
            ++counters.oversizedPackets;
            return true;
        }
        ++counters.rxPackets;
        counters.rxBytes += size;
        if (receiveCount == received.size()) {
            ++counters.receiveQueueDrops;
            return true;
        }
        auto& entry = received[(receiveHead + receiveCount) % received.size()];
        fillDatagram(entry, endpointFrom(address), {buffer.data(), size});
        ++receiveCount;
        return true;
    }

    bool writePacket() {
        Datagram datagram;
        {
            std::lock_guard lock(mutex);
            if (pendingCount == 0 || pending.front().due > Clock::now()) return false;
            std::pop_heap(pending.begin(), pending.begin() + pendingCount, later);
            const auto& next = pending[--pendingCount];
            sending = next.due;
            datagram = next.datagram;
        }
        bool sent{};
        if (steam) {
            sent = steam->send(datagram.peer, std::span(datagram.data).first(datagram.size));
        } else {
            const auto destination = socketAddress(datagram.peer);
            const auto bytes = sendto(socket, reinterpret_cast<const char*>(datagram.data.data()),
                static_cast<int>(datagram.size), 0, reinterpret_cast<const sockaddr*>(&destination), sizeof(destination));
            sent = bytes >= 0 && static_cast<std::size_t>(bytes) == datagram.size;
        }
        std::lock_guard lock(mutex);
        if (!sent) {
            ++counters.socketErrors;
        } else {
            ++counters.txPackets;
            counters.txBytes += datagram.size;
        }
        sending.reset();
        if (flushing) flushed.notify_one();
        return true;
    }

    void run() {
        for (;;) {
            {
                std::lock_guard lock(mutex);
                if (stopping) return;
            }
            for (std::size_t i = 0; i < PacketBudget && readPacket(); ++i) {}
            for (std::size_t i = 0; i < PacketBudget && writePacket(); ++i) {}

            std::unique_lock lock(mutex);
            if (stopping) return;
            auto nextPoll = Clock::now() + ReceivePollInterval;
            if (pendingCount != 0) nextPoll = std::min(nextPoll, pending.front().due);
            wake.wait_until(lock, nextPoll);
        }
    }
};

DatagramTransport::DatagramTransport() : m_impl(std::make_unique<Impl>()) {}

DatagramTransport::~DatagramTransport() { stop(); }

bool DatagramTransport::start(Endpoint bind, std::string& error) {
    auto& self = *m_impl;
    if (self.worker.joinable() || self.steam) {
        error = "Datagram transport is already running";
        return false;
    }
    if (bind.steamId) {
        if (bind.address || bind.port != SteamVirtualPort) {
            error = "Invalid Steam listen endpoint";
            return false;
        }
        auto steam = std::make_unique<SteamTransport>();
        if (!steam->startServer(error)) return false;
        if (steam->localEndpoint() != bind) {
            error = "The Steam listen endpoint does not match the signed-in account";
            return false;
        }
        self.local = bind;
        self.steam = std::move(steam);
        self.counters = {};
        self.lossState = Impl::LossSeed;
        self.nextOrder = 0;
        return true;
    }
    const auto fail = [&](std::string message) {
        self.releaseSocket();
        error = std::move(message);
        return false;
    };
#ifdef _WIN32
    WSADATA winsock{};
    if (const int status = WSAStartup(MAKEWORD(2, 2), &winsock); status != 0)
        return fail(describeSocketError("Cannot initialize Winsock", status));
    self.winsockStarted = true;
#endif
    self.socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (self.socket == InvalidSocket) return fail(describeSocketError("Cannot open UDP socket", socketError()));

#ifdef _WIN32
    const int exclusive = 1;
    if (setsockopt(self.socket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive),
            sizeof(exclusive)) != 0)
        return fail(describeSocketError("Cannot reserve UDP bind address", socketError()));
#endif
    if (!makeNonblocking(self.socket)) return fail(describeSocketError("Cannot configure UDP socket", socketError()));

    auto address = socketAddress(bind);
    if (::bind(self.socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        const int status = socketError();
        return fail(describeSocketError("Cannot bind UDP " + formatEndpoint(bind), status));
    }
    AddressLength length = sizeof(address);
    if (getsockname(self.socket, reinterpret_cast<sockaddr*>(&address), &length) != 0)
        return fail(describeSocketError("Cannot read bound UDP address", socketError()));

    self.stopping = false;
    self.counters = {};
    self.lossState = Impl::LossSeed;
    self.nextOrder = 0;
    self.local = endpointFrom(address);
    try {
        self.worker = std::thread([this] { m_impl->run(); });
    } catch (const std::system_error& exception) {
        self.local = {};
        return fail(std::string("Cannot start UDP worker: ") + exception.what());
    }
    error.clear();
    return true;
}

bool DatagramTransport::startClient(Endpoint server, std::string& error) {
    if (!server.steamId) return start({0, 0}, error);
    auto& self = *m_impl;
    if (self.worker.joinable() || self.steam) {
        error = "Datagram transport is already running";
        return false;
    }
    auto steam = std::make_unique<SteamTransport>();
    if (!steam->startClient(server, error)) return false;
    self.local = steam->localEndpoint();
    self.steam = std::move(steam);
    self.counters = {};
    self.lossState = Impl::LossSeed;
    self.nextOrder = 0;
    return true;
}

void DatagramTransport::poll() {
    auto& self = *m_impl;
    if (!self.steam) return;
    self.steam->poll();
    for (std::size_t i = 0; i < PacketBudget && self.writePacket(); ++i) {}
}

void DatagramTransport::setSteamLobby(std::uint64_t lobbyId) {
    if (m_impl->steam) m_impl->steam->setLobby(lobbyId);
}

void DatagramTransport::flushReady() {
    auto& self = *m_impl;
    if (self.steam) {
        for (std::size_t i = 0; i < QueueCapacity && self.writePacket(); ++i) {}
        return;
    }
    if (!self.worker.joinable()) return;

    std::unique_lock lock(self.mutex);
    const auto cutoff = Clock::now();
    const auto readySent = [&] {
        return (self.pendingCount == 0 || self.pending.front().due > cutoff)
            && (!self.sending || *self.sending > cutoff);
    };
    if (readySent()) return;

    self.flushing = true;
    self.wake.notify_one();
    self.flushed.wait_until(lock, cutoff + FlushTimeout, readySent);
    self.flushing = false;
}

void DatagramTransport::stop() {
    auto& self = *m_impl;
    if (self.worker.joinable()) {
        {
            std::lock_guard lock(self.mutex);
            self.stopping = true;
        }
        self.wake.notify_one();
        self.worker.join();
    }
    self.steam.reset();
    self.releaseSocket();
    self.local = {};
    self.pendingCount = 0;
    self.receiveHead = 0;
    self.receiveCount = 0;
}

Endpoint DatagramTransport::localEndpoint() const { return m_impl->local; }

bool DatagramTransport::send(Endpoint peer, std::span<const std::uint8_t> data, EmulationPolicy policy,
                             std::optional<NetworkEmulation> emulationOverride) {
    return sendWithResult(peer, data, policy, emulationOverride).accepted;
}

DatagramSendResult DatagramTransport::sendWithResult(Endpoint peer, std::span<const std::uint8_t> data,
                                                    EmulationPolicy policy,
                                                    std::optional<NetworkEmulation> emulationOverride) {
    auto& self = *m_impl;
    if ((peer.steamId ? !self.steam : self.socket == InvalidSocket) || peer.port == 0 ||
        data.empty() || data.size() > MaxDatagramBytes) return {};
    const bool considered = policy == EmulationPolicy::Apply;
    {
        std::lock_guard lock(self.mutex);
        NetworkEmulation settings{};
        if (considered) {
            ++self.counters.emulatedPackets;
            settings = emulationOverride ? normalizeEmulation(*emulationOverride) : self.emulation;
        }
        if (self.dropByEmulation(settings.lossPercent)) {
            ++self.counters.emulatedDrops;
            return {true, considered, true};
        }
        if (self.pendingCount == self.pending.size()) {
            ++self.counters.sendQueueDrops;
            return {false, considered, false};
        }
        auto& entry = self.pending[self.pendingCount++];
        fillDatagram(entry.datagram, peer, data);
        entry.due = Clock::now() + std::chrono::milliseconds(settings.latencyMs);
        entry.order = self.nextOrder++;
        std::push_heap(self.pending.begin(), self.pending.begin() + self.pendingCount, Impl::later);
    }
    if (self.steam) flushReady();
    else self.wake.notify_one();
    return {true, considered, false};
}

bool DatagramTransport::receive(Datagram& datagram) {
    auto& self = *m_impl;
    if (self.steam) {
        if (!self.steam->receive(datagram)) return false;
        std::lock_guard lock(self.mutex);
        ++self.counters.rxPackets;
        self.counters.rxBytes += datagram.size;
        return true;
    }
    std::lock_guard lock(self.mutex);
    if (self.receiveCount == 0) return false;
    const auto& entry = self.received[self.receiveHead];
    fillDatagram(datagram, entry.peer, {entry.data.data(), entry.size});
    self.receiveHead = (self.receiveHead + 1) % self.received.size();
    --self.receiveCount;
    return true;
}

TransportStats DatagramTransport::stats() const {
    std::lock_guard lock(m_impl->mutex);
    return m_impl->counters;
}

void DatagramTransport::setEmulation(NetworkEmulation settings) {
    std::lock_guard lock(m_impl->mutex);
    m_impl->emulation = normalizeEmulation(settings);
}

}
