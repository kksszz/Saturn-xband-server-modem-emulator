#pragma once
#include <xband/service_endpoint.hpp>
#include <deque>
#include <string_view>
#include <stdexcept>

namespace xband {
// Byte-driven wrapper around a PPP/application endpoint. Owner-thread only.
// Observed login shape, NOT historical account authentication. Password bytes
// are counted/validated but never retained. No guest RAM/PC or emulator headers.
class LoginHandshake final:public ServiceEndpoint {
public:
    enum class Phase {initial,greeting,username,password,ppp_start,ppp,failed};
    explicit LoginHandshake(ServiceEndpoint &endpoint,unsigned greetingDelay)
        :endpoint_(endpoint),delay_(greetingDelay){}
    bool transmit(uint8_t byte,unsigned frame)override{
        observe(frame);
        switch(phase_){
        case Phase::initial:
            if(byte!='\r')fail();
            emit("HELO");greeting_frame_=frame;phase_=Phase::greeting;break;
        case Phase::username:{
            constexpr std::string_view username="Pstrn\n";
            if(byte!=static_cast<uint8_t>(username[username_count_]))fail();
            if(++username_count_==username.size()){emit("Password:");phase_=Phase::password;}
            break;
        }
        case Phase::password:
            if(byte=='\n'){
                if(password_count_!=8)fail();
                phase_=Phase::ppp_start;
            }else{
                if(byte<0x21||byte>0x7e||password_count_==8)fail();
                ++password_count_;
            }
            break;
        case Phase::ppp_start:
            if(byte!=0x7e)fail();
            phase_=Phase::ppp;return endpoint_.transmit(byte,frame);
        case Phase::ppp:return endpoint_.transmit(byte,frame);
        default:fail();
        }
        return true;
    }
    void tick(unsigned frame)override{
        observe(frame);
        if(phase_==Phase::greeting&&frame-greeting_frame_>=delay_){
            emit("login:");phase_=Phase::username;
        }
        if(phase_==Phase::ppp)endpoint_.tick(frame);
    }
    bool peek(uint8_t &byte)const override{
        if(!prompts_.empty()){byte=prompts_.front();return true;}
        return phase_==Phase::ppp&&endpoint_.peek(byte);
    }
    void consume()override{
        if(!prompts_.empty())prompts_.pop_front();
        else if(phase_==Phase::ppp)endpoint_.consume();
        else throw std::logic_error("empty login response");
    }
    size_t pending()const override{return prompts_.size()+endpoint_.pending();}
    void reset()override{
        endpoint_.reset();prompts_.clear();phase_=Phase::initial;
        last_frame_=greeting_frame_=0;username_count_=password_count_=0;
    }
    Phase phase()const noexcept{return phase_;}
private:
    [[noreturn]] void fail(){
        prompts_.clear();endpoint_.reset();phase_=Phase::failed;
        throw std::runtime_error("observed login profile mismatch");
    }
    void observe(unsigned frame){
        if(phase_==Phase::failed||frame<last_frame_)fail();
        last_frame_=frame;
    }
    void emit(std::string_view text){
        if(prompts_.size()+text.size()>32)fail();
        for(auto byte:text)prompts_.push_back(static_cast<uint8_t>(byte));
    }
    ServiceEndpoint &endpoint_;
    unsigned delay_,last_frame_=0,greeting_frame_=0;
    size_t username_count_=0,password_count_=0;
    std::deque<uint8_t> prompts_;
    Phase phase_=Phase::initial;
};
}
