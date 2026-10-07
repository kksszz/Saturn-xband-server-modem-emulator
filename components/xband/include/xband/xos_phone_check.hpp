#pragma once
#include <xband/vf_phone_check.hpp>
namespace xband {
template<class PeekWord, class PeekLong, class PeekByte>
bool xosPhoneCheckActive(std::string_view number, uint32_t stack,
                        PeekWord word, PeekLong value, PeekByte byte) {
    return vfPhoneCheckActive(number, stack, word, value, byte);
}
}
