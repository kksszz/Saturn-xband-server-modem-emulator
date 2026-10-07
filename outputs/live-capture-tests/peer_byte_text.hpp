#pragma once
#include <sstream>
#include <string>

namespace diagnostic {
// Display only: never interpret arbitrary peer bytes as protocol commands or
// assume a Japanese encoding. Escapes preserve binary bytes and line breaks.
inline std::wstring peerByteText(const std::string& hex) {
    if(hex.empty()||hex=="--")return L"--";
    if(hex.size()>1024)return L"(invalid HEX)";
    auto nibble=[](char c)->int {
        if(c>='0'&&c<='9')return c-'0';
        if(c>='a'&&c<='f')return c-'a'+10;
        if(c>='A'&&c<='F')return c-'A'+10;
        return -1;
    };
    constexpr wchar_t digits[]=L"0123456789ABCDEF";
    std::istringstream input(hex);std::string token;std::wstring out;
    while(input>>token){
        if(token.size()!=2||nibble(token[0])<0||nibble(token[1])<0)return L"(invalid HEX)";
        const unsigned byte=unsigned(nibble(token[0])*16+nibble(token[1]));
        if(byte=='\\')out+=L"\\\\";
        else if(byte>=0x20&&byte<=0x7e)out+=wchar_t(byte);
        else {out+=L"\\x";out+=digits[byte>>4];out+=digits[byte&15];}
    }
    return out.empty()?L"--":out;
}
}
