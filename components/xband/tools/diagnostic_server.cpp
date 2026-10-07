#include <xband/windows_tcp_host.hpp>
#include "diagnostic_endpoint.hpp"
#include <chrono>
#include <atomic>
#include <iostream>
namespace {
std::atomic<bool> interrupted{false};
BOOL WINAPI stopSignal(DWORD event) {
    if (event != CTRL_C_EVENT && event != CTRL_BREAK_EVENT) return FALSE;
    interrupted.store(true); return TRUE;
}
struct Handler {
    Handler() { if (!SetConsoleCtrlHandler(stopSignal, TRUE)) throw std::runtime_error("Console handler failed"); }
    ~Handler() { SetConsoleCtrlHandler(stopSignal, FALSE); }
};
}
int wmain(int argc, wchar_t **argv) {
    if (argc < 3 || argc > 5) {
        std::cerr << "Usage: xband_diagnostic_server <config.json> <duration-ms:1..60000> [--ephemeral-loopback] [--status-stdout]\n"
            "Diagnostic only. Loopback only; fixture clock_hz=1000, 1000 ticks/frame. Not a game server.\n";
        return 2;
    }
    try {
        uint64_t duration = 0; std::wstring_view text(argv[2]);
        if (text.empty() || text.size() > 5) return 2;
        for (const auto c : text) { if (c < L'0' || c > L'9') return 2; duration = duration * 10 + static_cast<uint64_t>(c - L'0'); }
        if (!duration || duration > 60000) return 2;
        bool ephemeral = false, status_stdout = false;
        for (int i = 3; i < argc; ++i) {
            const std::wstring_view option(argv[i]);
            if (option == L"--ephemeral-loopback" && !ephemeral) ephemeral = true;
            else if (option == L"--status-stdout" && !status_stdout) status_stdout = true;
            else return 2;
        }
        const auto config = xband::protocol::loadServerConfig(std::filesystem::path(argv[1]));
        if (!config) { std::cerr << xband::protocol::configErrorText(config.error) << '\n'; return 1; }
        if (config.config->bind_address != "127.0.0.1" || config.config->allow_lan) {
            std::cerr << "Diagnostic runner requires 127.0.0.1 and allow_lan=false\n"; return 1;
        }
        Handler handler;
        using Host = xband::windows::TcpHost;
        Host host(*config.config, xband::FrameClock(1, 1000), [](uint64_t hz) -> std::unique_ptr<xband::ServiceEndpoint> {
            if (hz != 1000) throw std::runtime_error("Diagnostic clock mismatch");
            return std::make_unique<OwnedDiagnosticEndpoint>();
        }, ephemeral ? Host::PortMode::loopback_ephemeral_test : Host::PortMode::configured);
        const auto publish = [&] {
            if (!status_stdout) return;
            const auto wall_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
            std::cout << "HOST_STATUS " << nlohmann::json{{"emitted_at_ms", wall_ms}, {"host", host.status()}}.dump() << '\n' << std::flush;
        };
        std::cout << "Diagnostic listener: 127.0.0.1:" << host.port() << "; duration_ms=" << duration << '\n' << std::flush;
        const auto start = std::chrono::steady_clock::now();
        uint64_t next_status = 0;
        while (!interrupted.load()) {
            const auto elapsed = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
            if (elapsed >= duration) break;
            if (!host.step(elapsed)) { std::cerr << "Diagnostic host stopped after an error\n"; return 1; }
            if (elapsed >= next_status) { publish(); next_status = elapsed + 500; }
            Sleep(1);
        }
        host.stop(); publish(); std::cout << "Stopped; sockets closed.\n"; return 0;
    } catch (...) { std::cerr << "Diagnostic server startup/run failed\n"; return 1; }
}
