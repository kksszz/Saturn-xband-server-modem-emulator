#pragma once
#include "media_card_debit_wire.hpp"
#include <limits>

namespace media_card {
// Explicit experimental session transaction, not a pricing/retry/refund policy.
class ServiceDebitExchange {
public:
    enum class State { Disabled, Armed, Waiting, Completed, Uncertain };
    State state=State::Disabled;
    uint32_t requested=0;
    std::optional<int64_t> result;
    uint64_t deadline=0,timeout=0;
    std::vector<uint8_t> response;
    bool fullCardReply=false;
    std::optional<CardReport> updatedCard;
    std::optional<CardReport> initialCard;
    // Positive evidence of the observed exhausted-card partial reply, not an
    // arbitrary mismatch, missing card, timeout or invented negative balance.
    bool exhaustedShortfall()const{
        if(state!=State::Completed||!fullCardReply||!result||*result<0||
           *result>=int64_t(requested)||!initialCard||!initialCard->raw||
           !updatedCard||!updatedCard->raw||initialCard->value!=*result||
           updatedCard->value!=0)return false;
        for(unsigned i=0;i<8;++i)if((*initialCard->raw)[i]!=(*updatedCard->raw)[i])return false;
        // The native card value is a weighted bit count (base8 digits), not
        // ordinary population count across all five bytes.
        int64_t value=0;
        for(unsigned i=8;i<13;++i){
            unsigned bits=0;for(unsigned b=0;b<8;++b)bits+=((*initialCard->raw)[i]>>b)&1u;
            value=value*8+bits;
            if((*updatedCard->raw)[i]!=0)return false;
        }
        return value==initialCard->value;
    }
    // Diagnostic continuation guard, NOT a reconstructed historical policy.
    const char* continuationProblem()const{
        if(state!=State::Completed||result!=int64_t(requested))
            return "Nonmatching consumption result; no continuation, retry or refund";
        if(!fullCardReply||!initialCard||!initialCard->raw||!updatedCard||!updatedCard->raw)
            return "Card reports incomplete; no continuation, retry or refund";
        if(initialCard->value<0||updatedCard->value<0||
           int64_t(initialCard->value)-updatedCard->value!=int64_t(requested))
            return "Balance difference does not match debit; no continuation, retry or refund";
        for(unsigned i=0;i<8;++i)if((*initialCard->raw)[i]!=(*updatedCard->raw)[i])
            return "Card identity changed; no continuation, retry or refund";
        return nullptr;
    }
    void arm(uint32_t amount,uint64_t timeoutMs,bool fullReply=false){
        if(state!=State::Disabled||amount>32767||timeoutMs==0)
            throw std::invalid_argument("Invalid or repeated explicit debit trial");
        requested=amount;timeout=timeoutMs;fullCardReply=fullReply;state=State::Armed;
    }
    std::vector<uint8_t> request(const CardReport& card,uint64_t now){
        if(state!=State::Armed||!card.raw)
            throw std::invalid_argument("Debit trial requires an inserted reported card");
        auto wire=debitRequest(requested,*card.raw);
        initialCard=card;
        deadline=timeout>std::numeric_limits<uint64_t>::max()-now?
            std::numeric_limits<uint64_t>::max():now+timeout;
        state=State::Waiting;return wire; // Exactly once; no retransmit path.
    }
    void expire(uint64_t now){
        if(state==State::Waiting&&now>=deadline)state=State::Uncertain;
    }
    void abandon(){
        if(state==State::Waiting||state==State::Armed)state=State::Uncertain;
    }
    void receive(std::span<const uint8_t> bytes,uint64_t now){
        expire(now);
        if(state!=State::Waiting)return;
        const size_t maximum=fullCardReply?25:5;
        if(bytes.size()>maximum-response.size()){
            state=State::Uncertain;return;
        }
        response.insert(response.end(),bytes.begin(),bytes.end());
        if(!response.empty()&&response[0]!=0x1e){state=State::Uncertain;return;}
        if(!fullCardReply){
            // Isolated handler-body fixture only; not the whole Saturn wire reply.
            if(response.size()==5){result=debitReply(response);state=State::Completed;}
            return;
        }
        if(response.size()>=2&&response[1]!=0){state=State::Uncertain;return;}
        if(response.size()<8)return;
        uint32_t length=0;for(unsigned i=4;i<8;++i)length=(length<<8)|response[i];
        if(length!=0&&length!=13){state=State::Uncertain;return;}
        const size_t expected=8+length+4;
        if(response.size()>expected){state=State::Uncertain;return;}
        if(response.size()==expected){
            updatedCard=registrationCardReport(std::span<const uint8_t>(response).first(8+length));
            std::array<uint8_t,5> body{0x1e};
            for(unsigned i=0;i<4;++i)body[1+i]=response[8+length+i];
            result=debitReply(body);state=State::Completed;
        }
    }
};
}
