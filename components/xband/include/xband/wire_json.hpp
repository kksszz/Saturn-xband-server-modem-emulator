#pragma once
#include <xband/protocol_state.hpp>
#include <xband/base64.hpp>
#include <nlohmann/json.hpp>
#include <initializer_list>
#include <string>
#include <unordered_set>
#include <vector>
#include <memory>

namespace xband::protocol {
enum class WireError { none, syntax, duplicate_key, depth, size, envelope, unsupported, body };
struct WireMessage {
    WireError error = WireError::syntax;
    nlohmann::json value;
    explicit operator bool() const noexcept { return error == WireError::none; }
};
namespace wire_detail {
using Json = nlohmann::json;
// Protocol objects normally have at most seven keys. Keep those inline while
// preserving exact duplicate detection and hashed handling of larger objects.
class ObjectKeys {
    std::array<std::string,8> small_;
    size_t size_=0;
    std::unique_ptr<std::unordered_set<std::string>> large_;
public:
    void reset(){size_=0;large_.reset();}
    bool insert(const std::string &key){
        if(large_)return large_->insert(key).second;
        for(size_t i=0;i<size_;++i)if(small_[i]==key)return false;
        if(size_<small_.size()){small_[size_++]=key;return true;}
        large_=std::make_unique<std::unordered_set<std::string>>();
        for(const auto &item:small_)large_->insert(item);
        return large_->insert(key).second;
    }
};
inline bool keys(const Json &value, std::initializer_list<const char *> names) {
    if (!value.is_object() || value.size() != names.size()) return false;
    for (auto name : names) if (!value.contains(name)) return false;
    return true;
}
inline bool text(const Json &value, size_t low, size_t high) {
    return value.is_string() && value.get_ref<const std::string &>().size() >= low &&
        value.get_ref<const std::string &>().size() <= high;
}
inline bool integer(const Json &value, uint64_t low, uint64_t high) {
    if (!value.is_number_integer()) return false;
    if (value.is_number_unsigned()) {
        const auto n = value.get<uint64_t>(); return n >= low && n <= high;
    }
    const auto n = value.get<int64_t>();
    return n >= 0 && static_cast<uint64_t>(n) >= low && static_cast<uint64_t>(n) <= high;
}
inline bool decimalField(const Json &value, bool nonzero = false) {
    uint64_t n = 0;
    return value.is_string() && decimal(value.get_ref<const std::string &>(), n) && (!nonzero || n != 0);
}
inline bool tokenField(const Json &value) {
    return value.is_string() && token(value.get_ref<const std::string &>());
}
inline bool digits(const Json &value, size_t low, size_t high) {
    if (!text(value, low, high)) return false;
    for (char c : value.get_ref<const std::string &>()) if (c < '0' || c > '9') return false;
    return true;
}
inline bool choice(const Json &value, std::initializer_list<const char *> choices) {
    for (auto item : choices) if (value == item) return true;
    return false;
}
inline bool card(const Json &c) {
    if (!keys(c, {"inserted", "read_state", "number", "remaining_units", "nominal_units"}) ||
        !c["inserted"].is_boolean() || !choice(c["read_state"], {"absent", "readable", "unreadable", "unknown"}) ||
        !(c["number"].is_null() || digits(c["number"], 1, 32)) ||
        !(c["remaining_units"].is_null() || integer(c["remaining_units"], 0, 100)) ||
        !(c["nominal_units"].is_null() || integer(c["nominal_units"], 10, 10) ||
          integer(c["nominal_units"], 50, 50) || integer(c["nominal_units"], 100, 100))) return false;
    if (!c["inserted"].get<bool>())
        return c["read_state"] == "absent" && c["number"].is_null() && c["remaining_units"].is_null() && c["nominal_units"].is_null();
    if (c["read_state"] == "absent") return false;
    return c["remaining_units"].is_null() || c["nominal_units"].is_null() || c["remaining_units"] <= c["nominal_units"];
}
}

// Pure validation. No authentication, dispatch, socket reads or state changes.
// Allocation failures propagate to the host boundary; malformed input is returned
// as an error with no partially validated DOM. Input cap bounds parser allocation.
inline WireMessage parseWire(std::string_view input) {
    using namespace wire_detail;
    if (input.empty() || input.size() > 65536) return {WireError::size, {}};
    if (input.size() >= 3 && static_cast<unsigned char>(input[0]) == 0xef &&
        static_cast<unsigned char>(input[1]) == 0xbb && static_cast<unsigned char>(input[2]) == 0xbf)
        return {WireError::syntax, {}};
    Json value;
    std::array<ObjectKeys,8> objects;
    size_t objectDepth=0;
    try {
        value = Json::parse(input.begin(), input.end(),
            [&](int depth, Json::parse_event_t event, Json &item) {
                if ((event == Json::parse_event_t::object_start || event == Json::parse_event_t::array_start) && depth >= 8)
                    throw WireError::depth;
                if (event == Json::parse_event_t::object_start) objects[objectDepth++].reset();
                else if (event == Json::parse_event_t::object_end) --objectDepth;
                else if (event == Json::parse_event_t::key && !objects[objectDepth-1].insert(item.get_ref<const std::string &>()))
                    throw WireError::duplicate_key;
                return true;
            });
    } catch (WireError error) { return {error, {}}; }
      catch (const Json::exception &) { return {WireError::syntax, {}}; }
    if (!keys(value, {"v", "type", "endpoint", "session", "id", "reply_to", "body"}) ||
        !integer(value["v"], 2, 2) || !value["type"].is_string() ||
        !value["endpoint"].is_string() || !endpoint(value["endpoint"].get_ref<const std::string &>()) ||
        !decimalField(value["id"], true) ||
        !(value["reply_to"].is_null() || decimalField(value["reply_to"], true)) || !value["body"].is_object())
        return {WireError::envelope, {}};
    const auto &type = value["type"].get_ref<const std::string &>();
    const bool handshake = type == "hello" || type == "hello_ok";
    // Connection errors may precede session issuance. Dispatcher must enforce
    // pre-authentication policy; an empty-session error grants no authority.
    const bool earlyError = type == "error" && value["session"] == "";
    if (handshake ? value["session"] != "" : (!earlyError && !tokenField(value["session"]))) return {WireError::envelope, {}};
    const auto &b = value["body"];
    bool valid = false, response = false;
    if (type == "ping" || type == "pong") { valid = b.empty(); response = type == "pong"; }
    else if (type == "open") {
        valid = keys(b, {"target", "subscriber"}) && b["target"] == "local-service" && digits(b["subscriber"], 1, 20);
    } else if (type == "open_ok" || type == "close_ok") {
        valid = keys(b, {"call"}) && tokenField(b["call"]); response = true;
    } else if (type == "close") {
        valid = keys(b, {"call", "reason"}) && tokenField(b["call"]) && (b["reason"] == "hangup" || b["reason"] == "reset");
    } else if (type == "advance" || type == "advance_ok") {
        valid = keys(b, {"call", "tick"}) && tokenField(b["call"]) && decimalField(b["tick"]); response = type == "advance_ok";
    } else if (type == "data_ack") {
        valid = keys(b, {"call", "next_offset", "limit"}) && tokenField(b["call"]) && decimalField(b["next_offset"]) && decimalField(b["limit"]);
    } else if (type == "hello") {
        valid = keys(b, {"client", "clock_hz", "auth_key"}) && text(b["client"], 1, 64) && integer(b["clock_hz"], 1, 1000000000) && text(b["auth_key"], 64, 64);
        if (valid) for (char c : b["auth_key"].get_ref<const std::string &>())
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) valid = false;
    } else if (type == "hello_ok") {
        valid = keys(b, {"new_session", "max_chunk", "rx_window"}) && tokenField(b["new_session"]) && integer(b["max_chunk"], 4096, 4096) && integer(b["rx_window"], 65536, 65536); response = true;
    } else if (type == "data") {
        valid = keys(b, {"call", "offset", "bytes", "payload_b64"}) && tokenField(b["call"]) &&
            decimalField(b["offset"]) && integer(b["bytes"], 1, 4096) && b["payload_b64"].is_string();
        if (valid) {
            size_t decoded = 0;
            valid = base64Size(b["payload_b64"].get_ref<const std::string &>(), decoded) && decoded == b["bytes"].get<size_t>();
        }
    } else if (type == "snapshot") {
        valid = keys(b, {"call", "subscriber", "state", "sent_bytes", "received_bytes", "card"}) &&
            (b["call"].is_null() || tokenField(b["call"])) && (b["subscriber"].is_null() || digits(b["subscriber"], 1, 20)) &&
            choice(b["state"], {"idle", "opening", "service", "closing", "error"}) &&
            decimalField(b["sent_bytes"]) && decimalField(b["received_bytes"]) && card(b["card"]);
        if (valid && b["call"].is_null()) valid = b["sent_bytes"] == "0" && b["received_bytes"] == "0";
    } else if (type == "error") {
        valid = keys(b, {"code", "message", "scope", "call"}) &&
            choice(b["code"], {"VERSION_MISMATCH", "INVALID_MESSAGE", "ENDPOINT_IN_USE", "STALE_SESSION", "STALE_CALL", "BUSY",
                "SEQUENCE_ERROR", "FLOW_CONTROL_ERROR", "UNSUPPORTED_GUEST_DATA", "INTERNAL_ERROR", "AUTH_FAILED"}) &&
            text(b["message"], 1, 256) && choice(b["scope"], {"connection", "call"}) &&
            (b["call"].is_null() || tokenField(b["call"]));
        if (valid && b["scope"] == "call") valid = !earlyError && tokenField(b["call"]);
        if (valid && earlyError) valid = b["scope"] == "connection" && b["call"].is_null();
    } else return {WireError::unsupported, {}};
    // Errors can be unsolicited or correlated to the rejected request.
    if (!valid || (type != "error" && response == value["reply_to"].is_null())) return {WireError::body, {}};
    return {WireError::none, std::move(value)};
}
}
