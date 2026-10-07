#pragma once
#include <nlohmann/json.hpp>
#include <string>

namespace xband::monitor {
inline nlohmann::json mailClearStatus(const nlohmann::json &observation){
    for(const auto *key:{"submission_clear_experiment","batch_clear_experiment"}){
        const auto status=observation.value(key,nlohmann::json::object());
        if(status.value("enabled",false))return status;
    }
    return nlohmann::json::object();
}
inline std::string mailSavedLine(const nlohmann::json &observation){
    const auto state=observation.value("mail_capture_state",std::string("disabled"));
    if(state=="disabled")return "Saved: capture disabled";
    if(state=="empty")return "Saved: no new outgoing mail";
    if(state=="retained")return observation.value("mail_snapshot_committed",false)?
        "Saved: snapshot committed":"Saved: RAM custody (see storage)";
    if(state=="source_unresolved")return "Saved: HOLD - source unknown";
    if(state=="profile_epoch_lost")return "Saved: HOLD - context invalid";
    if(state=="capacity_refused")return "Saved: refused - capacity";
    return "Saved: pending / not confirmed";
}
inline std::string mailIncomingLine(const nlohmann::json &observation){
    if(observation.value("dummy_mail_enabled",false))return observation.value("dummy_mail_reply_queued",false)?
        "Incoming reply: dummy prepared":"Incoming reply: dummy mode";
    const auto selection=observation.value("mail_selection",nlohmann::json::object());
    return selection.empty()?"Incoming reply: not reported":
        "Incoming reply prepared: "+std::to_string(selection.value("prepared",size_t{}));
}
inline std::string mailHeldLine(const nlohmann::json &observation){
    const auto selection=observation.value("mail_selection",nlohmann::json::object());
    return selection.empty()?"Held inventory: not reported":
        "Held, not sent again: "+std::to_string(selection.value("withheld",size_t{}));
}
inline std::string mailClearLine(const nlohmann::json &observation){
    const auto clear=mailClearStatus(observation);
    if(clear.empty())return "Outbox clear: experiment OFF";
    if(clear.value("prepared",false))return clear.value("snapshot_committed",false)?
        "Outbox clear: prepared after save":"Outbox clear: inconsistent status";
    return "Outbox clear: not prepared";
}
// Presentation only. Counters describe the last response preparation, not
// successful guest delivery. Never infer card status, receipts or live routes.
inline std::string mailSelectionLine(const nlohmann::json &observation){
    const auto selection=observation.value("mail_selection",nlohmann::json::object());
    if(selection.empty())return "Mail counters: not reported";
    return "Prepared "+std::to_string(selection.value("prepared",size_t{}))+
        " | held "+std::to_string(selection.value("withheld",size_t{}))+
        " | later "+std::to_string(selection.value("deferred",size_t{}));
}
inline std::string mailStorageLine(const nlohmann::json &store){
    if(!store.value("enabled",false))return "Mail custody: disabled";
    const auto storage=store.value("storage",std::string{});
    if(storage=="snapshot-experiment")return "Storage: snapshot (experimental)";
    if(storage=="memory")return "Storage: memory only";
    return "Storage: not reported";
}
}
