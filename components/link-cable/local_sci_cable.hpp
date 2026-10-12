// SPDX-License-Identifier: GPL-3.0-only
// Windows-local, owner-thread transport for the two independent Saturn SH-2
// SCI lanes. Inspired by the project's PS1 transport; this has a separate
// namespace, session tokens and Saturn clock/revision validation.
#pragma once
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <stdexcept>
#include <string>
#include "link_cable_service.hpp"

namespace saturn::link {
class LocalSciCable {
public:
    using State = LinkCableService::State;
    static constexpr std::uint64_t capacity = 4096;
    static bool ValidQuantum(unsigned q) {
        return q == 64 || q == 128 || q == 256 || q == 512 ||
               q == 1024 || q == 2048 || q == 4096;
    }
private:
    struct Message {
        std::uint64_t cycle;
        std::uint8_t value, lane, reserved[6];
    };
    struct alignas(64) Endpoint {
        volatile LONG pid, ready;
        alignas(8) volatile LONG64 token, progress, write, read;
        volatile LONG clock, fault;
        alignas(64) Message ring[capacity];
    };
    struct Shared {
        volatile LONG magic, quantum;
        alignas(8) volatile LONG64 nextToken;
        alignas(64) Endpoint endpoints[2];
    };
    static_assert(sizeof(Message) == 16 && offsetof(Endpoint, ring) == 64 &&
                  offsetof(Shared, endpoints) == 64, "Shared SCI ABI layout");
    static constexpr LONG magic = 0x53534331; // SSC1, not PS1 or YLS1
    HANDLE mapping = nullptr, peerProcess = nullptr;
    Shared* memory = nullptr;
    Endpoint *self = nullptr, *peer = nullptr;
    LONG peerPid = 0;
    std::uint64_t token = 0, peerToken = 0, origin = 0, boundary, quantum;
    std::uint64_t revision = 0, lastCycle = 0, lastMessage = 0, readPosition = 0;
    unsigned clock = 0;
    bool bound = false, failed = false;
    std::string error;
    std::deque<Message> pending[2];
    LinkCableService::Diagnostics diagnostics;
    static LONG Load(volatile LONG& v) { return InterlockedCompareExchange(&v, 0, 0); }
    static std::uint64_t Load64(volatile LONG64& v) {
        return std::uint64_t(InterlockedCompareExchange64(&v, 0, 0));
    }
    void Fail(const char* reason) {
        failed = true; error = reason;
        if (self) {
            InterlockedExchange(&self->fault, 1);
            InterlockedExchange(&self->ready, 0);
        }
        pending[0].clear(); pending[1].clear(); diagnostics.receiveQueue = 0;
    }
    bool PeerIdentity() const {
        return peer && Load(peer->pid) == peerPid && Load64(peer->token) == peerToken;
    }
    bool PeerAlive() const {
        return PeerIdentity() && peerProcess &&
               WaitForSingleObject(peerProcess, 0) == WAIT_TIMEOUT;
    }
    bool Wait(volatile LONG64* progress, std::uint64_t target, int (*cancel)()) {
        const auto started = GetTickCount64();
        unsigned spins = 0;
        for (;;) {
            if (cancel && cancel()) { Fail("Game shutdown"); return false; }
            if (!PeerIdentity()) { Fail("Shared SCI peer session changed; restart both"); return false; }
            if (Load(peer->fault)) { Fail("Shared SCI peer reported a fault; restart both"); return false; }
            if (progress ? Load64(*progress) >= target : Load(peer->ready) != 0) return true;
            YieldProcessor();
            if ((++spins & 255) == 0) SwitchToThread();
            if ((spins & 1023) == 0) {
                if (!PeerAlive()) { Fail("Shared SCI peer exited; restart both"); return false; }
                if (GetTickCount64() - started > 30000) {
                    Fail("Shared SCI progress timed out"); return false;
                }
                if (progress && !Load(peer->ready)) {
                    Fail("Shared SCI peer reset/disconnected; restart both"); return false;
                }
            }
        }
    }
    void Drain() {
        if (!Connected()) return;
        const auto written = Load64(peer->write);
        if (written < readPosition || written - readPosition > capacity) {
            Fail("Shared SCI receive queue invalid"); return;
        }
        const auto batch = written - readPosition;
        while (readPosition < written) {
            const Message message = peer->ring[readPosition % capacity];
            if (!PeerIdentity() || message.lane > 1 || message.cycle < lastMessage ||
                message.cycle > Load64(peer->progress) + 2 * quantum ||
                pending[0].size() + pending[1].size() >= 65536) {
                Fail("Shared SCI message order/range invalid"); return;
            }
            lastMessage = message.cycle;
            pending[message.lane].push_back(message);
            ++readPosition; ++diagnostics.received;
        }
        InterlockedExchange64(&peer->read, LONG64(readPosition));
        diagnostics.receiveQueue = pending[0].size() + pending[1].size();
        diagnostics.peakReceiveQueue = std::max(diagnostics.peakReceiveQueue, diagnostics.receiveQueue);
        diagnostics.largestReceiveBatch = std::max(diagnostics.largestReceiveBatch, batch);
    }
public:
    explicit LocalSciCable(unsigned channel, unsigned q = 4096) : boundary(q), quantum(q) {
        if (channel < 1024 || channel > 65535 || !ValidQuantum(q))
            throw std::invalid_argument("Invalid shared SCI channel/quantum");
        const std::string name = "Local\\SaturnSCI_v1_" + std::to_string(channel);
        HANDLE initializer = CreateMutexA(nullptr, FALSE, (name + "_init").c_str());
        if (!initializer) throw std::runtime_error("Cannot create shared SCI initializer");
        const auto locked = WaitForSingleObject(initializer, 2000);
        if (locked != WAIT_OBJECT_0 && locked != WAIT_ABANDONED) {
            CloseHandle(initializer); throw std::runtime_error("Shared SCI initializer timed out");
        }
        try {
            mapping = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(Shared), name.c_str());
            if (!mapping) throw std::runtime_error("Cannot create shared SCI mapping");
            const bool fresh = GetLastError() != ERROR_ALREADY_EXISTS;
            memory = static_cast<Shared*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared)));
            if (!memory) throw std::runtime_error("Cannot map shared SCI memory");
            if (fresh) {
                InterlockedExchange(&memory->quantum, LONG(q));
                InterlockedExchange(&memory->magic, magic);
            }
            if (Load(memory->magic) != magic || Load(memory->quantum) != LONG(q))
                throw std::runtime_error("Shared SCI version/quantum mismatch");
            for (unsigned side = 0; side < 2; ++side) {
                auto& endpoint = memory->endpoints[side];
                const LONG owner = Load(endpoint.pid);
                bool vacant = owner == 0;
                if (owner > 0) {
                    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, DWORD(owner));
                    vacant = process ? WaitForSingleObject(process, 0) == WAIT_OBJECT_0 :
                                       GetLastError() == ERROR_INVALID_PARAMETER;
                    if (process) CloseHandle(process);
                }
                if (!vacant) continue;
                self = &endpoint; peer = &memory->endpoints[1 - side];
                token = std::uint64_t(InterlockedIncrement64(&memory->nextToken));
                InterlockedExchange(&self->ready, 0);
                InterlockedExchange64(&self->token, LONG64(token));
                InterlockedExchange64(&self->progress, 0);
                InterlockedExchange64(&self->write, 0);
                InterlockedExchange64(&self->read, 0);
                InterlockedExchange(&self->clock, 0);
                InterlockedExchange(&self->fault, 0);
                InterlockedExchange(&self->pid, LONG(GetCurrentProcessId()));
                break;
            }
            if (!self) throw std::runtime_error("Shared SCI already has two instances");
        } catch (...) {
            ReleaseMutex(initializer); CloseHandle(initializer); Release(); throw;
        }
        ReleaseMutex(initializer); CloseHandle(initializer);
    }
    ~LocalSciCable() { Release(); }
    LocalSciCable(const LocalSciCable&) = delete;
    LocalSciCable& operator=(const LocalSciCable&) = delete;
    void Release() {
        if (self && Load64(self->token) == token) {
            InterlockedExchange(&self->ready, 0);
            InterlockedExchange(&self->pid, 0);
        }
        self = nullptr; peer = nullptr; bound = false;
        if (peerProcess) { CloseHandle(peerProcess); peerProcess = nullptr; }
        if (memory) { UnmapViewOfFile(memory); memory = nullptr; }
        if (mapping) { CloseHandle(mapping); mapping = nullptr; }
    }
    bool Connected() const {
        return !failed && bound && PeerIdentity() && !Load(peer->fault) && Load(peer->ready) != 0;
    }
    State GetState() const { return failed ? State::Error : Connected() ? State::Connected : State::Listening; }
    const std::string& GetError() const { return error; }
    LinkCableService::Diagnostics GetDiagnostics() const { return diagnostics; }
    std::uint64_t SessionEpoch() const { return token; }
    void Disconnect() { Fail("Shared SCI reset/disconnected; restart both instances"); }
    std::uint64_t Advance(std::uint64_t cycle, std::uint64_t rev, unsigned clockTag, int (*cancel)()) {
        constexpr auto unlimited = ~std::uint64_t(0);
        if (failed) return unlimited;
        if (bound && (cycle < lastCycle || rev != revision || clockTag != clock)) {
            Fail("Shared SCI clock/revision/timeline changed"); return unlimited;
        }
        if (!bound) {
            peerPid = Load(peer->pid); if (peerPid <= 0) return unlimited;
            peerToken = Load64(peer->token);
            peerProcess = OpenProcess(SYNCHRONIZE, FALSE, DWORD(peerPid));
            if (!peerProcess) { Fail("Cannot access shared SCI peer process"); return unlimited; }
            origin = cycle; revision = rev; clock = clockTag; bound = true;
            InterlockedExchange(&self->clock, LONG(clockTag));
            InterlockedExchange(&self->ready, 1);
            if (!Wait(nullptr, 0, cancel)) return unlimited;
        }
        lastCycle = cycle;
        if (!Connected()) { Fail("Shared SCI peer reset/disconnected; restart both"); return unlimited; }
        if (unsigned(Load(peer->clock)) != clockTag) {
            Fail("Shared SCI clock modes differ"); return unlimited;
        }
        const auto now = cycle - origin;
        if (now < boundary) return boundary - now;
        if (now >= boundary + quantum || boundary > std::uint64_t(INT64_MAX) - quantum) {
            Fail("Shared SCI cycle budget exceeded"); return unlimited;
        }
        InterlockedExchange64(&self->progress, LONG64(boundary));
        if (!Wait(&peer->progress, boundary, cancel)) return unlimited;
        boundary += quantum; return boundary - now;
    }
    void Send(bool master, std::uint8_t value, std::uint64_t cycle) {
        if (!Connected() || cycle < origin) return;
        const auto written = Load64(self->write), read = Load64(self->read);
        if (written < read || written - read >= capacity ||
            cycle - origin > std::uint64_t(INT64_MAX) - quantum) {
            Fail("Shared SCI send queue overflow/range invalid"); return;
        }
        self->ring[written % capacity] = {cycle - origin, value, std::uint8_t(master), {}};
        InterlockedExchange64(&self->write, LONG64(written + 1));
        ++diagnostics.sent; ++diagnostics.transmitted;
    }
    int Receive(bool master, std::uint64_t cycle) {
        if (!Connected() || cycle < origin) return -1;
        Drain(); auto& queue = pending[master ? 1 : 0];
        if (failed || !Connected() || queue.empty() || cycle - origin < queue.front().cycle + quantum) return -1;
        const auto value = queue.front().value; queue.pop_front(); ++diagnostics.delivered;
        diagnostics.receiveQueue = pending[0].size() + pending[1].size();
        return value;
    }
};
}
#endif
