#pragma once
#include "mail_capture_store.hpp"
#include <algorithm>
#include <map>
#include <optional>
#include <span>
namespace xband {
// Local private-server routing only. A registration name is not authentication.
class MailAccountRoutes {
    std::map<std::string,std::vector<uint8_t>> names_;
public:
    void observe(const std::string &endpoint,std::span<const uint8_t> name){
        if(endpoint.empty()||endpoint.size()>64||name.empty()||name.size()>32)
            throw std::invalid_argument("Invalid observed account name");
        for(auto b:name)if(!b)throw std::invalid_argument("Embedded account terminator");
        names_[endpoint]={name.begin(),name.end()};
    }
    std::optional<std::string> uniqueEndpoint(std::span<const uint8_t> name)const{
        std::optional<std::string> result;
        for(const auto &[endpoint,value]:names_){
            if(value.size()!=name.size()||!std::equal(value.begin(),value.end(),name.begin()))continue;
            if(result)return {}; // Ambiguous: never guess the opposite modem.
            result=endpoint;
        }
        return result;
    }
    std::vector<const MailCapture*> pendingFor(const MailCaptureStore &store,const std::string &endpoint,size_t limit=4)const{
        std::vector<const MailCapture*> result;
        for(const auto &capture:store.captures()){
            const auto &to=capture.fields[0];
            if(to.empty()||to.back()!=0)continue;
            const auto target=uniqueEndpoint(std::span<const uint8_t>(to).first(to.size()-1));
            if(target&&*target==endpoint&&result.size()<limit)result.push_back(&capture);
        }
        return result; // No dequeue: delivery acknowledgement is unestablished.
    }
};
}
