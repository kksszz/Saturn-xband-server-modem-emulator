#pragma once
#include "deferred_credit_settlement.hpp"
#include "local_tcp_probe.hpp"
#include <memory>

namespace diagnostic {
// Explicit reviewed episode only; not a match classifier or automatic scheduler.
class ReviewedResetDebitBridge:public std::enable_shared_from_this<ReviewedResetDebitBridge>{
    std::shared_ptr<DeferredCreditSettlement> ledger;
    std::string episode,connection,phone;
    bool issued=false,finalized=false,confirmed=false;
public:
    ReviewedResetDebitBridge(std::shared_ptr<DeferredCreditSettlement> value,std::string ep,std::string conn,std::string subscriber):
        ledger(std::move(value)),episode(std::move(ep)),connection(std::move(conn)),phone(std::move(subscriber)){
        if(!ledger||connection.empty()||phone.empty())throw std::invalid_argument("Explicit reviewed settlement context required");
        const auto row=ledger->snapshot().at(episode);
        if(row.at("state")!="pending"||row.at("amount")!=1)throw std::runtime_error("Expected reviewed reset total1 pending");
    }
    bool verified()const{return confirmed;}
    const std::string& episodeKey()const{return episode;}
    std::string settlementState()const{return ledger->snapshot().at(episode).at("state").get<std::string>();}
    void interrupt(){
        if(issued&&!finalized){ledger->interrupt(episode);finalized=true;}
    }
    ~ReviewedResetDebitBridge(){try{interrupt();}catch(...){/* Persisted issued intent prevents replay on reload. */}}
    void attach(LocalTCPProbe& probe){
        if(probe.beforeCardDebitSend||probe.observeCardDebitSettlement||probe.cardDebitSettlementVerified||
           probe.cardDebit.state!=media_card::ServiceDebitExchange::State::Disabled)
            throw std::runtime_error("Settlement hooks require a fresh unarmed session");
        const auto self=shared_from_this();
        probe.beforeCardDebitSend=[self,&probe](const media_card::CardReport& current,const LocalTCPProbe::Bytes& payload){
            if(self->issued||self->finalized||probe.cardDebit.requested!=1||
               LocalTCPProbe::serviceRequestCode(probe.captured,true)!=4||
               xband::registrationPhone(probe.captured)!=self->phone)
                throw std::runtime_error("Reviewed reset requires matching fresh mail endpoint");
            const auto expected=self->ledger->issue(self->episode,self->connection,current);
            self->issued=true; // Intent persisted BEFORE any packet can be returned.
            if(expected!=payload)throw std::runtime_error("Settlement request payload mismatch");
        };
        probe.observeCardDebitSettlement=[self](const media_card::ServiceDebitExchange& exchange){
            if(!self->issued||self->finalized)return;
            using State=media_card::ServiceDebitExchange::State;
            if(exchange.state==State::Completed){
                self->confirmed=self->ledger->acknowledge(self->episode,self->connection,exchange.response);
                self->finalized=true;
            }else if(exchange.state==State::Uncertain)self->interrupt();
        };
        probe.cardDebitSettlementVerified=[self](){return self->verified();};
    }
};
// Shared across endpoint resets. Explicit operator reservation, never a retry queue.
class ReviewedResetReservation {
    std::shared_ptr<DeferredCreditSettlement> ledger;
    std::string episode,phone;
    unsigned side;
    bool claimed=false;
public:
    ReviewedResetReservation(std::shared_ptr<DeferredCreditSettlement> value,std::string ep,std::string subscriber,unsigned endpoint):
        ledger(std::move(value)),episode(std::move(ep)),phone(std::move(subscriber)),side(endpoint){
        if(!ledger||episode.empty()||phone.empty()||side>1)throw std::invalid_argument("Explicit reset reservation required");
        const auto row=ledger->snapshot().at(episode);
        if(row.at("state")!="pending"||row.at("amount")!=1)throw std::runtime_error("Reservation requires pending total1");
    }
    std::shared_ptr<ReviewedResetDebitBridge> arm(LocalTCPProbe& probe,unsigned endpoint,const std::string& connection){
        if(claimed||endpoint!=side||!probe.captured.empty()||
           (probe.state!=LocalTCPProbe::State::SynReceived&&probe.state!=LocalTCPProbe::State::Established))return {};
        claimed=true; // Even an interrupted pre-send call needs new operator review.
        auto bridge=std::make_shared<ReviewedResetDebitBridge>(ledger,episode,connection,phone);
        bridge->attach(probe);probe.armCardDebitTrial(1,60000);return bridge;
    }
    bool wasClaimed()const{return claimed;}
};
inline std::shared_ptr<ReviewedResetReservation> loadReviewedResetReservation(const std::filesystem::path& config){
    std::ifstream in(config,std::ios::binary);
    if(!in)throw std::runtime_error("Reviewed reset configuration unreadable");
    const auto j=nlohmann::json::parse(in);
    if(j.at("schema")!=1||j.at("kind")!="reviewed-reset-one-shot"||j.at("enabled")!=true)
        throw std::runtime_error("Explicit enabled reviewed reset configuration required");
    const auto path=std::filesystem::u8path(j.at("ledger").get<std::string>());
    if(!path.is_absolute()||!std::filesystem::is_regular_file(path))throw std::runtime_error("Existing absolute ledger path required");
    const auto side=j.at("side");
    if(!side.is_number_integer()||side.get<int64_t>()<0||side.get<int64_t>()>1)throw std::runtime_error("Endpoint must be0 or1");
    return std::make_shared<ReviewedResetReservation>(std::make_shared<DeferredCreditSettlement>(path),
        j.at("episode").get<std::string>(),j.at("phone").get<std::string>(),side.get<unsigned>());
}
}
