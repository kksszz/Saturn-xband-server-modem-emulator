// SPDX-License-Identifier: GPL-3.0-only
// Extracted from the project's YMIR link-cable implementation (2026-10-07).
// Host-independent YLS1 transport; no emulator core or UI dependency.
// Included inside app::services, with socket helpers in scope.
// V1: ordered 24-byte records, three big-endian u64s: magic/type, cycle, value.
// Only byte-record payloads can reach guest SCI; no protocol header is forwarded.
namespace {
constexpr uint64 kSyncMagic = 0x594c533100000000ULL; // YLS1
constexpr uint64 kHello = 0, kByte = 1, kProgress = 2;
constexpr uint64 kMasterByte = 3;
uint64 ReadSyncWord(const uint8 *p) {
    uint64 value = 0;
    for (int i = 0; i < 8; ++i) value = (value << 8) | p[i];
    return value;
}
}

void LinkCableService::SyncError(const char *message) {
    m_syncFault.store(true);
    Fail(message);
    m_syncCV.notify_all();
}

void LinkCableService::QueueSyncMessage(uint64 type, uint64 cycle, uint64 value) {
    if (m_syncTX.size() >= 65536) {
        SyncError("Synchronized link send queue overflow");
        return;
    }
    SyncMessage packet{};
    const uint64 words[]{kSyncMagic | type, cycle, value};
    for (int w = 0; w < 3; ++w)
        for (int b = 0; b < 8; ++b) packet[w * 8 + b] = uint8(words[w] >> (56 - b * 8));
    m_syncTX.push_back(packet);
    m_syncCV.notify_all();
}

uint64 LinkCableService::AdvanceSynchronized(uint64 cycle, uint64 revision, uint32 clockTag,
                                             const std::function<bool()> &cancelled) {
    constexpr uint64 unlimited = ~uint64{0};
    if (!m_synchronized || GetState() != State::Connected || m_stop.load()) return unlimited;
    std::unique_lock lock{m_syncMutex};
    if (m_syncBoundGeneration != m_syncGeneration) {
        m_syncBoundGeneration = m_syncGeneration;
        m_syncOrigin = cycle;
        m_syncRevision = revision;
        m_syncClock = clockTag;
        m_syncBoundary = kSyncQuantum;
        m_syncPublished = 0;
    }
    if (revision != m_syncRevision || cycle < m_syncOrigin || clockTag != m_syncClock) {
        SyncError("Synchronized link timeline changed; reconnecting with a fresh session");
        return unlimited;
    }
    const uint64 now = cycle - m_syncOrigin;
    if (now < m_syncBoundary) return m_syncBoundary - now;
    if (now >= m_syncBoundary + kSyncQuantum) {
        SyncError("Synchronized link core exceeded its cycle budget");
        return unlimited;
    }
    if (m_syncPublished != m_syncBoundary) {
        QueueSyncMessage(kProgress, m_syncBoundary, clockTag);
        m_syncPublished = m_syncBoundary;
    }
    const uint64 generation = m_syncGeneration;
    if constexpr (kDiagnostics) ++m_barriers;
    // One clock pair per boundary, not per CPU slice or byte. Includes the
    // unlocked UI callback and scheduling delays while waiting for the peer.
    struct WaitTimer {
        std::atomic<uint64> &total;
        std::chrono::steady_clock::time_point start = kDiagnostics ? std::chrono::steady_clock::now()
                                                                 : std::chrono::steady_clock::time_point{};
        ~WaitTimer() {
            if constexpr (kDiagnostics) total.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - start).count(), std::memory_order_relaxed);
        }
    } waitTimer{m_syncWaitNs};
    while (m_syncPeerProgress < m_syncBoundary && !m_stop.load() && !m_syncFault.load() &&
           GetState() == State::Connected && generation == m_syncGeneration) {
        m_syncWaiting.store(true);
        lock.unlock();
        const bool cancel = cancelled();
        lock.lock();
        if (cancel) { m_syncWaiting.store(false); return 0; }
        // Progress can arrive while the UI callback runs with the mutex unlocked.
        // Recheck before sleeping so that notification is not lost.
        m_syncCV.wait_for(lock, std::chrono::milliseconds(1), [&] {
            return m_syncPeerProgress >= m_syncBoundary || m_stop.load() || m_syncFault.load() ||
                   GetState() != State::Connected || generation != m_syncGeneration;
        });
    }
    m_syncWaiting.store(false);
    if (m_stop.load() || m_syncFault.load() || GetState() != State::Connected || generation != m_syncGeneration)
        return unlimited;
    if (m_syncPeerClock != clockTag) {
        SyncError("Synchronized link clock modes differ; use matching video and CPU clock settings");
        return unlimited;
    }
    m_syncBoundary += kSyncQuantum;
    return m_syncBoundary - now;
}

void LinkCableService::TransmitTimed(uint8 value, uint64 cycle) {
    if (!m_synchronized) { Transmit(value); return; }
    std::lock_guard lock{m_syncMutex};
    if (GetState() != State::Connected || m_stop.load() || m_syncFault.load() ||
        m_syncBoundGeneration != m_syncGeneration || cycle < m_syncOrigin) return;
    QueueSyncMessage(kByte, cycle - m_syncOrigin, value);
    m_transmitted.fetch_add(1);
    std::lock_guard traceLock{m_diagnosticsMutex};
    ++m_transmittedBytes[value];
    const auto index = m_byteTraceNext;
    RecordByteTraceLocked('T', value, std::chrono::steady_clock::now());
    if (m_byteTraceEnabled) m_byteTrace[index].cycle = cycle;
}

void LinkCableService::TransmitSCI(bool master, uint8 value, uint64 cycle) {
    if (!m_dualSCI) { TransmitTimed(value, cycle); return; }
    std::lock_guard lock{m_syncMutex};
    if (GetState() != State::Connected || m_stop.load() || m_syncFault.load()) return;
    if (m_synchronized && (m_syncBoundGeneration != m_syncGeneration || cycle < m_syncOrigin)) return;
    QueueSyncMessage(master ? kMasterByte : kByte, m_synchronized ? cycle - m_syncOrigin : 0, value);
    if constexpr (kDiagnostics) m_lastActiveMaster.store(master);
    ++m_transmitted;
    if constexpr (kDiagnostics) {
    std::lock_guard traceLock{m_diagnosticsMutex};
    ++m_transmittedBytes[value];
    const auto index = m_byteTraceNext;
    RecordByteTraceLocked(master ? 't' : 'T', value, std::chrono::steady_clock::now());
    if (m_byteTraceEnabled) m_byteTrace[index].cycle = cycle;
    }
}

std::optional<uint8> LinkCableService::ReceiveSCI(bool master, uint64 cycle) {
    if (!m_dualSCI) return ReceiveTimed(cycle);
    std::lock_guard lock{m_syncMutex};
    auto &queue = master ? m_masterRX : m_syncRX;
    if (GetState() != State::Connected || m_stop.load() || m_syncFault.load() || queue.empty()) return {};
    if (m_synchronized && (m_syncBoundGeneration != m_syncGeneration || cycle < m_syncOrigin ||
        cycle - m_syncOrigin < queue.front().cycle + kSyncQuantum)) return {};
    const uint8 value = queue.front().value;
    queue.pop_front();
    m_rxCount.store(m_syncRX.size() + m_masterRX.size());
    if constexpr (kDiagnostics) m_lastActiveMaster.store(master);
    ++m_delivered;
    if constexpr (kDiagnostics) {
    std::lock_guard traceLock{m_diagnosticsMutex};
    const auto index = m_byteTraceNext;
    RecordByteTraceLocked(master ? 'r' : 'R', value, std::chrono::steady_clock::now());
    if (m_byteTraceEnabled) m_byteTrace[index].cycle = cycle;
    }
    return value;
}

std::optional<uint8> LinkCableService::ReceiveTimed(uint64 cycle) {
    if (!m_synchronized) return Receive();
    std::lock_guard lock{m_syncMutex};
    if (GetState() != State::Connected || m_stop.load() || m_syncFault.load() ||
        m_syncBoundGeneration != m_syncGeneration || cycle < m_syncOrigin || m_syncRX.empty()) return {};
    // Fixed one-quantum virtual cable latency, independent of wall-clock jitter.
    if (cycle - m_syncOrigin < m_syncRX.front().cycle + kSyncQuantum) return {};
    const uint8 value = m_syncRX.front().value;
    m_syncRX.pop_front();
    m_rxCount.store(m_syncRX.size());
    ++m_delivered;
    std::lock_guard traceLock{m_diagnosticsMutex};
    RecordByteTraceLocked('R', value, std::chrono::steady_clock::now());
    return value;
}

bool LinkCableService::ParseSyncMessage(const SyncMessage &message) {
    const uint64 header = ReadSyncWord(message.data());
    const uint64 cycle = ReadSyncWord(message.data() + 8);
    const uint64 value = ReadSyncWord(message.data() + 16);
    const uint64 type = header & 0xffffffff;
    if ((header & 0xffffffff00000000ULL) != kSyncMagic) {
        SyncError("Incompatible link peer: both instances must use the synchronized preview"); return false;
    }
    std::lock_guard lock{m_syncMutex};
    if (!m_syncHello) {
        const uint64 mode = m_dualSCI ? (m_synchronized ? 3 : 2) : uint64(m_masterSCI);
        if (type != kHello || cycle != kSyncQuantum || value != mode) {
            SyncError("Synchronized link handshake/version/SCI routing mismatch"); return false;
        }
        m_syncHello = true;
        m_state.store(State::Connected, std::memory_order_release);
    } else if (type == kByte || (m_dualSCI && type == kMasterByte)) {
        auto &queue = type == kMasterByte ? m_masterRX : m_syncRX;
        auto &lastByte = type == kMasterByte ? m_lastMasterByte : m_syncLastPeerByte;
        if (value > 255 || (m_synchronized && (cycle < lastByte || cycle < m_syncPeerProgress ||
            cycle > m_syncPeerProgress + 2 * kSyncQuantum)) ||
            (!m_synchronized && cycle != 0) || m_syncRX.size() + m_masterRX.size() >= kMaxQueuedBytes) {
            SyncError("Invalid synchronized link byte order/range or receive queue overflow"); return false;
        }
        lastByte = cycle;
        queue.push_back({cycle, uint8(value)});
        m_rxCount.store(m_syncRX.size() + m_masterRX.size());
        RecordMaximum(m_peakRxCount, m_syncRX.size() + m_masterRX.size());
        ++m_received;
        if constexpr (kDiagnostics) {
        std::lock_guard traceLock{m_diagnosticsMutex};
        ++m_receivedBytes[value];
        }
    } else if (type == kProgress && m_synchronized) {
        if (cycle != m_syncPeerProgress + kSyncQuantum || value > UINT32_MAX) {
            SyncError("Invalid synchronized link progress sequence"); return false;
        }
        m_syncPeerProgress = cycle;
        m_syncPeerClock = uint32(value);
    } else {
        SyncError("Invalid synchronized link message type"); return false;
    }
    m_syncCV.notify_all();
    return true;
}

void LinkCableService::RunSynchronizedSession(uintptr_t handle) {
    const Socket peer = static_cast<Socket>(handle);
    m_state.store(State::Connecting);
    {
        std::lock_guard lock{m_syncMutex};
        m_syncTX.clear(); m_syncRX.clear(); m_masterRX.clear(); m_rxCount.store(0);
        m_lastMasterByte = 0;
        m_syncPeerProgress = m_syncLastPeerByte = 0;
        m_syncPeerClock = 0;
        m_syncHello = false;
        ++m_syncGeneration;
        m_syncFault.store(false);
        QueueSyncMessage(kHello, kSyncQuantum, m_dualSCI ? (m_synchronized ? 3 : 2) : uint64(m_masterSCI));
    }
    std::atomic<bool> done{false};
    const auto stopped = [&] { return done.load() || m_stop.load() || m_syncFault.load(); };
    std::thread sender{[&] {
        while (!stopped()) {
            std::vector<SyncMessage> packets;
            {
                std::unique_lock lock{m_syncMutex};
                m_syncCV.wait(lock, [&] { return stopped() || !m_syncTX.empty(); });
                if (stopped()) break;
                while (!m_syncTX.empty() && packets.size() < 64) {
                    packets.push_back(m_syncTX.front()); m_syncTX.pop_front();
                }
            }
            const char *data = reinterpret_cast<const char *>(packets.data());
            const size_t size = packets.size() * sizeof(SyncMessage);
            size_t offset = 0;
            while (offset < size && !stopped()) {
                int flags = 0;
#ifdef MSG_NOSIGNAL
                flags = MSG_NOSIGNAL;
#endif
                const int count = send(peer, data + offset, static_cast<int>(size - offset), flags);
                if (count < 0 && IsInProgress()) {
                    if (WaitSocket(peer, false, true) < 0) SyncError("Synchronized link send wait failed");
                    continue;
                }
                if (count <= 0) { SyncError("Synchronized link send failed"); break; }
                offset += count;
            }
            if (offset == size) for (const auto &packet : packets)
                if (ReadSyncWord(packet.data()) == (kSyncMagic | kByte) ||
                    ReadSyncWord(packet.data()) == (kSyncMagic | kMasterByte)) ++m_sent;
        }
    }};
    SyncMessage input{};
    std::array<uint8, sizeof(SyncMessage) * 64> received{};
    size_t used = 0;
    const auto handshakeDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!stopped()) {
        const int ready = WaitSocket(peer, true, false);
        if (ready < 0) { SyncError("Synchronized link socket error"); break; }
        if (ready > 0) {
            // Drain a batch rather than issuing select/recv for every 24-byte
            // record. Preserve stream order and incomplete records across reads.
            const int count = recv(peer, reinterpret_cast<char *>(received.data()), int(received.size()), 0);
            if (count < 0 && IsInProgress()) continue;
            if (count <= 0) { SyncError("Synchronized link peer disconnected"); break; }
            size_t offset = 0;
            bool valid = true;
            while (offset < size_t(count)) {
                const size_t chunk = std::min(input.size() - used, size_t(count) - offset);
                std::copy_n(received.data() + offset, chunk, input.data() + used);
                used += chunk;
                offset += chunk;
                if (used == input.size()) {
                    if (!ParseSyncMessage(input)) { valid = false; break; }
                    used = 0;
                }
            }
            if (!valid) break;
        }
        if (GetState() != State::Connected && std::chrono::steady_clock::now() > handshakeDeadline) {
            SyncError("Synchronized link handshake timed out (legacy peers are not supported)"); break;
        }
        // No continuous diagnostic file I/O in the synchronized receive path.
        if constexpr (kDiagnostics) UpdateLiveCapture(std::chrono::steady_clock::now());
    }
    done.store(true);
    m_syncCV.notify_all();
    sender.join();
    if (GetState() == State::Connected) m_state.store(State::Disconnected);
    m_syncCV.notify_all();
    if constexpr (kDiagnostics) UpdateLiveCapture(std::chrono::steady_clock::now(), true);
}
