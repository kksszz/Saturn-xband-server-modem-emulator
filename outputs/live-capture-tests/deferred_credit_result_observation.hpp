#pragma once
#include "local_tcp_probe.hpp"
#include "credit_result_policy.hpp"
#include <nlohmann/json.hpp>

namespace diagnostic {
// Production-safe observation only: no ledger, episode inference or sender.
inline nlohmann::json deferredCreditResultObservation(const LocalTCPProbe::Bytes& request){
    uint8_t opcode=0;
    const auto raw=LocalTCPProbe::observedGameResult(request,true,&opcode);
    if(raw.empty())return nullptr;
    const auto game=LocalTCPProbe::longword(raw,4),bits=LocalTCPProbe::longword(raw,8);
    const int64_t error=bits&0x80000000u?int64_t(bits)-0x100000000LL:int64_t(bits);
    const char* evidence="unclassified";
    if(opcode==0x20&&error==0)evidence="normal-result-not-completion-proof";
    if(creditLocalReset(opcode,error))evidence="common-local-reset-report";
    if(creditPeerReset(opcode,error))evidence="common-peer-reset-report";
    if(opcode==0x23&&error==-13&&(game==0xffffffffu||game==0xfffffffeu))
        evidence="unfinished-recovery-snapshot-required";
    return {{"mode","observation-only"},{"result_opcode",opcode},{"reported_game",game},
        {"reported_error_signed",error},{"rom_evidence",evidence},{"settlement_state","unresolved"},
        {"match_episode",nullptr},{"debit_enabled",false},
        {"reason","Independent match/endpoint binding not established; no consumption queued or sent"}};
}
}
