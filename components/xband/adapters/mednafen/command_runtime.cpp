#include "command_runtime.h"
#include "card_runtime.h"
#include <xband/windows_modem_service.hpp>
#include <xband/service_identity.hpp>
#include <xband/modem_service_buffer.hpp>
#include <xband/modem_command_session.hpp>
#include <xband/local_phone_policy.hpp>
#include <xband/xos_standby_observer.hpp>
#include <xband/peer_dial_gate.hpp>
#include <xband/peer_uart_timing.hpp>
#include <xband/async_modem_link.hpp>
#include <deque>
#include <memory>

struct XbandCommandRuntime {
    std::deque<uint8_t> replies;
    std::unique_ptr<xband::ModemCommandSession> session;
    XbandCommandLog log;
    XbandCommandClearReceive clearReceive;
    XbandCardRuntime* card=nullptr;
    bool failed=false;
    uint16_t port=0;
    std::string subscriber;
    unsigned frame=0;
    uint64_t ticket=0;
    bool closing=false,batch=false,serviceBegun=false;
    xband::ModemServiceBuffer buffer;
    std::unique_ptr<xband::windows::ModemServiceClient> service;
    uint16_t controlPort=0;
    std::unique_ptr<xband::windows::ModemServiceClient> control;
    bool controlJoined=false,controlBusy=false;
    uint64_t controlTick=0,controlGeneration=0,nextControlPoll=0;
    std::string controlResponse;
    xband::XOSStandbyObserver waitObserver;
    std::optional<nlohmann::json> waitNotice;
    unsigned side=0,caller=2,hostPending=0,nextRingFrame=0;
    bool bothJoined=false,ring=false,peer=false,peerDialing=false,peerEnding=false,peerClosed=false;
    bool resetButtonArmed=false,resetBoardProbe=false;
    uint64_t resetButtonTicket=0;
    bool gameUARTReady=false,returnUARTConfirmed=false,uartProbe=false;
    unsigned uartProbeStep=0,uartProbeFrame=0;
    uint64_t uartProbeTicket=0;
    unsigned peerControlTrace=0,peerBoardTrace=0,peerEscapeTrace=0,diagnosticPluses=0;
    std::string diagnosticASCII;
    uint64_t cycles=0,peerOrigin=0,peerTicket=0,sequence=0;
    xband::PeerDialGate dialGate;
    xband::PeerUartTiming peerUart;
    xband::AsyncModemReceive peerReceive;
    std::vector<uint8_t> peerTX;
    std::optional<nlohmann::json> peerNotice;
    static constexpr uint64_t byteCycles=18750,timeoutCycles=75000;
    explicit XbandCommandRuntime(XbandCommandLog logger,XbandCommandClearReceive clear):log(logger),clearReceive(clear) {
        xband::ModemCommandSession::Hooks hooks;
        hooks.reply=[this](std::string_view text) {
            if(text.size()>4096-replies.size())return false;
            replies.insert(replies.end(),text.begin(),text.end());return true;
        };
        hooks.payload=[this](uint8_t b){return peer?peerUart.push(b):buffer.transmit(b);};
        hooks.dial=[this](std::string_view number){
            // Shared local policy before service login: the ROM tests its own
            // phone with a dial. BUSY permits its phone-setting check to finish.
            // The service number must not be mistaken for that self-check.
            const auto role=xband::localDialRole(number,side,serviceBegun);
            if(role==xband::LocalDialRole::selfCheck)return xband::ModemCommandSession::Dial::busy;
            if(role==xband::LocalDialRole::service&&port&&!closing&&buffer.dial(number))
                return xband::ModemCommandSession::Dial::pending;
            return xband::ModemCommandSession::Dial::rejected;
        };
        hooks.hangup=[this]{
            if(dialGate.active()){dialGate.reset();peerDialing=false;}
            else if(peer||peerDialing){
                peerNotice=nlohmann::json{{"op","hangup"},{"generation",controlGeneration}};
                peerEnding=true;peer=peerDialing=false;peerUart.reset();peerReceive.reset();peerTX.clear();
            }
            buffer.reset();
            // AT initialization may finish and dial again within a single
            // guest frame. There is nothing to await if no transport exists.
            closing=bool(service);
            if(!service)batch=false;
        };
        hooks.clearReceive=[this,clear]{replies.clear();if(clear)clear();};
        hooks.intercept=[this](const std::string& command){
            const auto decoded=xband::decodeObservedAT(command,true);
            if(decoded.kind==xband::ATKind::dial&&
               xband::localDialRole(decoded.number,side,serviceBegun)==xband::LocalDialRole::peer){
                if(!controlJoined||peerClosed||peerEnding){
                    const std::string reply="\r\nNO CARRIER\r\n";replies.insert(replies.end(),reply.begin(),reply.end());return true;
                }
                session->beginPeerDial();peerTicket=session->ticket();peerDialing=true;
                waitNotice.reset();dialGate.arm(controlGeneration,frame);
                if(log)log("PEER DIAL WAITING FOR CONFIRMED ROUTE");return true;
            }
            if(decoded.kind==xband::ATKind::answer){
                if(!ring||caller==side||peerEnding||peerClosed){
                    const std::string reply="\r\nNO CARRIER\r\n";replies.insert(replies.end(),reply.begin(),reply.end());return true;
                }
                session->beginPeerDial();peerTicket=session->ticket();peerDialing=true;ring=false;waitNotice.reset();
                peerNotice=nlohmann::json{{"op","answer"},{"generation",controlGeneration}};
                if(log)log("ORIGINAL ATA QUEUED");return true;
            }
            return false;
        };
        hooks.observedCommand=[this](std::string_view text){
            if(log){const std::string copy(text);log(copy.c_str());}
        };
        session=std::make_unique<xband::ModemCommandSession>(std::move(hooks));
    }
    void resetControl(){
        resetReturnUART();
        resetButtonArmed=resetBoardProbe=false;resetButtonTicket=0;
        control.reset();controlJoined=controlBusy=false;controlTick=controlGeneration=nextControlPoll=0;
        controlResponse.clear();waitObserver.reset();waitNotice.reset();
        peer=peerDialing=peerEnding=peerClosed=ring=bothJoined=false;caller=2;nextRingFrame=0;
        cycles=peerOrigin=peerTicket=sequence=0;hostPending=0;dialGate.reset();peerUart.reset();peerReceive.reset();peerTX.clear();peerNotice.reset();
    }
    void fail() noexcept {failed=true;replies.clear();service.reset();buffer.reset();resetControl();}
    void resetReturnUART(){
        gameUARTReady=returnUARTConfirmed=uartProbe=false;
        uartProbeStep=uartProbeFrame=0;uartProbeTicket=0;
    }
    void observeReturnUART(unsigned reg,uint8_t value){
        if(!peer||!uartProbe||returnUARTConfirmed)return;
        // Original board probe precedes disc loading; the live ROM can take
        // over 300 frames before its first UART write. Time alone must neither
        // cancel this probe nor close a call. Ticket and exact writes guard it.
        if(session->ticket()!=uartProbeTicket){uartProbe=false;return;}
        // Only original control writes count; DLL/DLM and IER writes may occur
        // between them. Require a fresh mapped board-ID read for each sequence.
        if(reg!=2&&reg!=3&&reg!=4)return;
        static constexpr unsigned gameRegs[]={2,2,2,2,3,3,4};
        static constexpr uint8_t gameValues[]={0,6,2,1,0x80,3,3};
        static constexpr unsigned xosRegs[]={2,2,3,3,4};
        static constexpr uint8_t xosValues[]={6,0xc9,0x80,0x13,0x0b};
        const auto* regs=gameUARTReady?xosRegs:gameRegs;
        const auto* values=gameUARTReady?xosValues:gameValues;
        const unsigned count=gameUARTReady?5:7;
        if(reg!=regs[uartProbeStep]||value!=values[uartProbeStep]){uartProbe=false;return;}
        if(++uartProbeStep!=count)return;
        uartProbe=false;
        if(gameUARTReady)returnUARTConfirmed=true;
        else gameUARTReady=true;
    }
    void finishReturnUARTCall(){
        if(!returnUARTConfirmed)return;
        const auto observedTicket=uartProbeTicket;
        resetReturnUART();
        if(!peer||session->ticket()!=observedTicket)return;
        // Compatibility boundary observed in VF Remix: game -> XOS UART
        // reinitialization, not idle time or a guessed match result. Physical
        // modem behavior at this boundary still needs real-hardware validation.
        peer=false;peerDialing=ring=false;peerEnding=true;
        peerUart.reset();peerReceive.reset();peerTX.clear();dialGate.reset();
        replies.clear();if(clearReceive)clearReceive();
        waitObserver.reset();waitNotice.reset();
        session->disconnectedIfCurrent(observedTicket,frame);
        peerNotice=nlohmann::json{{"op","hangup"},{"generation",controlGeneration}};
        if(log)log("ORIGINAL GAME TO XOS UART REINITIALIZATION: PEER CLOSE QUEUED");
    }
    void finishResetButtonCall(){
        if(!resetButtonArmed)return;
        if(!peer||session->ticket()!=resetButtonTicket){
            resetButtonArmed=resetBoardProbe=false;return;
        }
        if(!resetBoardProbe)return;
        resetButtonArmed=resetBoardProbe=false;
        // Same confirmed guest-reinitialization boundary as the Ymir adapter.
        // No winner, result or debit is inferred from a console reset.
        peer=false;peerDialing=ring=false;peerEnding=true;
        peerUart.reset();peerReceive.reset();peerTX.clear();dialGate.reset();
        waitObserver.reset();waitNotice.reset();
        session->disconnectedIfCurrent(resetButtonTicket,frame);
        peerNotice=nlohmann::json{{"op","hangup"},{"generation",controlGeneration}};
        if(log)log("RESET BUTTON GUEST REINITIALIZATION: PEER CLOSE QUEUED");
    }
    void pollControl(uint64_t now){
        if(!controlPort)return;
        using Client=xband::windows::ModemServiceClient;
        if(!control)control=std::make_unique<Client>(
            xband::windows::ClientConfig{"127.0.0.1",controlPort,false,"call-"+std::to_string(side),std::string(64,'a'),60},now);
        if(!control->step(now)){
            if(log)log(control->failureName());
            throw std::runtime_error("Standby control transport failed");
        }
        if(control->state()==Client::State::idle){
            if(!control->open(subscriber,now))throw std::runtime_error("Control open rejected");
            return;
        }
        if(controlBusy&&control->readyToRun()){
            control->drain([this](uint8_t b){
                if(controlResponse.size()>=4096)throw std::runtime_error("Control response too large");
                controlResponse+=char(b);return true;
            },4096);
            if(control->pending())return;
            const auto response=nlohmann::json::parse(controlResponse);controlResponse.clear();
            if(card&&response.contains("media_card_command")&&!response.at("media_card_command").is_null()){
                const auto command=response.at("media_card_command").dump();
                xband_card_command(card,command.c_str());
            }
            const auto generation=response.at("generation").get<uint64_t>();
            if(!generation)throw std::runtime_error("Invalid control generation");
            if(controlGeneration&&controlGeneration!=generation){
                if(!peerClosed||generation!=controlGeneration+1)throw std::runtime_error("Unexpected peer generation");
                peer=peerDialing=peerEnding=peerClosed=ring=false;caller=2;sequence=0;peerTicket=0;
                peerUart.reset();peerReceive.reset();peerTX.clear();peerNotice.reset();dialGate.reset();
            }
            if(controlGeneration!=generation){waitObserver.reset();waitNotice.reset();}
            controlGeneration=generation;
            const auto nextCaller=response.value("caller",2u);
            if(nextCaller>2||(caller<2&&nextCaller!=caller))throw std::runtime_error("Invalid caller role");
            caller=nextCaller;
            if(response.contains("joined")){const auto members=response.at("joined").get<std::array<bool,2>>();bothJoined=members[0]&&members[1];}
            const auto state=response.value("state",0u);
            if(state>2||response.value("transport",std::string("async-v1"))!="async-v1")throw std::runtime_error("Invalid peer transport/state");
            if(response.at("closed").get<bool>()){
                if(!response.at("recoverable").get<bool>())throw std::runtime_error("Control session unrecoverable");
                if(!peerClosed){
                    peer=peerDialing=peerEnding=ring=false;peerClosed=true;peerTX.clear();peerUart.reset();peerReceive.reset();dialGate.reset();
                    if(peerTicket)session->disconnectedIfCurrent(peerTicket,frame);
                    if(log)log("PEER CARRIER CLOSED");
                }
                waitObserver.reset();waitNotice.reset();peerNotice=nlohmann::json{{"op","closed_ack"},{"generation",generation}};
            }else if(response.contains("standby")){
                const auto& standby=response.at("standby");
                if(standby.at("protocol")!="xband-readonly-v1")throw std::runtime_error("Invalid standby protocol");
                waitObserver.arm(generation,standby.at("ticket").get<uint64_t>(),caller==side);
            }
            const auto gate=dialGate.observe(generation,frame,side,caller,bothJoined,!peerNotice,response.at("closed").get<bool>());
            if(gate==xband::PeerDialGate::Result::ready){
                peerNotice=nlohmann::json{{"op","dial"},{"generation",generation},{"number",side?"3336666666":"3336666665"}};
                if(log)log("ORIGINAL PEER DIAL ROUTE CONFIRMED");
            }else if(gate==xband::PeerDialGate::Result::reject){peerDialing=false;session->disconnectedIfCurrent(peerTicket,frame);}
            if(!response.at("closed").get<bool>()&&state==1&&caller<2&&caller!=side&&!peerDialing&&!peer&&
               !session->carrier()&&session->state()==xband::ModemCommandSession::State::command&&
               session->command().empty()&&replies.empty()&&!hostPending&&(!ring||frame>=nextRingFrame)){
                const std::string reply="\r\nRING\r\n";replies.insert(replies.end(),reply.begin(),reply.end());
                ring=true;nextRingFrame=frame+120;if(log)log("ORIGINAL RING QUEUED");
            }
            if(!response.at("closed").get<bool>()&&state==2&&!peer&&!peerEnding&&!peerClosed){
                if(session->carrier()||!peerDialing)throw std::runtime_error("Unsolicited peer carrier");
                replies.clear();if(clearReceive)clearReceive();peerUart.reset();peerReceive.reset();peerTX.clear();
                peerOrigin=cycles;sequence=0;
                resetReturnUART();
                peerControlTrace=peerBoardTrace=peerEscapeTrace=diagnosticPluses=0;diagnosticASCII.clear();
                if(!session->connected(peerTicket,frame))throw std::runtime_error("Stale peer CONNECT");
                peer=true;peerDialing=false;waitObserver.peerConnected();waitNotice.reset();
                if(log)log("PEER CONNECT 14400 ACKNOWLEDGED");
            }
            if(response.contains("bytes")){
                const auto values=response.at("bytes").get<std::vector<unsigned>>();
                std::vector<uint8_t> bytes;for(auto b:values){if(b>255)throw std::runtime_error("Invalid peer byte");bytes.push_back(uint8_t(b));}
                if(!bytes.empty()&&!peer&&!peerEnding&&!peerClosed)throw std::runtime_error("Peer bytes without carrier");
                if(peer)peerReceive.accept(bytes,cycles-peerOrigin,byteCycles);
            }
            if(!controlJoined&&log)log("STANDBY CONTROL JOIN ACKNOWLEDGED");
            controlJoined=true;controlBusy=false;nextControlPoll=now+16;
        }
        if(control->state()!=Client::State::connected||controlBusy||now<nextControlPoll)return;
        nlohmann::json request;
        if(!controlJoined)request={{"op","join"},{"transport","async-v1"},{"standby_protocol","xband-readonly-v1"}};
        else if(peerNotice){request=*peerNotice;peerNotice.reset();}
        else if(peer){
            if(sequence==UINT64_MAX)throw std::runtime_error("Peer sequence exhausted");
            request={{"op","exchange"},{"generation",controlGeneration},{"elapsed",cycles-peerOrigin},
                {"sequence",++sequence},{"credit",peerReceive.credit()},{"bytes",peerTX}};peerTX.clear();
        }
        else if(waitNotice){request=*waitNotice;waitNotice.reset();}
        else request={{"op","poll"}};
        if(card){
            const auto report=xband_card_report(card);
            if(!report)throw std::runtime_error("Card telemetry unavailable");
            request["media_card"]=nlohmann::json::parse(report);
        }
        const auto wire=request.dump();
        if(!control->transfer(std::span(reinterpret_cast<const uint8_t*>(wire.data()),wire.size()),++controlTick))
            throw std::runtime_error("Control transfer rejected");
        controlBusy=true;
    }
    void poll(){
        const auto now=GetTickCount64();
        finishResetButtonCall();
        finishReturnUARTCall();
        pollControl(now);
        if(closing){
            if(service){
                if(!service->requestClose(now))throw std::runtime_error("Service close rejected");
                if(!service->step(now))throw std::runtime_error("Service close failed");
                if(service->state()!=xband::windows::ModemServiceClient::State::idle)return;
                service.reset();
            }
            closing=false;batch=false;
        }
        if(buffer.state()==xband::ModemServiceBuffer::State::idle)return;
        if(!service){
            ticket=session->ticket();
            service=std::make_unique<xband::windows::ModemServiceClient>(
                xband::windows::ClientConfig{"127.0.0.1",port,false,xband::serviceEndpoint(side),std::string(64,'a'),60},now);
        }
        if(!service->step(now)){
            if(log)log(service->failureName());
            service.reset();buffer.reset();batch=false;
            session->disconnectedIfCurrent(ticket,frame);return;
        }
        if(buffer.state()==xband::ModemServiceBuffer::State::dialing){
            if(service->state()==xband::windows::ModemServiceClient::State::idle&&(!controlPort||controlJoined))
                if(!service->open(subscriber,now))throw std::runtime_error("Service open rejected");
            if(service->state()==xband::windows::ModemServiceClient::State::connected){
                buffer.confirmConnected();
                if(!session->connected(ticket,frame))throw std::runtime_error("Stale service CONNECT");
                serviceBegun=true;if(log)log("SERVICE OPEN ACKNOWLEDGED");
            }
        }
    }
    void completedFrame(){
        ++frame;const bool wasData=session->dataMode();session->tick(frame);
        if(peer&&wasData&&!session->dataMode()&&log)log("ORIGINAL ESCAPE ACCEPTED: COMMAND MODE");
        if(dialGate.observe(controlGeneration,frame,side,caller,bothJoined,false,peerClosed||peerEnding)==xband::PeerDialGate::Result::reject){
            peerDialing=false;session->disconnectedIfCurrent(peerTicket,frame);
        }
        if(peer||!service||closing||!session->dataMode())return;
        if(batch&&service->readyToRun()){
            service->drain([this](uint8_t b){if(replies.size()==4096)return false;replies.push_back(b);return true;},256);
            if(service->pending())return;
            batch=false;
        }
        if(!batch&&service->state()==xband::windows::ModemServiceClient::State::connected){
            if(!service->transfer(buffer.outgoing(),frame))throw std::runtime_error("Service transfer rejected");
            buffer.transmitted();batch=true;
        }
    }
    void clock(uint64_t now,unsigned pending){
        if(now<cycles)throw std::runtime_error("Guest clock reversed; reset required");cycles=now;hostPending=pending;
        if(!peer)return;
        const auto elapsed=cycles-peerOrigin;
        peerUart.tick(elapsed,byteCycles,[this](uint8_t b){if(peerTX.size()==256)return false;peerTX.push_back(b);return true;});
        peerReceive.tick(elapsed,byteCycles,[this,elapsed](uint8_t b){
            if(replies.size()+hostPending>=256)return false;replies.push_back(b);peerUart.activity(elapsed);return true;
        });
    }
};
extern "C" XbandCommandRuntime* xband_command_create(XbandCommandLog log,XbandCommandClearReceive clear) {
    try{return new XbandCommandRuntime(log,clear);}catch(...){return nullptr;}
}
extern "C" void xband_command_destroy(XbandCommandRuntime* r){delete r;}
extern "C" void xband_command_configure_card(XbandCommandRuntime* r,XbandCardRuntime* card){if(r)r->card=card;}
extern "C" void xband_command_reset_button_nmi(XbandCommandRuntime* r){
    if(!r||r->failed||!r->peer)return;
    r->resetButtonArmed=true;r->resetBoardProbe=false;r->resetButtonTicket=r->session->ticket();
    if(r->log)r->log("RESET BUTTON NMI: PEER CLOSE ARMED");
}
extern "C" void xband_command_board_identified(XbandCommandRuntime* r){
    if(!r||r->failed)return;
    if(r->peer&&r->peerBoardTrace++<8&&r->log)r->log("PEER ACTIVE: ORIGINAL BOARD ID READ");
    if(r->peer&&!r->returnUARTConfirmed){
        r->uartProbe=true;r->uartProbeStep=0;r->uartProbeFrame=r->frame;r->uartProbeTicket=r->session->ticket();
    }
    if(r->resetButtonArmed)r->resetBoardProbe=true;
}
extern "C" void xband_command_observe_uart_write(XbandCommandRuntime* r,unsigned reg,uint8_t b){
    if(!r||r->failed||!r->peer)return;
    r->observeReturnUART(reg,b);
    if(!r->log||(reg!=2&&reg!=3&&reg!=4)||r->peerControlTrace++>=64)return;
    try{
        const auto message="PEER UART CONTROL reg="+std::to_string(reg)+" value="+std::to_string(unsigned(b))+" frame="+std::to_string(r->frame);
        r->log(message.c_str());
    }catch(...){r->fail();}
}
extern "C" void xband_command_reset(XbandCommandRuntime* r){
    if(!r)return;
    try{r->service.reset();r->buffer.reset();r->resetControl();r->session->reset(0);r->closing=false;r->batch=false;r->frame=0;r->serviceBegun=false;r->failed=false;}catch(...){r->fail();}
}
extern "C" void xband_command_feed(XbandCommandRuntime* r,uint8_t b){
    if(!r||r->failed)return;
    try{
        if(r->peer&&r->session->dataMode()){
            if(b=='+'){
                if(++r->diagnosticPluses==3&&r->peerEscapeTrace++<16&&r->log){
                    const auto message="DATA MODE: ORIGINAL +++ CANDIDATE frame="+std::to_string(r->frame);
                    r->log(message.c_str());
                }
            }else r->diagnosticPluses=0;
            if(b=='\r'){
                // Report only recognizable AT text, not arbitrary game payload.
                auto normalized=r->diagnosticASCII;
                for(auto& c:normalized)if(c>='a'&&c<='z')c=char(c-'a'+'A');
                const auto decoded=xband::decodeObservedAT(normalized,true);
                if((decoded.kind==xband::ATKind::attention||decoded.kind==xband::ATKind::hangup||decoded.kind==xband::ATKind::reset)&&r->peerEscapeTrace++<16&&r->log){
                    const auto message="DATA MODE: ORIGINAL AT TEXT "+r->diagnosticASCII+" frame="+std::to_string(r->frame);
                    r->log(message.c_str());
                }
                r->diagnosticASCII.clear();
            }else if(b>=32&&b<=126&&r->diagnosticASCII.size()<128)r->diagnosticASCII+=char(b);
            else r->diagnosticASCII.clear();
        }
        r->session->feed(b,r->frame);
    }catch(...){r->fail();}
}
extern "C" int xband_command_pop(XbandCommandRuntime* r){
    if(!r||r->failed||r->replies.empty())return -1;
    const int value=r->replies.front();r->replies.pop_front();return value;
}
extern "C" int xband_command_failed(const XbandCommandRuntime* r){return !r||r->failed;}
extern "C" int xband_command_configure_service(XbandCommandRuntime* r,uint16_t port,const char* subscriber){
    try{
        if(!r||r->service||r->buffer.state()!=xband::ModemServiceBuffer::State::idle||port<1024||!subscriber)return 0;
        const auto digits=xband::phoneDigits(subscriber);if(digits.empty())return 0;
        r->port=port;r->subscriber=digits;return 1;
    }catch(...){return 0;}
}
extern "C" void xband_command_poll(XbandCommandRuntime* r){
    if(!r||r->failed)return;try{r->poll();}catch(const std::exception& e){if(r->log)r->log(e.what());r->fail();}catch(...){r->fail();}
}
extern "C" int xband_command_configure_control(XbandCommandRuntime* r,uint16_t port){
    if(!r||r->control||port<1024)return 0;r->controlPort=port;return 1;
}
extern "C" int xband_command_configure_side(XbandCommandRuntime* r,unsigned side){
    if(!r||r->control||r->service||side>1)return 0;r->side=side;return 1;
}
extern "C" void xband_command_clock(XbandCommandRuntime* r,uint64_t cycles,unsigned pending){
    if(!r||r->failed)return;
    try{r->clock(cycles,pending);}catch(const std::exception& e){if(r->log)r->log(e.what());r->fail();}catch(...){r->fail();}
}
extern "C" int xband_command_peer(const XbandCommandRuntime* r){return r&&!r->failed&&r->peer;}
extern "C" uint8_t xband_command_tx_status(const XbandCommandRuntime* r){return r&&!r->failed&&r->peer?r->peerUart.txStatus():0x60;}
extern "C" uint8_t xband_command_interrupt(const XbandCommandRuntime* r,uint8_t ier,uint8_t fcr,unsigned bytes){
    return r&&!r->failed&&r->peer?r->peerUart.interrupt(ier,fcr,bytes,r->cycles-r->peerOrigin,r->timeoutCycles):((ier&1)&&bytes?4:1);
}
extern "C" void xband_command_uart_activity(XbandCommandRuntime* r,int clearHolding){
    if(!r||r->failed||!r->peer)return;
    r->peerUart.activity(r->cycles-r->peerOrigin);if(clearHolding)r->peerUart.clearHolding();
}
extern "C" void xband_command_observe_wait(XbandCommandRuntime* r,uint32_t clock,uint32_t start,
    uint32_t duration,int active,int monitor,int verified){
    if(!r||r->failed||!r->controlJoined||r->waitNotice||r->peer||r->peerDialing)return;
    try{
        const auto notice=r->waitObserver.observe({clock,start,duration,active!=0,monitor!=0,r->session->carrier(),false,verified!=0});
        if(notice!=xband::XOSStandbyObserver::Notice::none){
            r->waitNotice=nlohmann::json{{"op",notice==xband::XOSStandbyObserver::Notice::ready?"standby_ready":"standby_cancel"},
                {"generation",r->waitObserver.generation()},{"ticket",r->waitObserver.ticket()}};
            if(r->log)r->log(notice==xband::XOSStandbyObserver::Notice::ready?"STANDBY READY OBSERVED":"STANDBY CANCEL OBSERVED");
        }
    }catch(...){r->fail();}
}
extern "C" void xband_command_frame(XbandCommandRuntime* r){
    if(!r||r->failed)return;try{r->completedFrame();}catch(...){r->fail();}
}
extern "C" int xband_command_carrier(const XbandCommandRuntime* r){return r&&!r->failed&&r->session->carrier();}
