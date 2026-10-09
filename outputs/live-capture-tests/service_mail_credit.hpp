#pragma once
#include "deferred_credit_settlement.hpp"
#include "local_tcp_probe.hpp"
#include <memory>

namespace diagnostic {
// Explicit server-local mail-access policy. This is not an outcome classifier,
// retry scheduler, refund handler, or a reconstruction of historical timing.
class ServiceMailCredit:public std::enable_shared_from_this<ServiceMailCredit>{
    std::shared_ptr<DeferredCreditSettlement> ledger;
    std::string episode,owner,connection;
    bool issued=false,finalized=false,confirmed=false;
    std::function<void(const char*,const nlohmann::json&)> notify;
    void event(const char* name){
        if(notify)notify(name,ledger->snapshot().at(episode));
    }
public:
    ServiceMailCredit(std::shared_ptr<DeferredCreditSettlement> value,std::string subscriber,
        std::string conn,unsigned amount,const media_card::CardReport& card,
        std::function<void(const char*,const nlohmann::json&)> events={},std::string fixedEpisode={},
        bool allowExhaustedShortfall=false):
        ledger(std::move(value)),owner(std::move(subscriber)),connection(std::move(conn)),notify(std::move(events)){
        if(!ledger||owner.empty()||connection.empty()||!card.raw||card.value<0||
           (card.value<int64_t(amount)&&!allowExhaustedShortfall)||amount==0||amount>32767||
           (allowExhaustedShortfall&&fixedEpisode.empty()))
            throw std::runtime_error("Finite card with sufficient balance required");
        const auto rows=ledger->snapshot();
        for(const auto& row:rows){
            const auto state=row.at("state").get<std::string>();
            if(state!="issued"&&state!="uncertain"&&state!="blocked")continue;
            const auto previous=row.at("initial_card").at("raw").get<std::array<uint8_t,13>>();
            if(row.at("owner")==owner||std::equal(previous.begin(),previous.begin()+8,card.raw->begin()))
                throw std::runtime_error("Unresolved prior debit; no automatic retry or new consumption");
        }
        if(!fixedEpisode.empty())episode=std::move(fixedEpisode);
        else{uint64_t next=rows.size()+1;do{episode="mail-access/"+std::to_string(next++);}while(rows.contains(episode));}
        if(!ledger->queue(episode,owner,amount,card,allowExhaustedShortfall))throw std::runtime_error("Debit episode already exists; no automatic rearm");
    }
    const std::string& episodeKey()const{return episode;}
    std::string state()const{return ledger->snapshot().at(episode).at("state").get<std::string>();}
    bool verified()const{return confirmed;}
    void interrupt(){if(issued&&!finalized){ledger->interrupt(episode);finalized=true;event("credit_service_uncertain");}}
    ~ServiceMailCredit(){try{interrupt();}catch(...){/* Issued intent remains non-replayable. */}}
    void attach(LocalTCPProbe& probe){
        if(probe.cardDebit.state!=media_card::ServiceDebitExchange::State::Disabled||probe.beforeCardDebitSend||
           probe.observeCardDebitSettlement||probe.cardDebitSettlementVerified)
            throw std::runtime_error("Automatic mail cannot replace an existing debit");
        auto self=shared_from_this();
        const auto amount=ledger->snapshot().at(episode).at("amount").get<unsigned>();
        probe.cardDebit.arm(amount,60000,true);
        probe.beforeCardDebitSend=[self](const media_card::CardReport& card,const LocalTCPProbe::Bytes& payload){
            const auto expected=self->ledger->issue(self->episode,self->connection,card);
            self->issued=true;
            if(expected!=payload)throw std::runtime_error("Mail debit payload mismatch");
            self->event("credit_service_sent");
        };
        probe.cardDebitSettlementVerified=[self]{return self->confirmed;};
        probe.observeCardDebitSettlement=[self,&probe](const media_card::ServiceDebitExchange& exchange){
            if(!self->issued||self->finalized)return;
            using S=media_card::ServiceDebitExchange::State;
            if(exchange.state==S::Completed){
                self->confirmed=self->ledger->acknowledge(self->episode,self->connection,exchange.response);
                self->finalized=true;
                self->event(self->confirmed?"credit_service_confirmed":self->state()=="exhausted"?"credit_service_exhausted":"credit_service_uncertain");
                if(self->confirmed)probe.releaseAfterCardDebitTrial();
            }else if(exchange.state==S::Uncertain)self->interrupt();
        };
        event("credit_service_pending");
    }
};
}
