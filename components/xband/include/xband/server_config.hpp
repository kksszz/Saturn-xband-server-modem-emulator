#pragma once
#include <xband/server_handshake.hpp>
#include <filesystem>
#include <fstream>
#include <optional>

namespace xband::protocol {
struct ServerConfig {
    std::string bind_address = "127.0.0.1";
    uint16_t port = 0;
    bool allow_lan = false;
    std::vector<EndpointRegistry::Identity> identities;
};
enum class ConfigError { none, io, size, syntax, schema, address, identity };
struct ConfigResult {
    ConfigError error = ConfigError::syntax;
    std::optional<ServerConfig> config;
    explicit operator bool() const noexcept { return error == ConfigError::none && config.has_value(); }
};
// IPv4 literals only: no DNS, wildcard, multicast, public address or implicit
// interface selection. The socket adapter must still verify the address is local.
inline bool permittedBind(std::string_view address, bool lan) noexcept {
    std::array<unsigned, 4> parts{}; size_t cursor = 0;
    for (auto &part : parts) {
        const auto start = cursor;
        while (cursor < address.size() && address[cursor] >= '0' && address[cursor] <= '9') {
            part = part * 10 + static_cast<unsigned>(address[cursor++] - '0');
            if (cursor - start > 3 || part > 255) return false;
        }
        if (cursor == start || (cursor - start > 1 && address[start] == '0')) return false;
        if (&part != &parts.back()) { if (cursor == address.size() || address[cursor++] != '.') return false; }
    }
    if (cursor != address.size()) return false;
    if (parts[0] == 127) return true;
    return lan && (parts[0] == 10 || (parts[0] == 172 && parts[1] >= 16 && parts[1] <= 31) ||
        (parts[0] == 192 && parts[1] == 168));
}
// Strict, bounded configuration parsing. Unknown fields/duplicate keys fail;
// no raw parser exception or secret-bearing input escapes in diagnostics.
inline ConfigResult parseServerConfig(std::string_view text) noexcept {
    if (text.empty() || text.size() > 65536) return {ConfigError::size, {}};
    try {
        using Json = nlohmann::json;
        std::vector<std::unordered_set<std::string>> objects;
        const auto root = Json::parse(text.begin(), text.end(), [&](int depth, Json::parse_event_t event, Json &value) {
            if ((event == Json::parse_event_t::object_start || event == Json::parse_event_t::array_start) && depth >= 8) throw ConfigError::syntax;
            if (event == Json::parse_event_t::object_start) objects.emplace_back();
            else if (event == Json::parse_event_t::object_end) objects.pop_back();
            else if (event == Json::parse_event_t::key && !objects.back().insert(value.get<std::string>()).second) throw ConfigError::syntax;
            return true;
        });
        if (!root.is_object() || !root.contains("version") || !root.contains("port") || !root.contains("identities")) return {ConfigError::schema, {}};
        for (const auto &item : root.items()) if (item.key() != "version" && item.key() != "port" && item.key() != "identities" &&
            item.key() != "bind_address" && item.key() != "allow_lan") return {ConfigError::schema, {}};
        if (!wire_detail::integer(root["version"], 1, 1) || !wire_detail::integer(root["port"], 1, 65535) ||
            !root["identities"].is_array() || root["identities"].empty() || root["identities"].size() > 32) return {ConfigError::schema, {}};
        ServerConfig config; config.port = root["port"].get<uint16_t>();
        if (root.contains("allow_lan")) {
            if (!root["allow_lan"].is_boolean()) return {ConfigError::schema, {}};
            config.allow_lan = root["allow_lan"].get<bool>();
        }
        if (root.contains("bind_address")) {
            if (!root["bind_address"].is_string()) return {ConfigError::schema, {}};
            config.bind_address = root["bind_address"].get<std::string>();
        }
        if (!permittedBind(config.bind_address, config.allow_lan)) return {ConfigError::address, {}};
        for (const auto &entry : root["identities"]) {
            if (!wire_detail::keys(entry, {"endpoint", "auth_key"}) || !entry["endpoint"].is_string() || !entry["auth_key"].is_string()) return {ConfigError::identity, {}};
            config.identities.push_back({entry["endpoint"].get<std::string>(), entry["auth_key"].get<std::string>()});
        }
        try { EndpointRegistry validation(config.identities); }
        catch (const std::invalid_argument &) { return {ConfigError::identity, {}}; }
        return {ConfigError::none, std::move(config)};
    } catch (ConfigError error) { return {error, {}}; }
      catch (...) { return {ConfigError::syntax, {}}; }
}
inline ConfigResult readServerConfig(std::istream &input) noexcept {
    try {
        std::array<char, 65537> bytes{};
        input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (input.bad() || (input.fail() && !input.eof())) return {ConfigError::io, {}};
        return parseServerConfig(std::string_view(bytes.data(), static_cast<size_t>(input.gcount())));
    } catch (...) { return {ConfigError::io, {}}; }
}
inline ConfigResult loadServerConfig(const std::filesystem::path &path) noexcept {
    try {
        std::ifstream input(path, std::ios::binary);
        if (!input.is_open()) return {ConfigError::io, {}};
        return readServerConfig(input);
    } catch (...) { return {ConfigError::io, {}}; }
}
inline const char *configErrorText(ConfigError error) noexcept {
    switch (error) {
    case ConfigError::none: return "OK";
    case ConfigError::io: return "Cannot read configuration";
    case ConfigError::size: return "Configuration must contain 1..65536 bytes";
    case ConfigError::syntax: return "Invalid configuration JSON";
    case ConfigError::schema: return "Invalid configuration fields";
    case ConfigError::address: return "Disallowed bind address or LAN opt-in missing";
    case ConfigError::identity: return "Invalid or duplicate configured identity";
    }
    return "Invalid configuration";
}
}
