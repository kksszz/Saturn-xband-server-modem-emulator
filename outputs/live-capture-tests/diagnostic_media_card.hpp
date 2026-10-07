#pragma once
#include <nlohmann/json.hpp>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#ifdef _WIN32
#include <windows.h>
#endif

// Opt-in fault injection, NOT normal-card emulation. All-FF reads are the
// original guest's unreadable-card sentinel. No identity or debit is fabricated.
struct DiagnosticMediaCard {
    enum class Mode { Absent, Unreadable, DiagnosticZero };
    const char *modeName() const {
        return mode == Mode::Absent ? "absent" : mode == Mode::Unreadable ? "unreadable" : "diagnostic-zero";
    }
    bool enabled = false;
    unsigned slot = 1;
    Mode mode = Mode::Absent;
    std::filesystem::path path;
    std::string lastStatus;
    std::string publicationId;
    bool accepted = false, ackPending = false;
    std::uint64_t registerReads = 0;
    std::chrono::steady_clock::time_point nextPoll{};

    static Mode parse(const nlohmann::json &j, unsigned slot) {
        if (slot < 1 || slot > 2 || j.at("schema") != "ymir.media-card-input" ||
            j.at("version") != 1 || !j.at("slots").is_array() || j.at("slots").size() != 2)
            throw std::runtime_error("Invalid card input schema");
        const nlohmann::json *selected = nullptr;
        bool seen[3] = {};
        for (const auto &s : j.at("slots")) {
            if (!s.at("slot").is_number_unsigned() && !s.at("slot").is_number_integer())
                throw std::runtime_error("Invalid card slot");
            auto id = s.at("slot").get<int>();
            if (id < 1 || id > 2 || seen[id]) throw std::runtime_error("Duplicate/invalid card slot");
            seen[id] = true;
            if (unsigned(id) == slot) selected = &s;
        }
        auto name = selected->at("mode").get<std::string>();
        if (name == "absent") return Mode::Absent;
        if (name == "unreadable") return Mode::Unreadable;
        // Research-only all-zero data, NOT an authenticated exhausted card.
        if (name == "diagnostic-zero") return Mode::DiagnosticZero;
        throw std::runtime_error("Normal/empty card encoding unsupported; exposing absent card");
    }

    void configure(unsigned side) {
        const auto env = std::getenv("YMIR_DIAGNOSTIC_CARD_STATE");
        if (!env || !*env) return; // Existing runners remain unchanged by default.
        path = std::filesystem::u8path(env);
        slot = side + 1;
        enabled = true;
        poll(true);
    }

    void poll(bool force = false) {
        if (!enabled) return;
        auto now = std::chrono::steady_clock::now();
        if (!force && now < nextPoll) return;
        nextPoll = now + std::chrono::milliseconds(250);
        std::string status;
        publicationId.clear();
        accepted = false;
        ackPending = true;
        try {
            if (std::filesystem::file_size(path) > 65536) throw std::runtime_error("Card input too large");
            std::ifstream stream(path);
            if (!stream) throw std::runtime_error("Cannot read card input");
            nlohmann::json data;
            stream >> data;
            publicationId = data.value("publication_id", std::string{});
            if (publicationId.size() > 128) throw std::runtime_error("Invalid publication id");
            mode = parse(data, slot);
            accepted = true;
            status = mode == Mode::Absent ? "absent" : mode == Mode::Unreadable ? "unreadable (all-FF fault)" : "diagnostic-zero (synthetic, identity unverified)";
        } catch (const std::exception &e) {
            mode = Mode::Absent; // Never retain a stale present card on invalid input.
            status = std::string("ERROR: ") + e.what();
        }
        if (status != lastStatus) {
            std::cout << "MEDIA_CARD slot=" << slot << " " << status << '\n';
            lastStatus = status;
        }
    }

    void acknowledge() {
        if (publicationId.empty() || path.empty()) return;
        auto destination = path;
        destination += ".slot" + std::to_string(slot) + ".ack.json";
        auto temporary = destination;
        temporary += ".tmp";
        try {
            nlohmann::json result = {{"schema","ymir.media-card-ack"},{"version",1},
                {"slot",slot},{"publication_id",publicationId},{"phase","register-read"},
                {"accepted",accepted},{"effective_mode",modeName()},
                {"register_reads",registerReads}};
            { std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
              out << result.dump(); out.flush();
              if (!out) throw std::runtime_error("Cannot write card acknowledgement"); }
#ifdef _WIN32
            if (!MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                throw std::runtime_error("Cannot replace card acknowledgement");
#else
            std::filesystem::rename(temporary, destination);
#endif
        } catch (const std::exception &e) {
            std::cerr << "MEDIA_CARD_ACK_ERROR " << e.what() << '\n';
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
        }
    }

    std::uint8_t read(std::uint8_t original) {
        if (!enabled) return original;
        ++registerReads;
        if (ackPending) { ackPending=false; acknowledge(); }
        // Preserve unrelated bits. Present+data-high makes all 104 samples FF.
        if (mode == Mode::DiagnosticZero) return (original | 0x10) & ~0x01;
        return mode == Mode::Unreadable ? original | 0x11 : original & ~0x10;
    }
};
