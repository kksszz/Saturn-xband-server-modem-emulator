#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <string_view>

namespace xband {
// Observed command subset; records requests, not physical modem capabilities.
inline bool applyObservedModemProfile(std::string_view command,bool allowNormal,
                                     std::map<std::string,int> &settings){
    constexpr std::string_view base="AT &D0&Q6&S1E0V1&C1W2L3M1S7=30S8=1";
    if(!command.starts_with(base))return false;
    const auto suffix=command.substr(base.size());
    if(!suffix.empty()&&!(allowNormal&&(suffix=="\\N"||suffix=="\\N0")))return false;
    settings={{"&D",0},{"&Q",6},{"&S",1},{"E",0},{"V",1},{"&C",1},
              {"W",2},{"L",3},{"M",1},{"S7",30},{"S8",1}};
    if(!suffix.empty())settings["\\N"]=0;
    return true;
}
enum class ATLineEvent {none,complete,overflow};
// Command mode only. Data-mode +++ timing belongs to the modem session.
inline ATLineEvent appendATByte(std::string &line,uint8_t byte){
    if(byte=='\r')return ATLineEvent::complete;
    if(byte=='\n'||(byte=='+'&&line.empty()))return ATLineEvent::none;
    if(line.size()>=128){line.clear();return ATLineEvent::overflow;}
    line.push_back(static_cast<char>(byte>='a'&&byte<='z'?byte-'a'+'A':byte));
    return ATLineEvent::none;
}
enum class ATKind {unknown,attention,hangup,reset,answer,profile,dial};
struct ATCommand {
    ATKind kind=ATKind::unknown;
    std::string number;
    std::map<std::string,int> profile;
};
inline ATCommand decodeObservedAT(std::string_view line,bool allowNormal){
    ATCommand result;
    if(line=="AT")result.kind=ATKind::attention;
    else if(line=="ATH0")result.kind=ATKind::hangup;
    else if(line=="ATZ"||line=="ATZ0")result.kind=ATKind::reset;
    else if(line=="ATA")result.kind=ATKind::answer;
    else if(applyObservedModemProfile(line,allowNormal,result.profile))result.kind=ATKind::profile;
    else{
        constexpr std::string_view prefix="ATS91=15S92=15DT";
        if(line.starts_with(prefix)){
            const auto number=line.substr(prefix.size());
            if(number.find_first_of("0123456789")!=std::string_view::npos&&
               number.find_first_not_of("0123456789-")==std::string_view::npos){
                result.kind=ATKind::dial;result.number=number;
            }
        }
    }
    return result;
}
}
