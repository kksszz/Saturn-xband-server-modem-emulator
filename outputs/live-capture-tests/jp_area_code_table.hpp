#pragma once
#include <algorithm>
#include <nlohmann/json.hpp>
#include <xband/local_phone_policy.hpp>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>
namespace diagnostic {
struct RegionLookup {
    std::string status="unknown",display;
    std::set<std::string> prefectures,areas;
    size_t matchedDigits=0;
};
// Immutable offline snapshot. A telephone prefix indicates the allocation
// region, not the current user's location, identity or authenticated address.
class JapanAreaCodeTable {
    struct Entry{std::set<std::string> prefectures,areas;};
    std::map<std::string,Entry> prefixes;
    std::map<std::string,Entry> areaCodes;
    size_t rows=0;
    static bool digits(const std::string& s){return !s.empty()&&std::all_of(s.begin(),s.end(),[](char c){return c>='0'&&c<='9';});}
public:
    explicit JapanAreaCodeTable(const std::filesystem::path& file){
        if(std::filesystem::file_size(file)>2*1024*1024)throw std::runtime_error("Oversized region table");
        std::ifstream in(file,std::ios::binary);if(!in)throw std::runtime_error("Cannot open region table");
        const auto j=nlohmann::json::parse(in);if(j.at("schema")!=1||!j.at("rows").is_array()||!j.at("sources").is_array())throw std::runtime_error("Invalid region table schema");
        for(const auto&r:j.at("rows")){
            const auto prefs=r.at("prefectures").get<std::vector<std::string>>();
            const auto codes=r.at("area_codes").get<std::vector<std::string>>();
            const auto numbers=r.at("number_prefixes").get<std::vector<std::string>>();
            if(prefs.empty()||codes.empty()||numbers.empty()||r.at("source").get<size_t>()>=j.at("sources").size()||r.at("page").get<unsigned>()==0)throw std::runtime_error("Invalid region row");
            for(const auto&p:prefs)if(p.empty()||p.size()>24||p.find('\0')!=std::string::npos)throw std::runtime_error("Invalid prefecture");
            for(const auto&c:codes){
                if(!digits(c)||c[0]!='0'||c.size()<2||c.size()>5)throw std::runtime_error("Invalid area code");
                auto &entry=areaCodes[c];entry.prefectures.insert(prefs.begin(),prefs.end());entry.areas.insert(c);
            }
            for(const auto&n:numbers){
                if(!digits(n)||n[0]!='0'||n.size()<2||n.size()>6)throw std::runtime_error("Invalid numbering prefix");
                auto &entry=prefixes[n];entry.prefectures.insert(prefs.begin(),prefs.end());entry.areas.insert(codes.begin(),codes.end());
            }
            ++rows;
        }
        if(!rows||rows>2000)throw std::runtime_error("Invalid region table size");
    }
    size_t rowCount()const{return rows;}
    size_t prefixCount()const{return prefixes.size();}
    RegionLookup lookup(const std::string& phone)const{
        RegionLookup result;const auto number=xband::phoneDigits(phone);
        if(number.size()<2||number.size()>10){result.status="not_domestic_fixed_line";return result;}
        // Short diagnostic numbers are accepted only when every matching
        // geographic area code agrees on the prefecture. Do not use 0422
        // to hide an ambiguous broader042 match in a shortened number.
        if(number.size()<10){
            for(const auto&[code,entry]:areaCodes)if(number.starts_with(code)){
                result.matchedDigits=std::max(result.matchedDigits,code.size());
                result.prefectures.insert(entry.prefectures.begin(),entry.prefectures.end());result.areas.insert(code);
            }
            if(result.prefectures.size()==1){result.status="resolved_area_code";result.display=*result.prefectures.begin();}
            else if(!result.prefectures.empty())result.status="ambiguous";
            return result;
        }
        for(size_t n=std::min<size_t>(6,number.size());n>=2;--n){
            const auto it=prefixes.find(number.substr(0,n));if(it==prefixes.end())continue;
            result.matchedDigits=n;result.prefectures=it->second.prefectures;result.areas=it->second.areas;
            if(result.prefectures.size()==1){result.status="resolved";result.display=*result.prefectures.begin();}
            else result.status="ambiguous";
            return result;
        }
        return result;
    }
};
inline std::shared_ptr<const JapanAreaCodeTable> activeRegions;
}
