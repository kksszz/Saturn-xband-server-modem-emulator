#pragma once
#include <chrono>
#include <cstdint>
#include <stdexcept>
namespace diagnostic {
inline uint32_t mailAcceptanceDate(std::chrono::system_clock::time_point utc){
    // Explicit Japan time, independent of the execution host's time zone.
    const std::chrono::year_month_day day{std::chrono::floor<std::chrono::days>(utc+std::chrono::hours{9})};
    const int y=int(day.year());
    const unsigned m=unsigned(day.month()),d=unsigned(day.day());
    if(!day.ok()||y<1||y>0x7fff)throw std::runtime_error("Unsupported mail acceptance date");
    // Low seven bits are unverified; no guessed time-of-day encoding.
    return (uint32_t(y)<<16)|((m-1)<<12)|(d<<7);
}
}
