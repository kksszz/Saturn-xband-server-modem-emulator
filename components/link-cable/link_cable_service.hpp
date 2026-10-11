// SPDX-License-Identifier: GPL-3.0-only
// Extracted from the project's YMIR link-cable implementation (2026-10-07).
// Host-independent YLS1 transport; no emulator core or UI dependency.
#pragma once

#include <cstdint>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace saturn::link {
using uint8 = std::uint8_t;
using uint16 = std::uint16_t;
using uint32 = std::uint32_t;
using uint64 = std::uint64_t;
using int64 = std::int64_t;

#ifndef SATURN_LINK_DIAGNOSTICS
#define SATURN_LINK_DIAGNOSTICS 0
#endif

/// @brief Connects a Saturn SH-2 SCI to another compatible instance over TCP.
///
/// Socket I/O runs on worker threads. The emulation thread only touches the
/// bounded byte queues. Synchronization mode explicitly waits for peer progress.
class LinkCableService {
public:
    // Opt-in for standalone regression/diagnostic builds only, never a user setting.
    static constexpr bool kDiagnostics = SATURN_LINK_DIAGNOSTICS != 0;
    enum class State { Disconnected, Listening, Connecting, Connected, Error };

    struct Diagnostics {
        uint64_t transmitted = 0;
        uint64_t sent = 0;
        uint64_t received = 0;
        uint64_t delivered = 0;
        size_t receiveQueue = 0;
        size_t peakReceiveQueue = 0;
        size_t peakSendQueue = 0;
        uint64_t longestSendWaitUs = 0;
        uint64_t longestReceiveWaitUs = 0;
        uint64_t largestReceiveBatch = 0;
        std::array<uint64_t, 256> transmittedBytes{};
        std::array<uint64_t, 256> receivedBytes{};
    };

    LinkCableService() = default;
    ~LinkCableService();

    LinkCableService(const LinkCableService &) = delete;
    LinkCableService &operator=(const LinkCableService &) = delete;

    void Listen(uint16_t port);
    void Connect(std::string host, uint16_t port);
    void AutoConnectLocal(uint16_t port);
    void Disconnect();
    void SetDiagnosticsLogPath(const std::filesystem::path &path);
    enum class CaptureState { Unavailable, Ready, Requested, Recording, Saving, Saved, Incomplete, Failed };
    void RequestLiveCapture();
    CaptureState GetCaptureState() const { return m_captureState.load(); }
    // Diagnostic routing choice, set once during setup before threads start.
    void SetMasterSCI(bool master) { m_masterSCI = master; }
    bool UsesMasterSCI() const { return m_masterSCI; }
    // Independent master/slave lanes. Set before starting network workers.
    void SetDualSCI(bool enabled) { m_dualSCI = enabled; }
    bool UsesDualSCI() const { return m_dualSCI; }
    bool LastActiveMasterSCI() const { return m_lastActiveMaster.load(); }
    void TransmitSCI(bool master, uint8 value, uint64 cycle);
    std::optional<uint8> ReceiveSCI(bool master, uint64 cycle);
    // Experimental wire protocol; select before starting workers.
    void SetSynchronized(bool enabled) { m_synchronized = enabled; }
    bool UsesSynchronization() const { return m_synchronized; }
    bool IsSyncWaiting() const { return m_syncWaiting.load(); }
    // Cumulative wall times, not CPU utilization. Run includes synchronization
    // and renderer/audio waits; pacing is outside RunFrame.
    struct Performance {
        uint64 runNs = 0, pacingNs = 0, syncWaitNs = 0, runCalls = 0, barriers = 0;
        uint64 audioWaitNs = 0, softwarePresentNs = 0, threadCpuNs = 0, cpuSamples = 0;
    };
    Performance GetPerformance() const {
        return {m_runNs.load(), m_pacingNs.load(), m_syncWaitNs.load(), m_runCalls.load(), m_barriers.load(),
                m_audioWaitNs.load(), m_softwarePresentNs.load(), m_threadCpuNs.load(), m_cpuSamples.load()};
    }
    void RecordRunTime(uint64 ns) { m_runNs.fetch_add(ns); m_runCalls.fetch_add(1); }
    void RecordPacingTime(uint64 ns) { m_pacingNs.fetch_add(ns); }
    void RecordAudioWaitTime(uint64 ns) { m_audioWaitNs.fetch_add(ns); }
    void RecordSoftwarePresentTime(uint64 ns) { m_softwarePresentNs.fetch_add(ns); }
    void RecordThreadCpuTime(uint64 ns) { m_threadCpuNs.fetch_add(ns); m_cpuSamples.fetch_add(1); }
    // Empty on unsupported platforms or OS query failure. Counts current thread only.
    static std::optional<uint64> CurrentThreadCpuTimeNs();
    void AbortSynchronization(const char *reason) {
        if (!m_synchronized || GetState() != State::Connected) return;
        m_stop.store(true);
        SyncError(reason);
    }
    static constexpr uint64 kSyncQuantum = 4096;
    uint64 AdvanceSynchronized(uint64 cycle, uint64 revision, uint32 clockTag,
                               const std::function<bool()> &cancelled);
    void TransmitTimed(uint8 value, uint64 cycle);
    std::optional<uint8> ReceiveTimed(uint64 cycle);
    void RecordLifecycleEvent(char event, uint32 value, uint64 sourceTimestampNs = 0);
    void RecordSCIEvent(char event, uint8 value, uint64 cycle, uint32 pc, uint8 scr, uint8 ssr);
    void RecordEmulatedFrame(uint8 scr, uint8 ssr, uint64_t sciReceived, uint64_t dmaReceived,
                             uint64_t dmaTransmitted, uint64_t rxFlagCleared);

    State GetState() const {
        return m_state.load(std::memory_order_acquire);
    }
    std::string GetError() const;
    std::vector<std::string> TakeNotifications();
    Diagnostics GetDiagnostics() const;

    void Transmit(uint8 value);
    std::optional<uint8> Receive();

private:
    enum class Mode { Listen, Connect, AutoLocal };

    static constexpr size_t kMaxQueuedBytes = 64 * 1024;
    struct QueuedByte {
        uint8 value;
        std::chrono::steady_clock::time_point queuedAt;
    };
    struct ByteTraceEvent {
        int64_t elapsedUs;
        uint64_t frame;
        char direction;
        uint8 value;
        uint64 cycle = 0;
        uint32 pc = 0;
        uint8 scr = 0;
        uint8 ssr = 0;
    };
    static constexpr size_t kByteTraceCapacity = 32 * 1024;

    void Start(Mode mode, std::string host, uint16_t port);
    void Run(Mode mode, std::string host, uint16_t port);
    void RunSession(Mode mode, std::string host, uint16_t port);
    void RunSynchronizedSession(uintptr_t socket);
    void SyncError(const char *message);
    using SyncMessage = std::array<uint8, 24>;
    void QueueSyncMessage(uint64 type, uint64 cycle, uint64 value); // m_syncMutex held
    bool ParseSyncMessage(const SyncMessage &message);
    void Fail(std::string message);
    void RecordTraceSample(std::chrono::steady_clock::time_point now);
    void RecordByteTraceLocked(char direction, uint8 value, std::chrono::steady_clock::time_point now);
    void DumpByteTraceIfIdle(std::chrono::steady_clock::time_point now);
    void UpdateLiveCapture(std::chrono::steady_clock::time_point now, bool finish = false);

    std::atomic<State> m_state{State::Disconnected};
    bool m_synchronized = false;
    bool m_dualSCI = false;
    std::atomic<bool> m_lastActiveMaster{false};
    std::atomic<uint64> m_runNs{0}, m_pacingNs{0}, m_syncWaitNs{0}, m_runCalls{0}, m_barriers{0};
    std::atomic<uint64> m_audioWaitNs{0}, m_softwarePresentNs{0}, m_threadCpuNs{0}, m_cpuSamples{0};
    Performance m_livePerformanceStart{};
    std::chrono::steady_clock::time_point m_livePerformanceAt{};
    std::atomic<bool> m_syncFault{false}, m_syncWaiting{false};
    std::mutex m_syncMutex;
    std::condition_variable m_syncCV;
    std::deque<SyncMessage> m_syncTX;
    struct TimedByte { uint64 cycle; uint8 value; };
    std::deque<TimedByte> m_syncRX;
    std::deque<TimedByte> m_masterRX;
    uint64 m_lastMasterByte = 0;
    uint64 m_syncGeneration = 0, m_syncPeerProgress = 0, m_syncLastPeerByte = 0;
    uint32 m_syncPeerClock = 0;
    bool m_syncHello = false;
    // Emulation thread owns these; also protected by m_syncMutex.
    uint64 m_syncBoundGeneration = 0, m_syncOrigin = 0, m_syncRevision = 0;
    uint64 m_syncBoundary = kSyncQuantum, m_syncPublished = 0;
    uint32 m_syncClock = 0;
    bool m_masterSCI = false;
    std::atomic<bool> m_stop{false};
    std::thread m_thread;

    mutable std::mutex m_errorMutex;
    std::string m_error;
    std::mutex m_notificationMutex;
    std::vector<std::string> m_notifications;

    std::mutex m_txMutex;
    std::condition_variable m_txCV;
    std::deque<QueuedByte> m_tx;
    std::mutex m_rxMutex;
    std::deque<QueuedByte> m_rx;
    std::atomic<size_t> m_rxCount{0};
    std::atomic<size_t> m_peakRxCount{0};
    std::atomic<size_t> m_peakTxCount{0};
    std::atomic<size_t> m_windowPeakRxCount{0};
    std::atomic<size_t> m_windowPeakTxCount{0};
    std::atomic<uint64_t> m_longestSendWaitUs{0};
    std::atomic<uint64_t> m_longestReceiveWaitUs{0};
    std::atomic<uint64_t> m_largestReceiveBatch{0};
    std::atomic<uint64_t> m_windowSendWaitUs{0};
    std::atomic<uint64_t> m_windowReceiveWaitUs{0};
    std::atomic<uint64_t> m_windowReceiveBatch{0};
    std::atomic<uint64_t> m_emulatedFrames{0};
    std::atomic<uint64_t> m_windowFrameGapUs{0};
    // Accessed only by the emulation thread.
    std::chrono::steady_clock::time_point m_previousFrameAt{};
    std::atomic<uint8> m_sciControl{0};
    std::atomic<uint8> m_sciStatus{0};
    std::atomic<uint64_t> m_sciReceived{0};
    std::atomic<uint64_t> m_dmaReceived{0};
    std::atomic<uint64_t> m_dmaTransmitted{0};
    std::atomic<uint64_t> m_rxFlagCleared{0};
    std::atomic<uint64_t> m_transmitted{0};
    std::atomic<uint64_t> m_sent{0};
    std::atomic<uint64_t> m_received{0};
    std::atomic<uint64_t> m_delivered{0};
    mutable std::mutex m_diagnosticsMutex;
    std::array<uint64_t, 256> m_transmittedBytes{};
    std::array<uint64_t, 256> m_receivedBytes{};
    std::ofstream m_diagnosticsLog;
    std::ofstream m_byteTraceLog;
    std::filesystem::path m_liveTracePath;
    std::atomic<CaptureState> m_captureState{CaptureState::Unavailable};
    struct LiveTraceEvent { uint64_t sequence; ByteTraceEvent event; };
    std::vector<LiveTraceEvent> m_liveEvents;
    uint64_t m_liveCursor = 0;
    uint64_t m_liveDropped = 0;
    int64_t m_liveCaptureId = 0;
    std::chrono::steady_clock::time_point m_liveDeadline{};
    std::chrono::steady_clock::time_point m_liveSampleAt{};
    std::ofstream m_initializationLog;
    std::vector<ByteTraceEvent> m_initializationEvents;
    uint64 m_initializationDropped = 0;
    struct LifecycleEvent {
        int64_t epochUs;
        uint64 frame;
        char event;
        uint32 value;
        uint64 sourceTimestampNs;
    };
    std::ofstream m_lifecycleLog;
    std::deque<LifecycleEvent> m_lifecycleEvents;
    uint64 m_lifecycleDropped = 0;
    bool m_byteTraceEnabled = false;
    std::vector<ByteTraceEvent> m_byteTrace;
    size_t m_byteTraceNext = 0;
    size_t m_byteTraceCount = 0;
    uint64_t m_byteTraceTotal = 0;
    uint64_t m_cableByteTraceTotal = 0;
    uint64_t m_lastDumpCableByteTotal = 0;
    std::chrono::steady_clock::time_point m_lastByteTraceAt{};
    std::chrono::steady_clock::time_point m_lastTcpReceiveAt{};
    int64_t m_traceEpochUs = 0;
    std::chrono::steady_clock::time_point m_traceStarted{};
    std::chrono::steady_clock::time_point m_previousTraceSample{};
    uint64_t m_previousFrames = 0;
    uint64_t m_previousTransmitted = 0;
    uint64_t m_previousSent = 0;
    uint64_t m_previousReceived = 0;
    uint64_t m_previousDelivered = 0;
};

} // namespace saturn::link
