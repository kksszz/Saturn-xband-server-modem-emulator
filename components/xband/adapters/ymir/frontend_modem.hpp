#pragma once
#include <memory>
#include <string>
#include <cstdint>
#include <iosfwd>
#include <filesystem>
namespace ymir {struct Saturn;}
namespace xband::ymir_adapter {
// Desktop-facing boundary. Only request()/snapshot() cross threads. Declare
// this owner before Saturn; attach while stopped, then pump on emulator thread.
class FrontendModem {
public:
    struct Config {bool enabled=false,allowLAN=false;int side=0,port=58240;std::string address="127.0.0.1",phone="3336666666";};
    struct Snapshot {bool enabled=false,carrier=false;unsigned frame=0;uint64_t sent=0,received=0;std::string status="Disabled";};
    FrontendModem();~FrontendModem();
    void attach(ymir::Saturn &saturn);
    // Call during frontend initialization, before emulator thread starts.
    // On error, persistence stays disabled and existing files are untouched.
    void configureStorage(const std::filesystem::path &path);
    void request(Config config);
    Snapshot snapshot()const;
    bool hasPendingRequest()const;
    bool pump();
    // Owner thread, between scheduling iterations only. At most one select
    // with a 1 ms requested timeout; never waits for a complete transaction.
    void waitForActivity();
    void frameCompleted();
    uint64_t budget(uint64_t cycle,uint64_t revision)noexcept;
    void reset(const char *reason);
    // Emulator owner thread only, between scheduling iterations. Diagnostic
    // raw bytes; never performs mapped reads or changes flash/UART state.
    void dumpFlash(std::ostream &out) const;
    void shutdown();
private:struct Impl;std::unique_ptr<Impl> impl;
};
}
