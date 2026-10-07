#include <xband/server_config.hpp>
#include <iostream>
template<class Char> int run(int argc, Char **argv) {
    if (argc != 2) { std::cerr << "Usage: xband_config_check <configuration.json>\nValidation only; no listening socket.\n"; return 2; }
    try {
    const auto result = xband::protocol::loadServerConfig(std::filesystem::path(argv[1]));
    if (!result) { std::cerr << xband::protocol::configErrorText(result.error) << '\n'; return 1; }
    std::cout << "Configuration valid; identities=" << result.config->identities.size()
        << "; LAN opt-in=" << (result.config->allow_lan ? "yes" : "no") << ". No socket opened.\n";
    return 0;
    } catch (...) { std::cerr << "Cannot read configuration\n"; return 1; }
}
#ifdef _WIN32
int wmain(int argc, wchar_t **argv) { return run(argc, argv); }
#else
int main(int argc, char **argv) { return run(argc, argv); }
#endif
