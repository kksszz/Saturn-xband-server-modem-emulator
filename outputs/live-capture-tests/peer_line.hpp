#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

// Owner-thread API between modem routing and the peer transport.
// The implementation must outlive its modem ports. No method may block waiting
// for the other endpoint. Future IPC adapters must pump IO outside these calls.
// This is a C++ component boundary, not a network protocol or hardware model.
class PeerLine {
public:
    enum class State {Idle, Ringing, Connected};
    enum class Send {Delivered, Backpressure, NoCarrier};
    struct Snapshot {
        State state;
        uint64_t session;
        int caller;
        std::array<size_t,2> pendingReceive;
        std::array<uint64_t,2> sent,received;
    };
    virtual ~PeerLine() = default;
    // Passive: no clock advancement, queue draining or state transitions.
    virtual Snapshot snapshot() const = 0;
    virtual State state() const = 0;
    virtual uint64_t session() const = 0;
    virtual bool ringing(unsigned side) const = 0;
    virtual bool dial(unsigned side) = 0;
    virtual bool answer(unsigned side,uint64_t token) = 0;
    virtual bool hangup(unsigned side,uint64_t token) = 0;
    // Delivered means accepted once into the transport, not consumed by guest.
    // Backpressure/NoCarrier must not accept the byte. Callers may retry it.
    // Old session tokens must never inject bytes into a subsequent connection.
    virtual Send send(unsigned side,uint64_t token,uint8_t byte) = 0;
    virtual bool receive(unsigned side,uint64_t token,uint8_t &byte) = 0;
};
