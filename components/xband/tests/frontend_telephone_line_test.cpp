#include <xband/windows_modem_service.hpp>
#include <frontend_modem.hpp>
#include <flash_storage.hpp>
#include <ymir/sys/saturn.hpp>
#include <xband/virtual_media_card.hpp>
#include "pb3_pair_control.hpp"
#include <iostream>
#include <sstream>
#include <source_location>
#include <fstream>
namespace {
void check(bool ok,std::source_location where=std::source_location::current()){if(!ok)throw std::runtime_error("Frontend telephone line assertion at "+std::to_string(where.line()));}
struct Idle: xband::ServiceEndpoint {
    bool transmit(uint8_t,unsigned)override{return true;}
    void tick(unsigned)override{}
    bool peek(uint8_t&)const override{return false;}
    void consume()override{}
    size_t pending()const override{return 0;}
    void reset()override{}
};
}
int main()try{
    using Host=xband::windows::TcpHost;
    const uint16_t base=58460;
    auto pair=std::make_shared<PB3PairControl>();pair->standbyEnabled=true;
    Host service({"127.0.0.1",base,false,{{"pb3-0",std::string(64,'a')}}},xband::FrameClock(1,1),
        [](uint64_t){return std::make_unique<Idle>();});
    Host control({"127.0.0.1",uint16_t(base+4),false,{{"call-0",std::string(64,'a')}}},xband::FrameClock(1,1),
        [pair](uint64_t){return std::make_unique<PB3PairControlEndpoint>(pair,0);});
    xband::ymir_adapter::FrontendModem modem;
    auto saturn=std::make_unique<ymir::Saturn>();saturn->Reset(true);modem.attach(*saturn);
    const auto cardPath=std::filesystem::temp_directory_path()/("xband-line-card-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64()))/"card.bin";
    const xband::ymir_adapter::VirtualCardStorage::Image card={1,2,3,4,5,6,7,0,0,0,1,15,15};
    auto expectedCard=card;
    {xband::ymir_adapter::VirtualCardStorage seed(cardPath);seed.save(card);}
    modem.configureVirtualCard(cardPath,true);
    std::ostringstream initial;modem.dumpFlash(initial);
    xband::ymir_adapter::FrontendModem::Config config;config.enabled=true;config.port=base;
    const auto pump=[&](auto done,std::source_location where=std::source_location::current()){const auto until=GetTickCount64()+5000;while(GetTickCount64()<until){
        check(service.step(GetTickCount64())&&control.step(GetTickCount64()));modem.pump();
        check(modem.snapshot().enabled);if(done())return;Sleep(1);}
        throw std::runtime_error("Frontend telephone line timeout at "+std::to_string(where.line()));};
    const auto at=[&](std::string_view command){for(auto b:command)saturn->mainBus.Write<uint8_t>(0x05895001,uint8_t(b));};
    const auto reply=[&]{std::string value;for(unsigned i=0;i<256;++i){
        if(!(saturn->mainBus.Read<uint8_t>(0x05895015)&1))break;
        value+=char(saturn->mainBus.Read<uint8_t>(0x05895001));}return value;};
    // Boot with the line already OFF, including the service socket/management race.
    pair->setTelephoneLine(0,false);service.setAccepting(false);modem.request(config);
    pump([&]{return !modem.snapshot().telephoneLineConnected;});
    check(saturn->mainBus.Read<uint8_t>(0x05885029)==0x11&&modem.snapshot().virtualCardInserted);
    pump([&]{return pair->mediaCards[0].snapshot(GetTickCount64()).value("available",false);});
    check(modem.snapshot().virtualCardUnits==100);
    check(pair->mediaCards[0].snapshot(GetTickCount64()).at("units")==100);
    check(pair->mediaCards[0].request(false,GetTickCount64()));
    pump([&]{return !modem.snapshot().virtualCardInserted&&pair->mediaCards[0].pending(GetTickCount64()).is_null();});
    check(!(saturn->mainBus.Read<uint8_t>(0x05885025)&0x10)&&modem.snapshot().virtualCardUnits==100);
    check(pair->mediaCards[0].request(true,GetTickCount64()));
    pump([&]{return modem.snapshot().virtualCardInserted&&pair->mediaCards[0].pending(GetTickCount64()).is_null();});
    check((saturn->mainBus.Read<uint8_t>(0x05885025)&0x10)&&modem.snapshot().virtualCardUnits==100);
    // Existing settings UI and server controls use one insertion API. A later
    // local removal must not be overwritten by a repeated acknowledged token.
    modem.requestCardInsertion(false);pump([&]{return !modem.snapshot().virtualCardInserted;});
    for(unsigned i=0;i<8;++i)pump([&]{return true;});check(!modem.snapshot().virtualCardInserted);
    modem.requestCardInsertion(true);pump([&]{return modem.snapshot().virtualCardInserted;});
    // Drive the real mapped card pins: clear one unit in the last byte.
    const auto cardPin=[&](uint8_t value){saturn->mainBus.Write<uint8_t>(0x05885021,value);};
    cardPin(0x8d);cardPin(0x81);for(unsigned i=0;i<100;++i){cardPin(0x85);cardPin(0x81);}cardPin(0x89);cardPin(0x81);
    expectedCard[12]=7;
    pump([&]{return modem.snapshot().virtualCardUnits==99&&pair->mediaCards[0].snapshot(GetTickCount64()).at("units")==99;});
    check(pair->mediaCards[0].request(false,GetTickCount64()));
    pump([&]{return !modem.snapshot().virtualCardInserted&&pair->mediaCards[0].pending(GetTickCount64()).is_null();});
    check(modem.snapshot().virtualCardUnits==99);
    check(pair->mediaCards[0].request(true,GetTickCount64()));
    pump([&]{return modem.snapshot().virtualCardInserted&&pair->mediaCards[0].pending(GetTickCount64()).is_null();});
    check(modem.snapshot().virtualCardUnits==99); // Reinsert does not refill.
    check(pair->mediaCards[0].requestReadFault(true,GetTickCount64()));
    pump([&]{return modem.snapshot().virtualCardReadFault&&pair->mediaCards[0].pending(GetTickCount64()).is_null();});
    check(modem.snapshot().virtualCardInserted&&!modem.snapshot().virtualCardUnits);
    check(pair->mediaCards[0].matchAdmission(GetTickCount64())==xband::CardMatchAdmission::unreadable);
    check((saturn->mainBus.Read<uint8_t>(0x05885025)&0x11)==0x11); // Present but chip gives no readable data.
    cardPin(0x8d);cardPin(0x81);for(unsigned i=0;i<104;++i){
        check((saturn->mainBus.Read<uint8_t>(0x05885025)&0x11)==0x11);
        cardPin(0x85);cardPin(0x81);cardPin(0x89);cardPin(0x81);
    }
    for(bool hard:{false,true}){
        if(hard)modem.hardReset();else modem.softReset();saturn->Reset(hard);
        check(modem.snapshot().virtualCardInserted&&modem.snapshot().virtualCardReadFault);
        pump([&]{return pair->mediaCards[0].snapshot(GetTickCount64()).value("available",false);});
    }
    check(pair->mediaCards[0].requestReadFault(false,GetTickCount64()));
    pump([&]{return !modem.snapshot().virtualCardReadFault&&modem.snapshot().virtualCardUnits==99&&pair->mediaCards[0].pending(GetTickCount64()).is_null();});
    {std::ifstream input(cardPath,std::ios::binary);decltype(expectedCard) persisted{};
     input.read(reinterpret_cast<char*>(persisted.data()),persisted.size());check(bool(input)&&persisted==expectedCard);}
    at("AT\r");check(reply().find("OK")!=std::string::npos);
    at("ATZ\r");check(reply().find("OK")!=std::string::npos);
    at("ATS91=15S92=15DT0120717360\r");check(reply().find("NO CARRIER")!=std::string::npos);
    for(unsigned cycle=0;cycle<2;++cycle){
        pair->setTelephoneLine(0,true);service.setAccepting(true);
        pump([&]{return modem.snapshot().telephoneLineConnected;});check(!modem.snapshot().carrier);
        at("ATS91=15S92=15DT0120717360\r");pump([&]{return modem.snapshot().carrier;});check(reply().find("CONNECT")!=std::string::npos);
        pair->setTelephoneLine(0,false);service.setAccepting(false);
        pump([&]{return !modem.snapshot().telephoneLineConnected;});
        check(!modem.snapshot().carrier&&modem.snapshot().virtualCardConfigured&&modem.snapshot().virtualCardInserted);
        check(reply().find("NO CARRIER")!=std::string::npos);
        at("ATS91=15S92=15DT0120717360\r");check(reply().find("NO CARRIER")!=std::string::npos);
    }
    pair->setTelephoneLine(0,true);service.setAccepting(true);
    pump([&]{return modem.snapshot().telephoneLineConnected;});
    at("ATS91=15S92=15DT0120717360\r");pump([&]{return modem.snapshot().carrier;});(void)reply();
    // Unexpected service EOF: board stays alive and only explicit ATD reconnects.
    service.setAccepting(false);pump([&]{return !modem.snapshot().carrier;});
    check(reply().find("NO CARRIER")!=std::string::npos);
    service.setAccepting(true);at("ATZ\r");check(reply().find("OK")!=std::string::npos);
    at("ATS91=15S92=15DT0120717360\r");pump([&]{return modem.snapshot().carrier;});(void)reply();
    // Management EOF also ends the call, then re-joins without replaying bytes.
    control.setAccepting(false);pump([&]{return !modem.snapshot().carrier;});
    check(reply().find("NO CARRIER")!=std::string::npos);
    control.setAccepting(true);pump([&]{return pair->joined[0];});
    at("ATZ\r");check(reply().find("OK")!=std::string::npos);
    at("ATS91=15S92=15DT0120717360\r");pump([&]{return modem.snapshot().carrier;});check(reply().find("CONNECT")!=std::string::npos);
    // Real console soft/hard resets: board remains detectable before the next pump;
    // repeat it without replacing the modem/card or manually toggling settings.
    for(unsigned cycle=0;cycle<4;++cycle){
        const bool hard=cycle>=2;
        if(hard)modem.hardReset();else modem.softReset();
        saturn->Reset(hard);
        check(modem.snapshot().enabled&&!modem.snapshot().carrier&&modem.snapshot().virtualCardInserted);
        check(saturn->mainBus.Read<uint8_t>(0x05885029)==0x11);
        pump([&]{return pair->joined[0];});
        at("ATZ\r");check(reply().find("OK")!=std::string::npos);
        at("ATS91=15S92=15DT0120717360\r");pump([&]{return modem.snapshot().carrier;});check(reply().find("CONNECT")!=std::string::npos);
    }
    pair->setTelephoneLine(0,false);service.setAccepting(false);
    pump([&]{return !modem.snapshot().telephoneLineConnected;});(void)reply();
    for(bool hard:{false,true}){
        if(hard)modem.hardReset();else modem.softReset();
        saturn->Reset(hard);
        check(modem.snapshot().enabled&&!modem.snapshot().telephoneLineConnected);
        check(modem.snapshot().virtualCardInserted&&saturn->mainBus.Read<uint8_t>(0x05885029)==0x11);
        at("ATS91=15S92=15DT0120717360\r");check(reply().find("NO CARRIER")!=std::string::npos);
    }
    pair->setTelephoneLine(0,true);service.setAccepting(true);
    pump([&]{return modem.snapshot().telephoneLineConnected;});
    at("ATS91=15S92=15DT0120717360\r");pump([&]{return modem.snapshot().carrier;});(void)reply();
    // Reset-button NMI differs from menu Reset: preserve the old call until
    // the guest's mapped board-ID probe, then close via the ordinary pair path.
    modem.softReset();pump([&]{return pair->joined[0]&&!modem.snapshot().carrier&&modem.snapshot().status.starts_with("Server connected");});
    PB3PairControlEndpoint remote(pair,1);
    const auto remoteRequest=[&](nlohmann::json value){
        for(auto b:value.dump())check(remote.transmit(uint8_t(b),0));remote.tick(0);
        std::string response;uint8_t b;while(remote.peek(b)){response+=char(b);remote.consume();}
        return nlohmann::json::parse(response);
    };
    remoteRequest({{"op","join"},{"transport",pair->asynchronous?"async-v1":"lockstep"}});
    pair->roles->caller=1;pair->state=1;
    pump([&]{return reply().find("RING")!=std::string::npos;});
    at("ATA\r");pump([&]{return pair->state==2&&modem.snapshot().carrier;});
    check(reply().find("CONNECT")!=std::string::npos);
    unsigned carrierEnds=0;pair->creditCarrierEnded=[&]{++carrierEnds;};
    const auto oldGeneration=pair->generation;
    modem.consoleResetButton(true);modem.consoleResetButton(false);
    (void)saturn->mainBus.Read<uint8_t>(0x05895019); // Polling UART is not a board re-probe.
    pump([&]{return true;});check(pair->state==2&&modem.snapshot().carrier&&carrierEnds==0);
    check(saturn->mainBus.Read<uint8_t>(0x05885029)==0x11);
    check(modem.snapshot().carrier); // Bus callback performs no network or reset work.
    pump([&]{return pair->closing&&!modem.snapshot().carrier;});
    check(pair->state==0&&carrierEnds==1&&modem.snapshot().enabled&&modem.snapshot().virtualCardInserted);
    remoteRequest({{"op","closed_ack"},{"generation",oldGeneration}});
    pump([&]{return pair->generation==oldGeneration+1&&!pair->closing;});
    at("ATZ\r");check(reply().find("OK")!=std::string::npos);
    at("ATS91=15S92=15DT0120717360\r");pump([&]{return modem.snapshot().carrier;});(void)reply();
    // No stale reset latch can end this later service call.
    check(saturn->mainBus.Read<uint8_t>(0x05885029)==0x11);
    pump([&]{return true;});check(modem.snapshot().carrier&&carrierEnds==1);
    // A console-level soft reset after the button request is also completion
    // evidence; no board probe is needed and its new timeline must not fail.
    modem.softReset();pump([&]{return pair->joined[0]&&!modem.snapshot().carrier&&modem.snapshot().status.starts_with("Server connected");});
    pair->roles->caller=1;pair->state=1;
    pump([&]{return reply().find("RING")!=std::string::npos;});
    at("ATA\r");pump([&]{return pair->state==2&&modem.snapshot().carrier;});(void)reply();
    const auto secondGeneration=pair->generation;
    modem.consoleResetButton(true);modem.consoleResetButton(false);saturn->Reset(false);
    check(modem.budget(0,100)==~uint64_t{0});
    pump([&]{return pair->closing&&!modem.snapshot().carrier;});check(carrierEnds==2);
    remoteRequest({{"op","closed_ack"},{"generation",secondGeneration}});
    pump([&]{return pair->generation==secondGeneration+1&&!pair->closing;});
    at("ATZ\r");check(reply().find("OK")!=std::string::npos);
    at("ATS91=15S92=15DT0120717360\r");pump([&]{return modem.snapshot().carrier;});(void)reply();
    // A diagnostic timeline fault blocks unsafe network work, not board power.
    // Hot swap during an active service call: no modem reset/hang-up, actual
    // card-OFF interval, different persistent image, old balance preserved.
    const auto swappedPath=cardPath.parent_path()/"swapped.bin";
    auto newCard=card;newCard[8]=newCard[9]=newCard[10]=0;newCard[11]=63;newCard[12]=3; //50
    {xband::ymir_adapter::VirtualCardStorage seed(swappedPath);seed.save(newCard);}
    const auto swapUTF8=swappedPath.u8string();
    check(pair->mediaCards[0].requestImage(std::string(reinterpret_cast<const char*>(swapUTF8.data()),swapUTF8.size()),GetTickCount64()));
    pump([&]{return !modem.snapshot().virtualCardInserted&&modem.snapshot().virtualCardUnits==50;});
    check(modem.snapshot().carrier&&modem.snapshot().telephoneLineConnected);
    modem.frameCompleted();pump([&]{return modem.snapshot().virtualCardInserted;});
    check(modem.snapshot().virtualCardUnits==50&&modem.snapshot().carrier);
    modem.requestCardImage(cardPath);pump([&]{return !modem.snapshot().virtualCardInserted&&modem.snapshot().virtualCardUnits==99;});
    modem.frameCompleted();pump([&]{return modem.snapshot().virtualCardInserted;});
    check(modem.snapshot().carrier&&modem.snapshot().virtualCardUnits==99);
    (void)modem.budget(0,0);(void)modem.budget(0,1);
    check(modem.snapshot().enabled&&!modem.snapshot().carrier&&modem.snapshot().virtualCardInserted);
    (void)reply();at("AT\r");check(reply().find("OK")!=std::string::npos);
    check(saturn->mainBus.Read<uint8_t>(0x05885029)==0x11);
    std::ostringstream after;modem.dumpFlash(after);check(initial.str()==after.str());
    modem.shutdown();std::ifstream saved(cardPath,std::ios::binary);
    xband::ymir_adapter::VirtualCardStorage::Image actual{};
    saved.read(reinterpret_cast<char*>(actual.data()),actual.size());check(saved.gcount()==13&&actual==expectedCard);
    saturn.reset();
    std::cout<<"PASS real frontend: boot OFF, AT recognition, rejected dial, repeated ON/OFF, carrier loss, flash/card preserved\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
