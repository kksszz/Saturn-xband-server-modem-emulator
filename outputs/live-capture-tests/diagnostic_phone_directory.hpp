#pragma once
#include <array>
#include <string>
#include <stdexcept>
// Two explicit LOCAL diagnostic subscribers. No external dialing or service lookup.
class DiagnosticPhoneDirectory {
public:
    enum class Destination { Self, Peer, Unknown };
    DiagnosticPhoneDirectory(std::string a,std::string b):numbers{canonical(a),canonical(b)} {
        if(numbers[0].empty()||numbers[1].empty()||numbers[0]==numbers[1])
            throw std::invalid_argument("local subscriber numbers must be valid and distinct");
    }
    Destination resolve(unsigned side,const std::string &dial) const {
        if(side>1)throw std::out_of_range("subscriber side");
        const auto number=canonical(dial);
        if(number.empty())return Destination::Unknown;
        if(number==numbers[side])return Destination::Self;
        if(number==numbers[1-side])return Destination::Peer;
        return Destination::Unknown;
    }
    const std::string &subscriber(unsigned side) const {
        if(side>1)throw std::out_of_range("subscriber side");
        return numbers[side];
    }
private:
    std::array<std::string,2> numbers;
    static std::string canonical(const std::string &value) {
        if(value.empty()||value.size()>24)return {};
        std::string result;bool digit=false;
        for(char c:value) {
            if(c>='0'&&c<='9'){result+=c;digit=true;}
            else if(c=='-'&&digit){digit=false;}
            else return {};
        }
        return digit&&result.size()<=20?result:std::string{};
    }
};
