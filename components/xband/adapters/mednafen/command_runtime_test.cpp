#include "command_runtime.h"
#include "card_runtime.h"
#include "peer_poll_schedule.hpp"
#include <xband/windows_tcp_host.hpp>
#include <mmsystem.h>
#include "pb3_pair_control.hpp"
#include <cstdlib>
#include <deque>
#include <iostream>
#include <string>
#include <filesystem>
static unsigned checks=0,commands=0,clears=0;
static bool joined=false;
static unsigned readyNotices=0,cancelNotices=0;
static void check(bool b){++checks;if(!b){std::cerr<<"failed check "<<checks<<'\n';std::exit(1);}}
static void log(const char*){++commands;}
static void clear(){++clears;}
static std::vector<std::string> peerDiagnostics;
static void peerLog(const char* text){peerDiagnostics.emplace_back(text);}
static bool sawDiagnostic(const std::string& prefix){
 return std::any_of(peerDiagnostics.begin(),peerDiagnostics.end(),[&](const auto& text){return text.starts_with(prefix);});
}
struct Echo: xband::ServiceEndpoint {
 std::deque<uint8_t> bytes;
 bool transmit(uint8_t b,unsigned)override{bytes.push_back(b);return true;}
 void tick(unsigned)override{}
 bool peek(uint8_t& b)const override{if(bytes.empty())return false;b=bytes.front();return true;}
 void consume()override{bytes.pop_front();}
 size_t pending()const override{return bytes.size();}
 void reset()override{bytes.clear();}
};
struct Control: Echo {
 void tick(unsigned)override{
   if(bytes.empty())return;
   const std::string input(bytes.begin(),bytes.end());bytes.clear();
   const auto request=nlohmann::json::parse(input);const auto op=request.at("op").get<std::string>();
   if(!joined){check(op=="join");check(request.at("standby_protocol")=="xband-readonly-v1");joined=true;}
   if(op=="standby_ready"||op=="standby_cancel"){
     check(request.at("generation")==1);check(request.at("ticket")==7);
     if(op=="standby_ready")++readyNotices;else ++cancelNotices;
   }
   const auto reply=nlohmann::json{{"generation",1},{"closed",false},{"recoverable",true},{"caller",2},
     {"standby",{{"protocol","xband-readonly-v1"},{"ticket",7}}}}.dump();
   bytes.insert(bytes.end(),reply.begin(),reply.end());
 }
};
struct GatedEcho: Echo {
 bool transmit(uint8_t b,unsigned frame)override{check(joined);return Echo::transmit(b,frame);}
};
static void feed(XbandCommandRuntime* r,const std::string& s){for(unsigned char c:s)xband_command_feed(r,c);}
static std::string drain(XbandCommandRuntime* r){std::string s;for(int c;(c=xband_command_pop(r))>=0;)s+=char(c);return s;}
int main(){
 struct TimerPeriod {TimerPeriod(){timeBeginPeriod(1);}~TimerPeriod(){timeEndPeriod(1);}} timerPeriod;
 xband::mednafen::PeerPollSchedule pace;
 check(pace.admit(100,100));check(!pace.admit(100,101));
 check(!pace.admit(4196,100));check(pace.admit(8292,101));
 check(!pace.admit(12388,101));pace.reset();check(pace.admit(0,0));
 check(pace.controlDelay(true)==1&&pace.controlDelay(false)==16);
#ifndef XBAND_MEDNAFEN_LEGACY_PUMP
 check(xband_command_live_poll(nullptr)==0);
#endif
 auto* r=xband_command_create(log,clear);check(r!=nullptr);
 feed(r,"AT");check(drain(r).empty());feed(r,"\r");check(drain(r)=="\r\nOK\r\n");
 feed(r,"atz0\r");check(drain(r)=="\r\nOK\r\n");
 feed(r,"AT &D0&Q6&S1E0V1&C1W2L3M1S7=30S8=1\\N0\r");check(drain(r)=="\r\nOK\r\n");
 feed(r,"ATS91=15S92=15DT0120-717360\r");check(drain(r)=="\r\nNO CARRIER\r\n");
 feed(r,"UNKNOWN\r");check(drain(r)=="\r\nERROR\r\n");
 feed(r,"ATH0\r");check(drain(r)=="\r\nOK\r\n");
 check(commands==6);check(clears==2);
 // Actual observed ROM sequence, including the next attention command.
 feed(r,"AT\rATS91=15S92=15DT555\rAT\r");
 check(drain(r)=="\r\nOK\r\n\r\nBUSY\r\n\r\nOK\r\n");
 feed(r,"ATS91=15S92=15DT03-555\r");check(drain(r)=="\r\nBUSY\r\n");
 feed(r,"ATS91=15S92=15DT0120717360\r");check(drain(r)=="\r\nNO CARRIER\r\n");
 feed(r,"ATS91=15S92=15DT\r");check(drain(r)=="\r\nERROR\r\n");
 feed(r,"ATS91=15S92=15DT555X\r");check(drain(r)=="\r\nERROR\r\n");
 xband_command_reset(r);
 feed(r,"ATS91=15S92=15DT555\r");check(drain(r)=="\r\nBUSY\r\n");
 feed(r,"AT\r");xband_command_reset(r);check(drain(r).empty());
 feed(r,"AT\r");check(drain(r)=="\r\nOK\r\n");
 feed(r,std::string(129,'A'));check(drain(r)=="\r\nERROR\r\n");
 xband_command_reset(r);
 for(unsigned i=0;i<1000&&!xband_command_failed(r);++i)feed(r,"AT\r");
 check(xband_command_failed(r));check(drain(r).empty());
 xband_command_reset(r);check(!xband_command_failed(r));feed(r,"AT\r");check(drain(r)=="\r\nOK\r\n");
 xband_command_destroy(r);check(xband_command_pop(nullptr)==-1);check(xband_command_failed(nullptr));
 xband_command_reset(nullptr);xband_command_feed(nullptr,0);xband_command_destroy(nullptr);
 // Isolated ephemeral loopback server; no production server/card/mail files.
 using Host=xband::windows::TcpHost;
 Host host({"127.0.0.1",58280,false,{{"service-0",std::string(64,'a')}}},xband::FrameClock(1,60),
   [](uint64_t){return std::make_unique<Echo>();},Host::PortMode::loopback_ephemeral_test);
 r=xband_command_create(nullptr,nullptr);
 check(!xband_command_configure_service(r,0,"0355555555"));
 check(!xband_command_configure_service(r,host.port(),"BAD"));
  check(!xband_command_configure_service(r,host.port(),"123456789012345678901"));
  check(xband_command_configure_service(r,host.port(),"12345678901234567890"));
 check(xband_command_configure_service(r,host.port(),"0355555555"));
 // ROM can initialize and dial within one guest frame, before any host poll.
 for(const char* command:{"AT\r","ATH0\r","ATZ\r","AT &D0&Q6&S1E0V1&C1W2L3M1S7=30S8=1\r"}){
   feed(r,command);check(drain(r)=="\r\nOK\r\n");
 }
 feed(r,"ATS91=15S92=15DT0120-717360\r");check(drain(r).empty());check(!xband_command_carrier(r));
 auto pump=[&](auto done,bool frames){
   const auto deadline=GetTickCount64()+5000;
   while(!done()){
     check(GetTickCount64()<deadline);check(host.step(GetTickCount64()));
     xband_command_poll(r);check(!xband_command_failed(r));
     if(frames)xband_command_frame(r);
     Sleep(1);
   }
 };
 pump([&]{return xband_command_carrier(r)!=0;},false);
 check(drain(r)=="\r\nCONNECT 14400\r\n");
 feed(r,"hello");std::string echo;
 pump([&]{echo+=drain(r);return echo.size()>=5;},true);check(echo=="hello");
 xband_command_reset(r);check(!xband_command_carrier(r));check(drain(r).empty());
 xband_command_poll(r);check(!xband_command_failed(r));
 feed(r,"ATS91=15S92=15DT0120717360\r");
 xband_command_reset(r);xband_command_poll(r);check(drain(r).empty());check(!xband_command_carrier(r));
 xband_command_destroy(r);
 // Separate control data must never leak into guest UART. Service open is
 // admitted only after control join; observer emits one ready/cancel per wait.
 joined=false;
 Host gatedHost({"127.0.0.1",58280,false,{{"service-0",std::string(64,'a')}}},xband::FrameClock(1,1),
   [](uint64_t){return std::make_unique<GatedEcho>();},Host::PortMode::loopback_ephemeral_test);
 Host controlHost({"127.0.0.1",58284,false,{{"call-0",std::string(64,'a')}}},xband::FrameClock(1,1),
   [](uint64_t){return std::make_unique<Control>();},Host::PortMode::loopback_ephemeral_test);
 r=xband_command_create(nullptr,nullptr);
 check(xband_command_configure_service(r,gatedHost.port(),"0355555555"));
 check(xband_command_configure_control(r,controlHost.port()));
 feed(r,"ATS91=15S92=15DT0120717360\r");
 auto pumpControl=[&](auto done){
   const auto deadline=GetTickCount64()+5000;
   while(!done()){
     check(GetTickCount64()<deadline);check(gatedHost.step(GetTickCount64()));check(controlHost.step(GetTickCount64()));
     xband_command_poll(r);check(!xband_command_failed(r));xband_command_frame(r);Sleep(1);
   }
 };
 pumpControl([&]{return xband_command_carrier(r)!=0;});check(joined);
 check(drain(r)=="\r\nCONNECT 14400\r\n");
 // Drop service carrier with original guard-time escape/ATH sequence.
 xband_command_frame(r);
 for(unsigned i=0;i<61;++i)xband_command_frame(r);
 feed(r,"+++");for(unsigned i=0;i<61;++i)xband_command_frame(r);
 drain(r);feed(r,"ATH0\r");check(drain(r)=="\r\nOK\r\n");
 check(!xband_command_carrier(r));
 xband_command_observe_wait(r,10,10,100,1,1,1);
 pumpControl([&]{return readyNotices==1;});check(drain(r).empty());
 xband_command_observe_wait(r,11,10,100,1,1,1);
 xband_command_observe_wait(r,110,10,100,1,1,1);
 pumpControl([&]{return cancelNotices==1;});check(readyNotices==1);check(drain(r).empty());
 xband_command_destroy(r);
 // Real server control endpoint, two isolated runtimes and independent clocks.
 auto pair=std::make_shared<PB3PairControl>();pair->standbyEnabled=true;
 Host peerControl0({"127.0.0.1",58284,false,{{"call-0",std::string(64,'a')}}},xband::FrameClock(1,1),
   [pair](uint64_t){return std::make_unique<PB3PairControlEndpoint>(pair,0);},Host::PortMode::loopback_ephemeral_test);
 Host peerControl1({"127.0.0.1",58285,false,{{"call-1",std::string(64,'a')}}},xband::FrameClock(1,1),
   [pair](uint64_t){return std::make_unique<PB3PairControlEndpoint>(pair,1);},Host::PortMode::loopback_ephemeral_test);
 Host peerService0({"127.0.0.1",58280,false,{{"service-0",std::string(64,'a')}}},xband::FrameClock(1,1),
   [](uint64_t){return std::make_unique<Echo>();},Host::PortMode::loopback_ephemeral_test);
 Host peerService1({"127.0.0.1",58281,false,{{"service-1",std::string(64,'a')}}},xband::FrameClock(1,1),
   [](uint64_t){return std::make_unique<Echo>();},Host::PortMode::loopback_ephemeral_test);
 std::array<XbandCommandRuntime*,2> peers{xband_command_create(peerLog,nullptr),xband_command_create(nullptr,nullptr)};
 check(xband_command_configure_side(peers[0],0));check(xband_command_configure_side(peers[1],1));
 check(!xband_command_configure_side(peers[1],2));
 check(xband_command_configure_service(peers[0],peerService0.port(),"0355555555"));
 check(xband_command_configure_service(peers[1],peerService1.port(),"0366666666"));
 check(xband_command_configure_control(peers[0],peerControl0.port()));
 check(xband_command_configure_control(peers[1],peerControl1.port()));
 const auto managementCardPath=std::filesystem::temp_directory_path()/
   ("mednafen-managed-card-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64())+".bin");
 auto* managementCard=xband_card_create(managementCardPath.c_str());check(managementCard);
 xband_command_configure_card(peers[0],managementCard);
 for(auto* terminal:peers)feed(terminal,"ATS91=15S92=15DT0120717360\r");
 uint64_t cycle0=0,cycle1=0;
 auto pumpPeers=[&](auto done){
   const auto deadline=GetTickCount64()+5000;
   while(!done()){
     check(GetTickCount64()<deadline);
     const auto now=GetTickCount64();
     check(peerControl0.step(now));check(peerControl1.step(now));check(peerService0.step(now));check(peerService1.step(now));
     cycle0+=5000;cycle1+=7000;
     xband_command_clock(peers[0],cycle0,0);xband_command_clock(peers[1],cycle1,0);
     for(auto* terminal:peers){xband_command_poll(terminal);check(!xband_command_failed(terminal));xband_command_frame(terminal);}
     Sleep(1);
   }
 };
 pumpPeers([&]{return xband_command_carrier(peers[0])&&xband_command_carrier(peers[1]);});
 for(auto* terminal:peers){
   check(drain(terminal)=="\r\nCONNECT 14400\r\n");
   for(unsigned i=0;i<61;++i)xband_command_frame(terminal);
   feed(terminal,"+++");for(unsigned i=0;i<61;++i)xband_command_frame(terminal);drain(terminal);
   feed(terminal,"ATH0\r");check(drain(terminal)=="\r\nOK\r\n");
 }
 // Route assignment is a fixture input, not evidence of full service matching.
 pair->roles->caller=0;
 feed(peers[0],"ATS91=15S92=15DT0312345679\r");
 std::string ringing;
 pumpPeers([&]{ringing+=drain(peers[1]);return ringing.find("RING")!=std::string::npos;});
 check(!xband_command_peer(peers[0]));check(!xband_command_peer(peers[1]));
 feed(peers[1],"ATA\r");
 pumpPeers([&]{return xband_command_peer(peers[0])&&xband_command_peer(peers[1]);});
 for(auto* terminal:peers)check(drain(terminal)=="\r\nCONNECT 14400\r\n");
 auto gameUART=[](XbandCommandRuntime* terminal){
   xband_command_board_identified(terminal);
   for(auto value:{0,6,2,1})xband_command_observe_uart_write(terminal,2,uint8_t(value));
   xband_command_observe_uart_write(terminal,3,0x80);
   xband_command_observe_uart_write(terminal,0,8); // Divisor writes are not sequence controls.
   xband_command_observe_uart_write(terminal,3,3);
   xband_command_observe_uart_write(terminal,4,3);
 };
 auto xosUART=[](XbandCommandRuntime* terminal){
   xband_command_observe_uart_write(terminal,2,6);
   xband_command_observe_uart_write(terminal,2,0xc9);
   xband_command_observe_uart_write(terminal,3,0x80);
   xband_command_observe_uart_write(terminal,0,8);
   xband_command_observe_uart_write(terminal,1,0);
   xband_command_observe_uart_write(terminal,3,0x13);
   xband_command_observe_uart_write(terminal,4,0x0b);
 };
 gameUART(peers[0]);xband_command_poll(peers[0]);check(xband_command_peer(peers[0]));
 // Real control TCP UI commands operate on the card, not the live modem call.
 pumpPeers([&]{return pair->mediaCards[0].fresh(GetTickCount64());});
 check(pair->mediaCards[0].request(false,GetTickCount64()));
 pumpPeers([&]{const auto s=pair->mediaCards[0].snapshot(GetTickCount64());return !s.at("pending").get<bool>()&&!s.at("inserted").get<bool>();});
 check(xband_command_peer(peers[0])&&xband_command_peer(peers[1]));
 check(pair->mediaCards[0].requestReadFault(true,GetTickCount64()));
 pumpPeers([&]{return pair->mediaCards[0].snapshot(GetTickCount64()).value("read_fault",false);});
 check(xband_card_units(managementCard)==-1);
 // Wait for the command acknowledgement before the next UI operation.
 pumpPeers([&]{return !pair->mediaCards[0].snapshot(GetTickCount64()).at("pending").get<bool>();});
 check(pair->mediaCards[0].requestReadFault(false,GetTickCount64()));
 pumpPeers([&]{const auto s=pair->mediaCards[0].snapshot(GetTickCount64());return !s.at("pending").get<bool>()&&!s.value("read_fault",true);});
 check(pair->mediaCards[0].request(true,GetTickCount64()));
 pumpPeers([&]{const auto s=pair->mediaCards[0].snapshot(GetTickCount64());return !s.at("pending").get<bool>()&&s.at("inserted").get<bool>();});
 check(xband_command_peer(peers[0])&&xband_command_peer(peers[1])&&xband_card_units(managementCard)==100);
 xband_command_observe_uart_write(peers[0],2,0xc9);
 check(sawDiagnostic("PEER UART CONTROL reg=2 value=201"));
 check(xband_command_peer(peers[0]));
 feed(peers[0],"at\r");std::string rawAttention;
 pumpPeers([&]{rawAttention+=drain(peers[1]);return rawAttention.size()>=3;});
 check(rawAttention=="at\r"&&drain(peers[0]).empty());
 check(sawDiagnostic("DATA MODE: ORIGINAL AT TEXT at"));
 check(xband_command_peer(peers[0])); // Trace must not convert payload to a command.
 const std::string binary("\x7e\x00\xff\x11",4);feed(peers[0],binary);
 check((xband_command_tx_status(peers[0])&0x40)==0);
 std::string binaryReceived;
 pumpPeers([&]{binaryReceived+=drain(peers[1]);return binaryReceived.size()>=binary.size();});
 check(binaryReceived==binary);check(drain(peers[0]).empty());
 // Cart-event-style owner pumping between frame boundaries. No additional
 // guest frames, synthetic UART bursts or overlapping/replayed exchanges.
 auto lastHostPump=GetTickCount64();
 unsigned livePumps=0;
 auto pumpLivePeers=[&](auto done){
   const auto deadline=GetTickCount64()+5000;
   while(!done()){
     const auto now=GetTickCount64();check(now<deadline);
     check(peerControl0.step(now));check(peerControl1.step(now));
     check(peerService0.step(now));check(peerService1.step(now));
     cycle0+=5000;cycle1+=7000;
     xband_command_clock(peers[0],cycle0,0);xband_command_clock(peers[1],cycle1,0);
#ifndef XBAND_MEDNAFEN_LEGACY_PUMP
     for(auto* terminal:peers){
       livePumps+=xband_command_live_poll(terminal);
       check(xband_command_live_poll(terminal)==0); // Same guest cycle is paced.
     }
#endif
     if(now-lastHostPump>=16){
       lastHostPump=now;
       for(auto* terminal:peers){xband_command_poll(terminal);xband_command_frame(terminal);}
     }
     for(auto* terminal:peers)check(!xband_command_failed(terminal)&&xband_command_peer(terminal));
     Sleep(1);
   }
 };
 std::vector<uint64_t> roundTrips;
 for(unsigned i=0;i<16;++i){
   const std::string sent(1,char(0x80+i));std::string forward,back;
   const auto start=std::chrono::steady_clock::now();feed(peers[0],sent);
   pumpLivePeers([&]{forward+=drain(peers[1]);return !forward.empty();});check(forward==sent);
   feed(peers[1],forward);
   pumpLivePeers([&]{back+=drain(peers[0]);return !back.empty();});check(back==sent);
   roundTrips.push_back(std::chrono::duration_cast<std::chrono::microseconds>(
     std::chrono::steady_clock::now()-start).count());
 }
#ifndef XBAND_MEDNAFEN_LEGACY_PUMP
 check(livePumps>0);
#endif
 std::sort(roundTrips.begin(),roundTrips.end());
 std::cout<<"XBAND_LATENCY samples=16 median_ms="<<(roundTrips[7]+roundTrips[8])/2000.0
          <<" min_ms="<<roundTrips.front()/1000.0<<" max_ms="<<roundTrips.back()/1000.0<<" live_pumps="<<livePumps<<'\n';
 xband_command_uart_activity(peers[1],0); // Begin a fresh FIFO timeout observation.
 check(xband_command_interrupt(peers[1],1,0xC1,1)==1);
 cycle1+=75001;xband_command_clock(peers[1],cycle1,1);
 check(xband_command_interrupt(peers[1],1,0xC1,1)==12);
 // An armed reset from an old ticket must not close a later call.
 xband_command_reset_button_nmi(peers[0]);
 for(unsigned i=0;i<61;++i)xband_command_frame(peers[0]);
 feed(peers[0],"+++");for(unsigned i=0;i<61;++i)xband_command_frame(peers[0]);drain(peers[0]);
 feed(peers[0],"ATH0\r");check(drain(peers[0])=="\r\nOK\r\n");
 check(sawDiagnostic("DATA MODE: ORIGINAL +++ CANDIDATE"));
 check(sawDiagnostic("ORIGINAL ESCAPE ACCEPTED: COMMAND MODE"));
 std::string carrierLost;
 pumpPeers([&]{carrierLost+=drain(peers[1]);return carrierLost.find("NO CARRIER")!=std::string::npos;});
 check(!xband_command_peer(peers[1]));
 pumpPeers([&]{return pair->generation==2;});
 for(auto* terminal:peers)drain(terminal);
 // Wait until both owners have observed the fresh generation before dialing.
 const auto renewalSettled=GetTickCount64()+80;
 pumpPeers([&]{return GetTickCount64()>=renewalSettled;});
 pair->roles->caller=0;
 feed(peers[0],"ATS91=15S92=15DT0312345679\r");ringing.clear();
 pumpPeers([&]{ringing+=drain(peers[1]);return ringing.find("RING")!=std::string::npos;});
 feed(peers[1],"ATA\r");
 pumpPeers([&]{return xband_command_peer(peers[0])&&xband_command_peer(peers[1]);});
 for(auto* terminal:peers)check(drain(terminal)=="\r\nCONNECT 14400\r\n");
 // A board-ID read alone and an old armed ticket do not close the new call.
 xband_command_board_identified(peers[0]);xband_command_board_identified(peers[1]);
 xband_command_poll(peers[0]);xband_command_poll(peers[1]);
 check(xband_command_peer(peers[0])&&xband_command_peer(peers[1]));
 xband_command_reset_button_nmi(peers[0]);xband_command_poll(peers[0]);
 check(xband_command_peer(peers[0])); // NMI without reinitialization is insufficient.
 xband_command_board_identified(peers[0]);
 check(xband_command_peer(peers[0])); // No external work from the bus observer.
 xband_command_poll(peers[0]);check(!xband_command_peer(peers[0]));
 check(drain(peers[0])=="\r\nNO CARRIER\r\n");carrierLost.clear();
 pumpPeers([&]{carrierLost+=drain(peers[1]);return carrierLost.find("NO CARRIER")!=std::string::npos;});
 check(!xband_command_peer(peers[1]));
 pumpPeers([&]{return pair->generation==3;});
 check(!pair->failed&&!pair->closing);
 for(auto* terminal:peers)drain(terminal);
 const auto thirdSettled=GetTickCount64()+80;
 pumpPeers([&]{return GetTickCount64()>=thirdSettled;});
 pair->roles->caller=0;
 feed(peers[0],"ATS91=15S92=15DT0312345679\r");ringing.clear();
 pumpPeers([&]{ringing+=drain(peers[1]);return ringing.find("RING")!=std::string::npos;});
 feed(peers[1],"ATA\r");
 pumpPeers([&]{return xband_command_peer(peers[0])&&xband_command_peer(peers[1]);});
 for(auto* terminal:peers)check(drain(terminal)=="\r\nCONNECT 14400\r\n");
 // Previous call's gameplay arm must not leak into this ticket.
 xband_command_board_identified(peers[0]);xosUART(peers[0]);
 xband_command_poll(peers[0]);check(xband_command_peer(peers[0]));
 // Initial game setup and routine FIFO operations must keep the call alive.
 gameUART(peers[1]);xband_command_poll(peers[1]);check(xband_command_peer(peers[1]));
 for(unsigned i=0;i<100;++i){
   xband_command_observe_uart_write(peers[1],2,2);
   xband_command_observe_uart_write(peers[1],2,1);
 }
 xosUART(peers[1]);xband_command_poll(peers[1]);check(xband_command_peer(peers[1]));
 // No fresh board read or an incomplete/wrong sequence
 // must not disconnect. Side 1 has no logger: tracing must not gate behavior.
 xband_command_board_identified(peers[1]);
 xband_command_observe_uart_write(peers[1],2,6);
 xband_command_observe_uart_write(peers[1],2,1);
 xosUART(peers[1]);xband_command_poll(peers[1]);check(xband_command_peer(peers[1]));
 xband_command_board_identified(peers[1]);
 // Regression: live ROM disc loading delays initialization by 315 frames.
 // Elapsed time alone must not close or invalidate the original probe.
 for(unsigned i=0;i<360;++i)xband_command_frame(peers[1]);
 xband_command_poll(peers[1]);check(xband_command_peer(peers[1]));
 xosUART(peers[1]);
 check(xband_command_peer(peers[1])); // Observers must not perform transport work.
 xband_command_poll(peers[1]);check(!xband_command_peer(peers[1]));
 check(drain(peers[1])=="\r\nNO CARRIER\r\n");carrierLost.clear();
 pumpPeers([&]{carrierLost+=drain(peers[0]);return carrierLost.find("NO CARRIER")!=std::string::npos;});
 check(!xband_command_peer(peers[0]));
 pumpPeers([&]{return pair->generation==4;});
 check(!pair->failed&&!pair->closing);
 for(auto* terminal:peers)xband_command_destroy(terminal);
 xband_card_destroy(managementCard);
 std::cout<<checks<<" command runtime checks passed\n";
}
