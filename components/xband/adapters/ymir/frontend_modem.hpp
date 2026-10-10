#pragma once
#include <memory>
#include <string>
#include <cstdint>
#include <iosfwd>
#include <filesystem>
#include <optional>
namespace ymir {struct Saturn;}
namespace xband::ymir_adapter {
// Desktop-facing boundary. Only request()/snapshot() cross threads. Declare
// this owner before Saturn; attach while stopped, then pump on emulator thread.
class FrontendModem {
public:
    struct Config {bool enabled=false,allowLAN=false;int side=0,port=58240;std::string address="127.0.0.1",phone="3336666666";};
    struct Snapshot {bool enabled=false,carrier=false;unsigned frame=0;uint64_t sent=0,received=0;std::string status="Disabled";bool virtualCardConfigured=false,virtualCardInserted=false;bool telephoneLineConnected=true;std::optional<int32_t> virtualCardUnits;bool virtualCardSaveFailed=false,virtualCardReadFault=false;};
    FrontendModem();~FrontendModem();
    void attach(ymir::Saturn &saturn);
    // Call during frontend initialization, before emulator thread starts.
    // On error, persistence stays disabled and existing files are untouched.
    void configureStorage(const std::filesystem::path &path);
    // Experimental explicit13-byte card file, before emulator thread starts.
    // Requires an existing image; never creates identity, balance or recharge.
    void configureVirtualCard(const std::filesystem::path &path,bool inserted=false);
    // UI-safe load/swap request. A running modem/call is preserved; inserted
    // replacement exposes OFF until a later frame and then returns to ON.
    void requestCardImage(std::filesystem::path path);
    // Thread-safe insertion request, applied at the next owner-thread pump.
    void requestCardInsertion(bool inserted);
    // Runtime unreadable-chip simulation; does not edit the card image.
    void requestCardReadFault(bool fault);
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
    // Owner thread: console soft reset ends old calls but keeps the configured
    // modem present. State restore/rewind must still use reset(), not this API.
    void softReset();
    // Hard Reset has the same modem lifecycle: preserve the peripheral's
    // persistent contents/presence and phone-line switch, discard old calls.
    void hardReset();
    // Owner thread: arm deferred carrier cleanup for console Reset button.
    // Let the guest report/finish first; clean up only after its board re-probe.
    void consoleResetButton(bool pressed);
    // Emulator owner thread only, between scheduling iterations. Diagnostic
    // raw bytes; never performs mapped reads or changes flash/UART state.
    void dumpFlash(std::ostream &out) const;
    void shutdown();
private:struct Impl;std::unique_ptr<Impl> impl;
};
}
