#pragma once
#include "../../components/xband/include/xband/at_command.hpp"

// Exact observed setup strings only; requested settings are recorded, not
// a claim of physical modem/error-correction behavior. Unknown input is atomic.
inline bool applyDiagnosticModemProfile(std::string_view command, bool allowNormal,
                                        std::map<std::string,int> &settings) {
    return xband::applyObservedModemProfile(command,allowNormal,settings);
}
