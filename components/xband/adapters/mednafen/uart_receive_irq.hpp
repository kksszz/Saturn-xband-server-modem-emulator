#pragma once
#include <xband/modem_register_bank.hpp>
namespace xband { namespace mednafen_probe {
// Match the existing Ymir observation-stage receive policy. SCU external
// input 12 is an index within EXT0..EXTF, not SCU internal interrupt 12.
inline bool receiveIRQ(const ModemRegisterBank& bank) {
    return (bank.ier&1)!=0 && !bank.rx.empty();
}
template<class Sink> void updateReceiveIRQ(const ModemRegisterBank& bank,Sink sink) {
    sink(12,receiveIRQ(bank));
}
} }
