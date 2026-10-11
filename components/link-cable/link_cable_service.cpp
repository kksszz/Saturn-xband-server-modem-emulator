// SPDX-License-Identifier: GPL-3.0-only
// Extracted from the project's YMIR link-cable implementation (2026-10-07).
// Host-independent YLS1 transport; no emulator core or UI dependency.
#include "link_cable_service.hpp"

#ifdef _WIN32
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <winsock2.h>
    #include <ws2tcpip.h>
#else
    #include <arpa/inet.h>
    #include <cerrno>
    #include <fcntl.h>
    #include <netdb.h>
    #include <netinet/tcp.h>
    #include <sys/select.h>
    #include <sys/socket.h>
    #include <unistd.h>
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <string>

namespace saturn::link {

std::optional<uint64> LinkCableService::CurrentThreadCpuTimeNs() {
#ifdef _WIN32
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) return {};
    const auto ticks = [](FILETIME value) {
        return (uint64(value.dwHighDateTime) << 32) | value.dwLowDateTime;
    };
    return (ticks(kernel) + ticks(user)) * 100;
#else
    return {};
#endif
}

namespace {

    template <typename T>
    void RecordMaximum(std::atomic<T> &maximum, T value) {
        T previous = maximum.load(std::memory_order_relaxed);
        while (previous < value &&
               !maximum.compare_exchange_weak(previous, value, std::memory_order_relaxed)) {
        }
    }

#ifdef _WIN32
    using Socket = SOCKET;
    constexpr Socket kInvalidSocket = INVALID_SOCKET;
    void CloseSocket(Socket socket) {
        if (socket != kInvalidSocket) {
            closesocket(socket);
        }
    }
    bool SetNonBlocking(Socket socket) {
        u_long enabled = 1;
        return ioctlsocket(socket, FIONBIO, &enabled) == 0;
    }
    bool IsInProgress() {
        const int error = WSAGetLastError();
        return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS;
    }
#else
    using Socket = int;
    constexpr Socket kInvalidSocket = -1;
    void CloseSocket(Socket socket) {
        if (socket != kInvalidSocket) {
            close(socket);
        }
    }
    bool SetNonBlocking(Socket socket) {
        const int flags = fcntl(socket, F_GETFL, 0);
        return flags >= 0 && fcntl(socket, F_SETFL, flags | O_NONBLOCK) == 0;
    }
    bool IsInProgress() {
        return errno == EWOULDBLOCK || errno == EINPROGRESS;
    }
#endif

    int WaitSocket(Socket socket, bool read, bool write) {
        fd_set readSet;
        fd_set writeSet;
        FD_ZERO(&readSet);
        FD_ZERO(&writeSet);
        if (read) {
            FD_SET(socket, &readSet);
        }
        if (write) {
            FD_SET(socket, &writeSet);
        }
        timeval timeout{0, 50'000};
        return select(static_cast<int>(socket + 1), read ? &readSet : nullptr, write ? &writeSet : nullptr, nullptr,
                      &timeout);
    }

} // namespace

LinkCableService::~LinkCableService() {
    Disconnect();
}

void LinkCableService::SetDiagnosticsLogPath(const std::filesystem::path &path) {
    if constexpr (!kDiagnostics) return;
    // Called during app setup, before the network worker starts. This is a
    // temporary local diagnostic trace containing cable bytes and frame timestamps.
    m_diagnosticsLog.open(path, std::ios::app);
    if (m_diagnosticsLog) {
        m_diagnosticsLog << "epoch_ms,elapsed_ms,frames,tx,send,recv,dequeue,tx_queued,rx_queued,tx_peak,rx_peak,"
                            "tx_wait_us,rx_wait_us,rx_batch,scr,ssr,sci_received,dma_rx,dma_tx,cpu_rx_clear,frame_gap_us\n";
        m_diagnosticsLog.flush();
        m_traceStarted = std::chrono::steady_clock::now();
        m_traceEpochUs = std::chrono::duration_cast<std::chrono::microseconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();
        m_byteTrace.resize(kByteTraceCapacity);
        m_initializationLog.open(path.parent_path() / "link-cable-init-registers.csv", std::ios::app);
        if (m_initializationLog) {
            m_initializationLog << "epoch_us,frame,event,value,cycle,pc,scr,ssr,dropped_total\n";
            m_initializationLog.flush();
        }
        m_lifecycleLog.open(path.parent_path() /
            (m_masterSCI ? "link-cable-doom-events.csv" : "link-cable-focus-events.csv"), std::ios::app);
        if (m_lifecycleLog) {
            m_lifecycleLog << "epoch_us,frame,event,value,source_timestamp_ns,dropped_total\n";
            m_lifecycleLog.flush();
        }
        m_byteTraceLog.open(path.parent_path() /
            (m_masterSCI ? "link-cable-doom-guest-trace.csv" : "link-cable-focus-guest-trace.csv"), std::ios::app);
        if (m_byteTraceLog) {
            m_byteTraceEnabled = true;
            m_liveTracePath = path.parent_path() / "link-cable-live-trace.csv";
            m_captureState.store(CaptureState::Ready);
            m_byteTraceLog << "dump,epoch_us,frame,direction,value,cycle,pc,scr,ssr\n";
            m_byteTraceLog.flush();
        }
    }
}

void LinkCableService::RecordLifecycleEvent(char event, uint32 value, uint64 sourceTimestampNs) {
    if constexpr (!kDiagnostics) return;
    if (!m_byteTraceEnabled) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - m_traceStarted).count();
    std::lock_guard lock{m_diagnosticsMutex};
    if (m_lifecycleEvents.size() == 256) {
        m_lifecycleEvents.pop_front();
        ++m_lifecycleDropped;
    }
    m_lifecycleEvents.push_back({m_traceEpochUs + elapsed, m_emulatedFrames.load(std::memory_order_relaxed),
                                 event, value, sourceTimestampNs});
}

void LinkCableService::RecordSCIEvent(char event, uint8 value, uint64 cycle, uint32 pc, uint8 scr, uint8 ssr) {
    if constexpr (!kDiagnostics) return;
    if (!m_byteTraceEnabled) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard lock{m_diagnosticsMutex};
    const size_t index = m_byteTraceNext;
    RecordByteTraceLocked(event, value, now);
    auto &entry = m_byteTrace[index];
    entry.cycle = cycle;
    entry.pc = pc;
    entry.scr = scr;
    entry.ssr = ssr;
    // Preserve register writes even if no cable bytes are ever sent. Bounded
    // storage, drained by the worker; no file I/O on the emulation thread.
    if ((event >= '0' && event <= '4') || event == 'v' || event == 'w') {
        if (m_initializationEvents.size() < 32768) {
            m_initializationEvents.push_back(entry);
        } else {
            ++m_initializationDropped;
        }
    }
}

void LinkCableService::RecordEmulatedFrame(uint8 scr, uint8 ssr, uint64_t sciReceived, uint64_t dmaReceived,
                                          uint64_t dmaTransmitted, uint64_t rxFlagCleared) {
    if constexpr (!kDiagnostics) return;
    const auto now = std::chrono::steady_clock::now();
    m_emulatedFrames.fetch_add(1, std::memory_order_relaxed);
    m_sciControl.store(scr, std::memory_order_relaxed);
    m_sciStatus.store(ssr, std::memory_order_relaxed);
    m_sciReceived.store(sciReceived, std::memory_order_relaxed);
    m_dmaReceived.store(dmaReceived, std::memory_order_relaxed);
    m_dmaTransmitted.store(dmaTransmitted, std::memory_order_relaxed);
    m_rxFlagCleared.store(rxFlagCleared, std::memory_order_relaxed);
    if (!m_byteTraceEnabled) {
        return;
    }
    if (m_previousFrameAt != std::chrono::steady_clock::time_point{}) {
        const auto gap = std::chrono::duration_cast<std::chrono::microseconds>(now - m_previousFrameAt).count();
        RecordMaximum(m_windowFrameGapUs, static_cast<uint64_t>(gap));
    }
    m_previousFrameAt = now;
    std::lock_guard lock{m_diagnosticsMutex};
    RecordByteTraceLocked('F', 0, now);
}

void LinkCableService::RecordByteTraceLocked(char direction, uint8 value,
                                             std::chrono::steady_clock::time_point now) {
    if constexpr (!kDiagnostics) return;
    if (!m_byteTraceEnabled) {
        return;
    }
    const auto elapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(now - m_traceStarted).count();
    m_byteTrace[m_byteTraceNext] = {elapsedUs, m_emulatedFrames.load(std::memory_order_relaxed), direction, value};
    m_byteTraceNext = (m_byteTraceNext + 1) % kByteTraceCapacity;
    m_byteTraceCount = std::min(m_byteTraceCount + 1, kByteTraceCapacity);
    ++m_byteTraceTotal;
    // Frames continue after a cable error. They must neither prevent an idle
    // dump nor rearm it without new cable traffic.
    if (direction == 'T' || direction == 'R' || direction == 't' || direction == 'r') {
        ++m_cableByteTraceTotal;
        m_lastByteTraceAt = now;
    }
}

void LinkCableService::DumpByteTraceIfIdle(std::chrono::steady_clock::time_point now) {
    if (!m_byteTraceLog) {
        return;
    }
    std::vector<ByteTraceEvent> events;
    uint64_t dump = 0;
    {
        std::lock_guard lock{m_diagnosticsMutex};
        if (m_cableByteTraceTotal - m_lastDumpCableByteTotal < 512 ||
            now - m_lastByteTraceAt < std::chrono::milliseconds(400) ||
            now - m_lastTcpReceiveAt < std::chrono::milliseconds(400)) {
            return;
        }
        const size_t first = (m_byteTraceNext + kByteTraceCapacity - m_byteTraceCount) % kByteTraceCapacity;
        events.reserve(m_byteTraceCount);
        for (size_t i = 0; i < m_byteTraceCount; ++i) {
            events.push_back(m_byteTrace[(first + i) % kByteTraceCapacity]);
        }
        m_lastDumpCableByteTotal = m_cableByteTraceTotal;
        dump = m_byteTraceTotal;
    }
    for (const auto &event : events) {
        m_byteTraceLog << dump << ',' << m_traceEpochUs + event.elapsedUs << ',' << event.frame << ','
                       << event.direction << ',' << static_cast<unsigned>(event.value) << ','
                       << event.cycle << ',' << event.pc << ',' << static_cast<unsigned>(event.scr) << ','
                       << static_cast<unsigned>(event.ssr) << '\n';
    }
    m_byteTraceLog.flush();
}

void LinkCableService::RequestLiveCapture() {
    if constexpr (!kDiagnostics) return;
    if (GetState() != State::Connected) return;
    auto state = m_captureState.load();
    while (state == CaptureState::Ready || state == CaptureState::Saved ||
           state == CaptureState::Incomplete || state == CaptureState::Failed) {
        if (m_captureState.compare_exchange_weak(state, CaptureState::Requested)) return;
    }
}

void LinkCableService::UpdateLiveCapture(std::chrono::steady_clock::time_point now, bool finish) {
    if constexpr (!kDiagnostics) return;
    if (m_captureState.load() == CaptureState::Requested) {
        if (finish) {
            m_captureState.store(CaptureState::Incomplete);
            return;
        }
        m_liveEvents.clear();
        m_livePerformanceStart = GetPerformance();
        m_livePerformanceAt = now;
        m_liveDropped = 0;
        m_liveDeadline = now + std::chrono::seconds(10);
        m_liveSampleAt = now;
        m_liveCaptureId = m_traceEpochUs +
            std::chrono::duration_cast<std::chrono::microseconds>(now - m_traceStarted).count();
        {
            std::lock_guard lock{m_diagnosticsMutex};
            m_liveCursor = m_byteTraceTotal;
        }
        m_captureState.store(CaptureState::Recording);
    }
    if (m_captureState.load() != CaptureState::Recording) return;
    if (!finish && now - m_liveSampleAt < std::chrono::milliseconds(100)) return;
    m_liveSampleAt = now;
    // Bounded memory; file output happens only after the capture window.
    constexpr size_t capacity = 1024 * 1024;
    std::vector<LiveTraceEvent> batch;
    batch.reserve(kByteTraceCapacity);
    const auto endUs = std::chrono::duration_cast<std::chrono::microseconds>(m_liveDeadline - m_traceStarted).count();
    {
        std::lock_guard lock{m_diagnosticsMutex};
        const uint64_t oldest = m_byteTraceTotal - m_byteTraceCount;
        if (m_liveCursor < oldest) {
            m_liveDropped += oldest - m_liveCursor;
            m_liveCursor = oldest;
        }
        for (; m_liveCursor < m_byteTraceTotal; ++m_liveCursor) {
            const auto &event = m_byteTrace[m_liveCursor % kByteTraceCapacity];
            if (event.elapsedUs <= endUs) batch.push_back({m_liveCursor, event});
        }
    }
    for (const auto &entry : batch) {
        if (m_liveEvents.size() < capacity) m_liveEvents.push_back(entry);
        else ++m_liveDropped;
    }
    if (!finish && now < m_liveDeadline) return;
    m_captureState.store(CaptureState::Saving);
    // Snapshot before formatting/writing the trace, excluding save-time I/O.
    const auto performance = GetPerformance();
    const auto performanceWindowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
        now - m_livePerformanceAt).count();
    std::ofstream log(m_liveTracePath, std::ios::app);
    log << "capture,sequence,epoch_us,frame,event,value,cycle,pc,scr,ssr,dropped_total,incomplete\n";
    const bool incomplete = finish || m_liveDropped != 0;
    for (const auto &entry : m_liveEvents) {
        const auto &event = entry.event;
        log << m_liveCaptureId << ',' << entry.sequence << ',' << m_traceEpochUs + event.elapsedUs << ','
            << event.frame << ',' << event.direction << ',' << static_cast<unsigned>(event.value) << ','
            << event.cycle << ',' << event.pc << ',' << static_cast<unsigned>(event.scr) << ','
            << static_cast<unsigned>(event.ssr) << ',' << m_liveDropped << ',' << incomplete << '\n';
    }
    // Footer also records empty captures and truncation explicitly.
    log << "# end," << m_liveCaptureId << ",events," << m_liveEvents.size()
        << ",dropped," << m_liveDropped << ",incomplete," << incomplete << '\n';
    if (m_synchronized) {
        log << "# sync_perf_v1," << m_liveCaptureId << ",window_ns," << performanceWindowNs
            << ",run_ns," << performance.runNs - m_livePerformanceStart.runNs
            << ",pacing_ns," << performance.pacingNs - m_livePerformanceStart.pacingNs
            << ",sync_wait_ns," << performance.syncWaitNs - m_livePerformanceStart.syncWaitNs
            << ",run_calls," << performance.runCalls - m_livePerformanceStart.runCalls
            << ",barriers," << performance.barriers - m_livePerformanceStart.barriers << '\n';
        log << "# sync_detail_v1," << m_liveCaptureId
            << ",audio_wait_ns," << performance.audioWaitNs - m_livePerformanceStart.audioWaitNs
            << ",software_present_ns," << performance.softwarePresentNs - m_livePerformanceStart.softwarePresentNs
            << ",thread_cpu_ns," << performance.threadCpuNs - m_livePerformanceStart.threadCpuNs
            << ",cpu_samples," << performance.cpuSamples - m_livePerformanceStart.cpuSamples << '\n';
    }
    log.flush();
    const bool saved = static_cast<bool>(log);
    m_liveEvents.clear();
    m_captureState.store(!saved ? CaptureState::Failed : incomplete ? CaptureState::Incomplete : CaptureState::Saved);
}

void LinkCableService::RecordTraceSample(std::chrono::steady_clock::time_point now) {
    UpdateLiveCapture(now);
    if (!m_diagnosticsLog || now - m_previousTraceSample < std::chrono::milliseconds(100)) {
        return;
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_traceStarted).count();
    // Sparse UI/pause events have their own queue so ongoing byte traffic cannot
    // evict them. Only the network worker writes files, outside the shared lock.
    std::deque<LifecycleEvent> lifecycle;
    std::vector<ByteTraceEvent> initialization;
    uint64 initializationDropped;
    uint64 dropped;
    {
        std::lock_guard lock{m_diagnosticsMutex};
        lifecycle.swap(m_lifecycleEvents);
        initialization.swap(m_initializationEvents);
        initializationDropped = m_initializationDropped;
        dropped = m_lifecycleDropped;
    }
    if (m_initializationLog && !initialization.empty()) {
        for (const auto &event : initialization) {
            m_initializationLog << m_traceEpochUs + event.elapsedUs << ',' << event.frame << ','
                                << event.direction << ',' << static_cast<unsigned>(event.value) << ','
                                << event.cycle << ',' << event.pc << ',' << static_cast<unsigned>(event.scr) << ','
                                << static_cast<unsigned>(event.ssr) << ',' << initializationDropped << '\n';
        }
        m_initializationLog.flush();
    }
    if (m_lifecycleLog && !lifecycle.empty()) {
        for (const auto &event : lifecycle) {
            m_lifecycleLog << event.epochUs << ',' << event.frame << ',' << event.event << ',' << event.value
                           << ',' << event.sourceTimestampNs << ',' << dropped << '\n';
        }
        m_lifecycleLog.flush();
    }
    const auto epoch = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
    const uint64_t frames = m_emulatedFrames.load(std::memory_order_relaxed);
    const uint64_t transmitted = m_transmitted.load(std::memory_order_relaxed);
    const uint64_t sent = m_sent.load(std::memory_order_relaxed);
    const uint64_t received = m_received.load(std::memory_order_relaxed);
    const uint64_t delivered = m_delivered.load(std::memory_order_relaxed);
    size_t txQueued = 0;
    {
        std::lock_guard lock{m_txMutex};
        txQueued = m_tx.size();
    }
    m_diagnosticsLog << epoch << ',' << elapsed << ',' << frames - m_previousFrames << ','
                     << transmitted - m_previousTransmitted << ',' << sent - m_previousSent << ','
                     << received - m_previousReceived << ',' << delivered - m_previousDelivered << ','
                     << txQueued << ',' << m_rxCount.load(std::memory_order_relaxed) << ','
                     << m_windowPeakTxCount.exchange(0, std::memory_order_relaxed) << ','
                     << m_windowPeakRxCount.exchange(0, std::memory_order_relaxed) << ','
                     << m_windowSendWaitUs.exchange(0, std::memory_order_relaxed) << ','
                     << m_windowReceiveWaitUs.exchange(0, std::memory_order_relaxed) << ','
                     << m_windowReceiveBatch.exchange(0, std::memory_order_relaxed) << ','
                     << static_cast<unsigned>(m_sciControl.load(std::memory_order_relaxed)) << ','
                     << static_cast<unsigned>(m_sciStatus.load(std::memory_order_relaxed)) << ','
                     << m_sciReceived.load(std::memory_order_relaxed) << ','
                     << m_dmaReceived.load(std::memory_order_relaxed) << ','
                     << m_dmaTransmitted.load(std::memory_order_relaxed) << ','
                     << m_rxFlagCleared.load(std::memory_order_relaxed) << ','
                     << m_windowFrameGapUs.exchange(0, std::memory_order_relaxed) << '\n';
    m_diagnosticsLog.flush();
    DumpByteTraceIfIdle(now);
    m_previousTraceSample = now;
    m_previousFrames = frames;
    m_previousTransmitted = transmitted;
    m_previousSent = sent;
    m_previousReceived = received;
    m_previousDelivered = delivered;
}

std::string LinkCableService::GetError() const {
    std::lock_guard lock{m_errorMutex};
    return m_error;
}

std::vector<std::string> LinkCableService::TakeNotifications() {
    std::lock_guard lock{m_notificationMutex};
    std::vector<std::string> notifications;
    notifications.swap(m_notifications);
    return notifications;
}

LinkCableService::Diagnostics LinkCableService::GetDiagnostics() const {
    Diagnostics diagnostics{
        m_transmitted.load(std::memory_order_relaxed), m_sent.load(std::memory_order_relaxed),
        m_received.load(std::memory_order_relaxed), m_delivered.load(std::memory_order_relaxed),
        m_rxCount.load(std::memory_order_relaxed),
    };
    diagnostics.peakReceiveQueue = m_peakRxCount.load(std::memory_order_relaxed);
    diagnostics.peakSendQueue = m_peakTxCount.load(std::memory_order_relaxed);
    diagnostics.longestSendWaitUs = m_longestSendWaitUs.load(std::memory_order_relaxed);
    diagnostics.longestReceiveWaitUs = m_longestReceiveWaitUs.load(std::memory_order_relaxed);
    diagnostics.largestReceiveBatch = m_largestReceiveBatch.load(std::memory_order_relaxed);
    {
        std::lock_guard lock{m_diagnosticsMutex};
        diagnostics.transmittedBytes = m_transmittedBytes;
        diagnostics.receivedBytes = m_receivedBytes;
    }
    return diagnostics;
}

void LinkCableService::Listen(uint16_t port) {
    Start(Mode::Listen, {}, port);
}

void LinkCableService::Connect(std::string host, uint16_t port) {
    Start(Mode::Connect, std::move(host), port);
}

void LinkCableService::AutoConnectLocal(uint16_t port) {
    Start(Mode::AutoLocal, "127.0.0.1", port);
}

void LinkCableService::Start(Mode mode, std::string host, uint16_t port) {
    Disconnect();
    {
        std::lock_guard lock{m_errorMutex};
        m_error.clear();
    }
    m_stop.store(false, std::memory_order_release);
    m_state.store(mode == Mode::Listen ? State::Listening : State::Connecting, std::memory_order_release);
    m_thread = std::thread{[this, mode, host = std::move(host), port] { Run(mode, host, port); }};
}

void LinkCableService::Disconnect() {
    m_stop.store(true, std::memory_order_release);
    m_txCV.notify_all();
    m_syncCV.notify_all();
    if (m_thread.joinable()) {
        m_thread.join();
    }
    {
        std::lock_guard lock{m_txMutex};
        m_tx.clear();
    }
    {
        std::lock_guard lock{m_rxMutex};
        m_rx.clear();
        m_rxCount.store(0, std::memory_order_release);
    }
    m_state.store(State::Disconnected, std::memory_order_release);
}

void LinkCableService::Fail(std::string message) {
    {
        std::lock_guard lock{m_errorMutex};
        m_error = message;
    }
    const State previous = m_state.exchange(State::Error, std::memory_order_acq_rel);
    if (previous == State::Connected && !m_stop.load(std::memory_order_acquire)) {
        std::lock_guard lock{m_notificationMutex};
        m_notifications.push_back("Battle (Taisen) Cable disconnected: " + message);
    }
}

void LinkCableService::Transmit(uint8 value) {
    if (GetState() != State::Connected) {
        return;
    }
    m_transmitted.fetch_add(1, std::memory_order_relaxed);
    {
        std::lock_guard lock{m_diagnosticsMutex};
        ++m_transmittedBytes[value];
        RecordByteTraceLocked('T', value, std::chrono::steady_clock::now());
    }
    bool overflow = false;
    {
        std::lock_guard lock{m_txMutex};
        if (m_tx.size() < kMaxQueuedBytes) {
            m_tx.push_back({value, std::chrono::steady_clock::now()});
            RecordMaximum(m_peakTxCount, m_tx.size());
            RecordMaximum(m_windowPeakTxCount, m_tx.size());
        } else {
            overflow = true;
        }
    }
    if (overflow) {
        Fail("Link-cable send queue overflow");
        m_stop.store(true, std::memory_order_release);
    } else {
        m_txCV.notify_one();
    }
}

std::optional<uint8> LinkCableService::Receive() {
    if (m_rxCount.load(std::memory_order_acquire) == 0) {
        return std::nullopt;
    }
    std::lock_guard lock{m_rxMutex};
    if (m_rx.empty()) {
        return std::nullopt;
    }
    const QueuedByte queued = m_rx.front();
    m_rx.pop_front();
    m_rxCount.store(m_rx.size(), std::memory_order_release);
    m_delivered.fetch_add(1, std::memory_order_relaxed);
    const auto receivedAt = std::chrono::steady_clock::now();
    const auto wait = std::chrono::duration_cast<std::chrono::microseconds>(receivedAt - queued.queuedAt);
    RecordMaximum(m_longestReceiveWaitUs, static_cast<uint64_t>(wait.count()));
    RecordMaximum(m_windowReceiveWaitUs, static_cast<uint64_t>(wait.count()));
    {
        std::lock_guard diagnosticsLock{m_diagnosticsMutex};
        RecordByteTraceLocked('R', queued.value, receivedAt);
    }
    return queued.value;
}

void LinkCableService::Run(Mode mode, std::string host, uint16_t port) {
    if (mode != Mode::AutoLocal) {
        RunSession(mode, std::move(host), port);
        return;
    }

    // The checkbox owns the lifetime of the virtual cable. A peer disconnect
    // drops the current TCP session but leaves the local cable enabled, so an
    // auto-paired instance keeps waiting for its partner to return.
    while (!m_stop.load(std::memory_order_acquire)) {
        {
            std::lock_guard lock{m_txMutex};
            m_tx.clear();
        }
        {
            std::lock_guard lock{m_rxMutex};
            m_rx.clear();
            m_rxCount.store(0, std::memory_order_release);
        }
        m_state.store(State::Connecting, std::memory_order_release);
        RunSession(mode, host, port);
        if (!m_stop.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    }
}

void LinkCableService::RunSession(Mode mode, std::string host, uint16_t port) {
#ifdef _WIN32
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        Fail("Could not initialize Winsock");
        return;
    }
#endif

    Socket listener = kInvalidSocket;
    Socket peer = kInvalidSocket;
    auto cleanup = [&] {
        CloseSocket(peer);
        CloseSocket(listener);
#ifdef _WIN32
        WSACleanup();
#endif
    };

    if (mode != Mode::Connect) {
        listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener == kInvalidSocket || !SetNonBlocking(listener)) {
            Fail("Could not create link-cable listener");
            cleanup();
            return;
        }
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(mode == Mode::AutoLocal ? INADDR_LOOPBACK : INADDR_ANY);
        address.sin_port = htons(port);
        if (bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0 || listen(listener, 1) != 0) {
            if (mode == Mode::AutoLocal) {
                CloseSocket(listener);
                listener = kInvalidSocket;
                mode = Mode::Connect;
            } else {
                Fail("Could not bind link-cable port (already in use or blocked)");
                cleanup();
                return;
            }
        }
        if (listener != kInvalidSocket) {
            m_state.store(State::Listening, std::memory_order_release);
            while (!m_stop.load(std::memory_order_acquire)) {
                const int ready = WaitSocket(listener, true, false);
                if (ready < 0) {
                    Fail("Link-cable listener failed");
                    cleanup();
                    return;
                }
                if (ready > 0) {
                    peer = accept(listener, nullptr, nullptr);
                    if (peer != kInvalidSocket) {
                        break;
                    }
                }
            }
            CloseSocket(listener);
            listener = kInvalidSocket;
        }
    }
    if (mode == Mode::Connect) {
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;
        addrinfo *addresses = nullptr;
        const std::string service = std::to_string(port);
        if (getaddrinfo(host.c_str(), service.c_str(), &hints, &addresses) != 0 || addresses == nullptr) {
            Fail("Could not resolve link-cable host");
            cleanup();
            return;
        }
        peer = socket(addresses->ai_family, addresses->ai_socktype, addresses->ai_protocol);
        if (peer == kInvalidSocket || !SetNonBlocking(peer)) {
            freeaddrinfo(addresses);
            Fail("Could not create link-cable socket");
            cleanup();
            return;
        }
        const int result = connect(peer, addresses->ai_addr, static_cast<int>(addresses->ai_addrlen));
        const bool pending = result != 0 && IsInProgress();
        freeaddrinfo(addresses);
        if (result != 0 && !pending) {
            Fail("Could not connect to link-cable host");
            cleanup();
            return;
        }
        while (pending && !m_stop.load(std::memory_order_acquire)) {
            const int ready = WaitSocket(peer, false, true);
            if (ready < 0) {
                Fail("Link-cable connection failed");
                cleanup();
                return;
            }
            if (ready > 0) {
                int error = 0;
#ifdef _WIN32
                int length = sizeof(error);
#else
                socklen_t length = sizeof(error);
#endif
                if (getsockopt(peer, SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>(&error), &length) != 0 ||
                    error != 0) {
                    Fail("Link-cable connection refused or unreachable");
                    cleanup();
                    return;
                }
                break;
            }
        }
    }

    if (m_stop.load(std::memory_order_acquire)) {
        cleanup();
        return;
    }
    if (peer == kInvalidSocket) {
        Fail("Link-cable connection failed");
        cleanup();
        return;
    }
    int noDelay = 1;
    setsockopt(peer, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char *>(&noDelay), sizeof(noDelay));
#ifdef SO_NOSIGPIPE
    setsockopt(peer, SOL_SOCKET, SO_NOSIGPIPE, &noDelay, sizeof(noDelay));
#endif
    SetNonBlocking(peer);
    if (m_synchronized || m_dualSCI) {
        RunSynchronizedSession(static_cast<uintptr_t>(peer));
        cleanup();
        return;
    }
    m_peakRxCount.store(0, std::memory_order_release);
    m_peakTxCount.store(0, std::memory_order_release);
    m_longestSendWaitUs.store(0, std::memory_order_release);
    m_longestReceiveWaitUs.store(0, std::memory_order_release);
    m_largestReceiveBatch.store(0, std::memory_order_release);
    m_windowPeakRxCount.store(0, std::memory_order_release);
    m_windowPeakTxCount.store(0, std::memory_order_release);
    m_windowSendWaitUs.store(0, std::memory_order_release);
    m_windowReceiveWaitUs.store(0, std::memory_order_release);
    m_windowReceiveBatch.store(0, std::memory_order_release);
    m_previousTraceSample = std::chrono::steady_clock::now();
    m_lastTcpReceiveAt = m_previousTraceSample;
    m_previousFrames = m_emulatedFrames.load(std::memory_order_relaxed);
    m_previousTransmitted = m_transmitted.load(std::memory_order_relaxed);
    m_previousSent = m_sent.load(std::memory_order_relaxed);
    m_previousReceived = m_received.load(std::memory_order_relaxed);
    m_previousDelivered = m_delivered.load(std::memory_order_relaxed);
    m_state.store(State::Connected, std::memory_order_release);

    // A serial byte must not wait for the receive thread's select timeout
    // before it reaches the peer. Wake a dedicated sender as soon as the
    // emulation thread queues it; keep socket I/O off the emulation thread.
    std::atomic<bool> stopSession{false};
    std::thread sender{[&] {
        std::array<uint8, 256> output{};
        while (!m_stop.load(std::memory_order_acquire) && !stopSession.load(std::memory_order_acquire)) {
            size_t count = 0;
            {
                std::unique_lock lock{m_txMutex};
                m_txCV.wait(lock, [&] {
                    return m_stop.load(std::memory_order_acquire) || stopSession.load(std::memory_order_acquire) ||
                           !m_tx.empty();
                });
                if (m_stop.load(std::memory_order_acquire) || stopSession.load(std::memory_order_acquire)) {
                    break;
                }
                count = std::min(m_tx.size(), output.size());
                for (size_t i = 0; i < count; ++i) {
                    output[i] = m_tx[i].value;
                }
            }
            int flags = 0;
#ifdef MSG_NOSIGNAL
            flags = MSG_NOSIGNAL;
#endif
            const int sent = send(peer, reinterpret_cast<const char *>(output.data()), static_cast<int>(count), flags);
            if (sent < 0 && IsInProgress()) {
                if (WaitSocket(peer, false, true) < 0) {
                    Fail("Link-cable send failed");
                    stopSession.store(true, std::memory_order_release);
                }
                continue;
            }
            if (sent <= 0) {
                Fail("Link-cable send failed");
                stopSession.store(true, std::memory_order_release);
                break;
            }
            m_sent.fetch_add(static_cast<uint64_t>(sent), std::memory_order_relaxed);
            const auto sentAt = std::chrono::steady_clock::now();
            {
                std::lock_guard lock{m_txMutex};
                for (int i = 0; i < sent; ++i) {
                    const auto wait = std::chrono::duration_cast<std::chrono::microseconds>(sentAt - m_tx.front().queuedAt);
                    RecordMaximum(m_longestSendWaitUs, static_cast<uint64_t>(wait.count()));
                    RecordMaximum(m_windowSendWaitUs, static_cast<uint64_t>(wait.count()));
                    m_tx.pop_front();
                }
            }
        }
    }};

    std::array<uint8, 256> buffer{};
    while (!m_stop.load(std::memory_order_acquire) && !stopSession.load(std::memory_order_acquire)) {
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(peer, &readSet);
        timeval timeout{0, 20'000};
        const int ready = select(static_cast<int>(peer + 1), &readSet, nullptr, nullptr, &timeout);
        RecordTraceSample(std::chrono::steady_clock::now());
        if (ready < 0) {
            Fail("Link-cable socket error");
            break;
        }
        if (ready == 0) {
            continue;
        }
        if (FD_ISSET(peer, &readSet)) {
            const int count = recv(peer, reinterpret_cast<char *>(buffer.data()), static_cast<int>(buffer.size()), 0);
            if (count < 0 && IsInProgress()) {
                continue;
            }
            if (count <= 0) {
                Fail("Link-cable peer disconnected");
                break;
            }
            std::lock_guard lock{m_rxMutex};
            if (m_rx.size() + static_cast<size_t>(count) > kMaxQueuedBytes) {
                Fail("Link-cable receive queue overflow");
                break;
            }
            const auto receivedAt = std::chrono::steady_clock::now();
            m_lastTcpReceiveAt = receivedAt;
            for (int i = 0; i < count; ++i) {
                m_rx.push_back({buffer[i], receivedAt});
            }
            const size_t queueSize = m_rx.size();
            m_rxCount.store(queueSize, std::memory_order_release);
            RecordMaximum(m_peakRxCount, queueSize);
            RecordMaximum(m_windowPeakRxCount, queueSize);
            RecordMaximum(m_largestReceiveBatch, static_cast<uint64_t>(count));
            RecordMaximum(m_windowReceiveBatch, static_cast<uint64_t>(count));
            m_received.fetch_add(static_cast<uint64_t>(count), std::memory_order_relaxed);
            {
                std::lock_guard diagnosticsLock{m_diagnosticsMutex};
                for (int i = 0; i < count; ++i) {
                    ++m_receivedBytes[buffer[i]];
                }
            }
        }
    }
    stopSession.store(true, std::memory_order_release);
    m_txCV.notify_all();
    sender.join();
    UpdateLiveCapture(std::chrono::steady_clock::now(), true);
    cleanup();
}

#include "link_cable_sync.inc"

} // namespace saturn::link
