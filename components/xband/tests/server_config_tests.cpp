#include <xband/server_config.hpp>
#include <sstream>
#include <iostream>
using namespace xband::protocol;
using Json = nlohmann::json;
void check(bool ok, const char *why) { if (!ok) throw std::runtime_error(why); }
Json config() { return {{"version", 1}, {"port", 58133}, {"identities", Json::array({{{"endpoint", "modem1"}, {"auth_key", std::string(64, 'a')}}})}}; }
int main(int argc, char **argv) {
    try {
        if (argc != 2) return 2;
        const std::string_view name = argv[1]; auto value = config();
        if (name == "config_defaults") {
            const auto r = parseServerConfig(value.dump()); check(bool(r) && r.config->bind_address == "127.0.0.1" && !r.config->allow_lan && r.config->port == 58133, "safe default");
            EndpointRegistry registry(r.config->identities); int owner = 0;
            check(registry.acquire("modem1", std::string(64, 'a'), &owner) == EndpointRegistry::Result::acquired, "registry integration");
        } else if (name == "config_address") {
            for (const auto *address : {"0.0.0.0", "8.8.8.8", "224.0.0.1", "localhost", "::", "192.168.001.1", "10.0.0.256", "10.0.0.1.", "172.32.0.1"}) {
                value["bind_address"] = address; value["allow_lan"] = true;
                check(parseServerConfig(value.dump()).error == ConfigError::address, "unsafe/malformed bind refused");
            }
            for (const auto *address : {"10.0.0.2", "172.16.0.2", "172.31.0.2", "192.168.1.2"}) {
                value["bind_address"] = address; value["allow_lan"] = false; check(!parseServerConfig(value.dump()), "LAN needs opt-in");
                value["allow_lan"] = true; check(bool(parseServerConfig(value.dump())), "explicit private bind accepted syntactically");
            }
        } else if (name == "config_schema") {
            value["tls"] = false; check(!parseServerConfig(value.dump()), "unknown key"); value = config();
            for (const Json port : {Json(0), Json(-1), Json(65536), Json(1.5), Json("58133"), Json(true)}) { value["port"] = port; check(!parseServerConfig(value.dump()), "invalid port"); }
            check(!parseServerConfig("{\"version\":1,\"version\":1}"), "duplicate keys");
            value = config(); value["identities"].push_back(value["identities"][0]); check(parseServerConfig(value.dump()).error == ConfigError::identity, "duplicate endpoint");
            value = config(); value["identities"][0]["auth_key"] = "secret-not-hex"; const auto result = parseServerConfig(value.dump());
            check(!result && std::string(configErrorText(result.error)).find("secret") == std::string::npos, "no input leaked");
        } else if (name == "config_stream") {
            std::istringstream input(value.dump()); check(bool(readServerConfig(input)), "read stream");
            std::istringstream huge(std::string(65537, ' ')); check(readServerConfig(huge).error == ConfigError::size, "bounded read");
            std::istringstream broken; broken.setstate(std::ios::badbit); check(readServerConfig(broken).error == ConfigError::io, "IO failure");
        } else throw std::runtime_error("unknown test");
        std::cout << "PASS " << name << '\n'; return 0;
    } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
