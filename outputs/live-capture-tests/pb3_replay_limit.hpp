#pragma once
#include <cstdint>
#include <cstdlib>
#include <string_view>
// Explicit diagnostic-only short replay; same clock/synchronization quantum.
inline uint64_t pb3ReplayLimit(){
    const char *value=std::getenv("XBAND_PB3_SHORT_REPLAY");
    return value&&std::string_view(value)=="1"?22118400ULL:2211840000ULL;
}
