#include "frontend_modem.hpp"
#include <xband/windows_modem_service.hpp>
#include <xband/modem_command_session.hpp>
#include <xband/modem_service_buffer.hpp>
#include <xband/peer_uart_timing.hpp>
#include <xband/async_modem_link.hpp>
#include <xband/local_phone_policy.hpp>
#include <xband/xos_phone_check.hpp>
#include <xband/xos_standby_observer.hpp>
#include <xband/peer_dial_gate.hpp>
#include "board_attachment.hpp"
#include "flash_storage.hpp"
#include <ymir/sys/saturn.hpp>
#include <mutex>
#include <optional>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ostream>

namespace xband::ymir_adapter {
using Client=windows::ModemServiceClient;
using Json=nlohmann::json;
struct FrontendModem::Impl {
    mutable std::mutex mutex;
    std::optional<Config> requested;
    Snapshot published;
    Config config;
    ymir::Saturn *saturn=nullptr;
    ModemRegisterBank bank;
    std::unique_ptr<FlashStorage> storage;
    unsigned storageFrames=0;
    void saveFlash() {
        if(!storage||bank.programBytes)return;
        try {storage->save(bank.flashData);}
        catch(const std::exception &e){std::fprintf(stderr,"XBAND_FLASH_SAVE_FAILURE %s\n",e.what());}
    }
    std::unique_ptr<BoardAttachment> board;
    std::unique_ptr<Client> service,control;
    ModemServiceBuffer buffer;
    std::unique_ptr<ModemCommandSession> session;
    unsigned frame=0;
    uint64_t sent=0,received=0,controlTick=0,generation=0,origin=0,revision=0,elapsed=0,end=0;
    bool enabled=false,failed=false,closeService=false,serviceBatch=false,controlBusy=false,joined=false;
    bool peer=false,ring=false,answering=false,clockReady=false,needStep=false,sample=false,timelineKnown=false;
    bool serviceBegun=false;
    bool peerEnding=false,peerClosed=false,peerDialing=false;
    unsigned caller=2; // undecided until the game submits its service request
    uint64_t timeline=0;
    unsigned serviceFrame=~0u;
    unsigned nextRingFrame=0;
    uint64_t serviceTicket=0,peerSessionTicket=0;
    Json pending={{"op","join"}};
    // Shared XBAND behavior, not a game experiment. Explicit 0 is a diagnostic
    // opt-out; the old variable remains an alias for existing launch scripts.
    const bool standbyExperiment=[] {
        const char* value=std::getenv("YMIR_XBAND_STANDBY_READONLY");
        if(!value)value=std::getenv("YMIR_XBAND_VF_STANDBY_READONLY_EXPERIMENT");
        return !value||std::string_view(value)!="0";
    }();
    XOSStandbyObserver standbyObserver;
    PeerDialGate peerDialGate;
    std::optional<Json> standbyNotice;
    std::vector<uint8_t> peerTX;
    PeerUartTiming peerUart;
    AsyncModemReceive asyncReceive;
    uint64_t asyncPollDue=0,asyncSequence=0;
    // Preview opt-in. Legacy lockstep remains available for comparison.
    const bool asynchronous=traceEnabled("YMIR_XBAND_ASYNC_MODE");
    const bool deadlineTiming=traceEnabled("YMIR_XBAND_UART_DEADLINE");
    static constexpr uint64_t asyncPollCycles=32768;
    // Catalog replay compatibility profile; physical clock remains unverified.
    static constexpr uint64_t peerByteCycles=18750,peerTimeoutCycles=75000;
    std::string status="Disabled";
    // Opt-in investigation only; no payload or command text is recorded.
    static bool traceEnabled(const char *name){const char *value=std::getenv(name);return value&&std::string_view(value)=="1";}
    const bool trace=traceEnabled("YMIR_XBAND_FRONTEND_TRACE");
    const bool peerTrace=traceEnabled("YMIR_XBAND_PEER_TRACE");
    uint64_t idReads=0,uartReads=0,uartWrites=0,commands=0;
    unsigned traceFrame=~0u;
    uint64_t peerSent=0,peerReceived=0;
    void tracePeer(const char *direction,uint64_t index,uint8_t byte){
        if(peerTrace&&index<512){
            std::fprintf(stderr,"XBAND_PEER side=%d direction=%s index=%llu byte=%02x cycle=%llu\n",config.side,direction,
                static_cast<unsigned long long>(index),unsigned(byte),static_cast<unsigned long long>(elapsed));
            std::fflush(stderr);
        }
    }
    void traceState(){
        if(!trace||traceFrame==frame||frame%300)return;
        traceFrame=frame;
        std::fprintf(stderr,"XBAND_FRONTEND side=%d frame=%u enabled=%d id_reads=%llu uart_reads=%llu uart_writes=%llu commands=%llu carrier=%d tx=%llu rx=%llu status=%s\n",
            config.side,frame,int(enabled),static_cast<unsigned long long>(idReads),
            static_cast<unsigned long long>(uartReads),static_cast<unsigned long long>(uartWrites),
            static_cast<unsigned long long>(commands),int(peer||(session&&session->carrier())),
            static_cast<unsigned long long>(sent),static_cast<unsigned long long>(received),status.c_str());
        std::fflush(stderr);
    }
    static std::string digits(std::string_view s){std::string out;for(char c:s)if(c!='-')out+=c;return out;}
    bool reply(std::string_view text){if(bank.rx.size()+text.size()>256)return false;for(auto c:text)bank.rx.push_back(uint8_t(c));return true;}
    LocalDialRole dialRole(std::string_view number){
        if(standbyExperiment && saturn){
            auto& cpu=saturn->masterSH2.GetProbe();
            if(xosPhoneCheckActive(number,cpu.R(15),
                [&](uint32_t a){return cpu.MemPeekWord(a,true);},
                [&](uint32_t a){return cpu.MemPeekLong(a,true);},
                [&](uint32_t a){return cpu.MemPeekByte(a,true);}))
                return LocalDialRole::selfCheck;
        }
        return localDialRole(number,config.side,serviceBegun);
    }
    void publish(){std::lock_guard lock(mutex);published={enabled,peer||(session&&session->carrier()),frame,sent,received,status};}
    void stop(const char *reason){
        enabled=false;failed=false;peer=false;clockReady=false;needStep=false;sample=false;
        session.reset();service.reset();control.reset();buffer.reset();peerTX.clear();bank.rx.clear();
        peerUart.reset();asyncReceive.reset();asyncPollDue=asyncSequence=0;
        standbyObserver.reset();standbyNotice.reset();
        peerDialGate.reset();
        if(board)board->setEnabled(false);status=reason;publish();
    }
    void fail(const char *reason){
        std::fprintf(stderr,"XBAND_MODEM_FAILURE side=%d reason=%s\n",config.side,reason);
        std::fflush(stderr);stop(reason);failed=true;
    }
    void start(Config c){
        stop("Starting");config=std::move(c);
        if(!config.enabled)return;
        if(config.side<0||config.side>1||config.port<1||config.port>65530)throw std::invalid_argument("Invalid endpoint or port");
        const auto expected=config.side?"3336666665":"3336666666";
        if(digits(config.phone)!=expected)throw std::invalid_argument("Preview server requires phone 3336666666 (1) or 3336666665 (2)");
        bank.resetUART();frame=0;sent=received=controlTick=generation=elapsed=end=0;
        idReads=uartReads=uartWrites=commands=0;traceFrame=~0u;
        peerSent=peerReceived=0;
        serviceBegun=false;caller=2;peerEnding=peerClosed=peerDialing=false;
        peerSessionTicket=0;
        nextRingFrame=0;
        closeService=serviceBatch=controlBusy=joined=ring=answering=timelineKnown=false;serviceFrame=~0u;pending={{"op","join"}};
        if(asynchronous)pending["transport"]="async-v1";
        if(standbyExperiment)pending["standby_protocol"]="xband-readonly-v1";
        const auto now=GetTickCount64();
        auto connection=[&](unsigned offset,std::string name){return std::make_unique<Client>(windows::ClientConfig{
            config.address,uint16_t(config.port+offset+config.side),config.allowLAN,name+std::to_string(config.side),std::string(64,'a'),60},now);};
        service=connection(0,"pb3-");control=connection(4,"call-");
        ModemCommandSession::Hooks hooks;
        if(trace)hooks.observedCommand=[this](std::string_view){++commands;};
        hooks.reply=[this](std::string_view s){return reply(s);};
        hooks.payload=[this](uint8_t b){if(peer)return peerUart.push(b);++sent;return buffer.transmit(b);};
        hooks.clearReceive=[this]{bank.rx.clear();};
        hooks.hangup=[this]{
            if(peerDialGate.active()){
                peerDialGate.reset();peerDialing=false;
            }else if(peer||peerDialing){
                peerEnding=true;peer=false;peerDialing=false;peerTX.clear();peerUart.reset();asyncReceive.reset();clockReady=false;
                pending={{"op","hangup"},{"generation",generation}};status="Peer call closing";
            }
            buffer.reset();closeService=true;
        };
        hooks.dial=[this](std::string_view number){
            const auto role=dialRole(number);
            if(role==LocalDialRole::selfCheck){
                status="Local phone setting check (BUSY)";
                if(trace)std::fprintf(stderr,"XBAND_PHONE_SELF_CHECK side=%d frame=%u after_service=%d response=BUSY\n",config.side,frame,int(serviceBegun));
                return ModemCommandSession::Dial::busy;
            }
            if(role!=LocalDialRole::service||!buffer.dial(number))return ModemCommandSession::Dial::rejected;
            serviceBegun=true;
            return ModemCommandSession::Dial::pending;
        };
        hooks.intercept=[this](const std::string &command){
            const auto decoded=decodeObservedAT(command,true);
            if(decoded.kind==ATKind::dial&&dialRole(decoded.number)==LocalDialRole::peer){
                if(peerClosed||peerEnding){reply("\r\nNO CARRIER\r\n");return true;}
                if(standbyExperiment){
                    // Service reply and call-control polling are independent.
                    // The original ATD can precede our next role snapshot.
                    if(!generation){reply("\r\nNO CARRIER\r\n");return true;}
                    session->beginPeerDial();peerSessionTicket=session->ticket();peerDialing=true;
                    peerDialGate.arm(generation,frame);status="Waiting for peer route confirmation";
                    if(trace)std::fprintf(stderr,"XBAND_PEER_DIAL_GATE side=%d frame=%u caller=%u joined=%d control_busy=%d pending=%s\n",
                        config.side,frame,caller,int(joined),int(controlBusy),pending.at("op").get<std::string>().c_str());
                    return true;
                }
                if(unsigned(config.side)!=caller||!joined||pending.at("op")!="poll")throw std::runtime_error("Peer route not ready");
                // Keep the internal protocol route separate from the entered number.
                session->beginPeerDial();peerSessionTicket=session->ticket();peerDialing=true;pending={{"op","dial"},{"generation",generation},{"number",config.side?"3336666666":"3336666665"}};status="Dialing paired modem (number independent)";return true;
            }
            if(decoded.kind==ATKind::answer){
                if(!ring||unsigned(config.side)==caller)throw std::runtime_error("Answer without incoming call");
                session->beginPeerDial();peerSessionTicket=session->ticket();peerDialing=true;pending={{"op","answer"},{"generation",generation}};ring=false;answering=true;return true;
            }
            return false;
        };
        session=std::make_unique<ModemCommandSession>(std::move(hooks));
        enabled=true;board->setEnabled(true);status="Connecting to server";publish();
    }
    uint8_t interruptCode()const{return peer?peerUart.interrupt(bank.ier,bank.fcr,bank.rx.size(),elapsed,peerTimeoutCycles):
        ((bank.ier&1)&&!bank.rx.empty()?4:1);}
    void tickPeer(){if(peer){peerUart.tick(elapsed,peerByteCycles,[this](uint8_t b){
        if(peerTX.size()==256)return false;
        peerTX.push_back(b);++sent;tracePeer("tx",peerSent++,b);return true;
    });if(asynchronous)asyncReceive.tick(elapsed,peerByteCycles,[this](uint8_t b){
        if(bank.rx.size()==256)return false;
        bank.rx.push_back(b);++received;peerUart.activity(elapsed);tracePeer("rx",peerReceived++,b);return true;
    });}}
    uint8_t uartRead(uint32_t a){if(trace)++uartReads;tickPeer();return bank.readRegister(a,true,interruptCode(),
        peer||(session&&session->carrier()),peer?peerUart.txStatus():0x60,[this](uint8_t){if(peer)peerUart.activity(elapsed);});}
    void uartWrite(uint32_t a,uint8_t value){
        if(trace)++uartWrites;
        tickPeer();
        bank.writeRegister(a,value,true,[this](uint8_t b){
            session->feed(b,frame);
        },[this]{if(peer)peerUart.activity(elapsed);},[this]{if(peer)peerUart.clearHolding();});
        tickPeer();
    }
    void attach(ymir::Saturn &s){
        if(board)throw std::logic_error("Modem already attached");saturn=&s;
        BoardAttachment::Hooks hooks;
        if(trace)hooks.observedRead=[this](uint32_t a,unsigned width,uint32_t){if(a==0x05885029&&width==1)++idReads;};
        hooks.mode=[] {return ModemRegisterBank::BoardMode{true,true,true,true};};
        hooks.uartRead=[this](uint32_t a){try{return uartRead(a);}catch(const std::exception&e){fail(e.what());return uint8_t(0);}};
        hooks.uartWrite=[this](uint32_t a,uint8_t v){try{uartWrite(a,v);}catch(const std::exception&e){fail(e.what());}};
        board=std::make_unique<BoardAttachment>(s.mainBus,bank,std::move(hooks));board->setEnabled(false);
    }
    void pollControl(uint64_t now){
        if(control->state()==Client::State::idle){if(!control->open(config.phone,now))throw std::runtime_error("Control open rejected");}
        if(controlBusy&&control->readyToRun()){
            std::string response;control->drain([&](uint8_t b){if(response.size()==4096)return false;response+=char(b);return true;},4096);
            if(control->pending())throw std::runtime_error("Control response too large");
            auto r=Json::parse(response);const auto token=r.at("generation").get<uint64_t>();
            if(generation&&generation!=token){
                if(!peerClosed||token!=generation+1||r.value("closed",false))throw std::runtime_error("Unexpected peer generation change");
                peer=false;peerEnding=peerClosed=peerDialing=false;answering=ring=false;clockReady=needStep=sample=false;
                caller=2;peerTX.clear();peerUart.reset();asyncReceive.reset();asyncPollDue=asyncSequence=0;
                peerSessionTicket=0;
                elapsed=end=origin=0;nextRingFrame=0;peerSent=peerReceived=0;
                status="Server connected; ready for next match";
            }
            const auto nextCaller=r.value("caller",0u);
            if(nextCaller>2 || (caller<2&&nextCaller!=caller))throw std::runtime_error("Invalid or changed caller role");
            caller=nextCaller;
            if(asynchronous&&r.value("transport",std::string{})!="async-v1")throw std::runtime_error("Asynchronous modem requires matching server");
            generation=token;
            const auto gateResult=peerDialGate.observe(generation,frame,unsigned(config.side),caller,
                r.at("joined").get<std::array<bool,2>>()[0]&&r.at("joined").get<std::array<bool,2>>()[1],
                pending.at("op")=="poll",r.value("closed",false));
            if(gateResult==PeerDialGate::Result::reject){
                peerDialing=false;session->disconnectedIfCurrent(peerSessionTicket,frame);
                status="Peer route rejected; no carrier";
                if(trace)std::fprintf(stderr,"XBAND_PEER_DIAL_REJECT side=%d frame=%u caller=%u\n",config.side,frame,caller);
            }else if(gateResult==PeerDialGate::Result::ready){
                pending={{"op","dial"},{"generation",generation},{"number",config.side?"3336666666":"3336666665"}};
                status="Dialing paired modem (confirmed route)";
                if(trace)std::fprintf(stderr,"XBAND_PEER_DIAL_CONFIRMED side=%d frame=%u generation=%llu\n",config.side,frame,static_cast<unsigned long long>(generation));
            }
            if(standbyExperiment&&r.contains("standby")){
                const auto& observation=r.at("standby");
                const auto ticket=observation.at("ticket").get<uint64_t>();
                if(observation.at("protocol")!="xband-readonly-v1"&&
                   observation.at("protocol")!="vf-readonly-v1")
                    throw std::runtime_error("Unsupported standby protocol");
                const bool localCaller=caller==unsigned(config.side);
                const auto receiverTicket=localCaller?uint64_t{0}:ticket;
                if(standbyObserver.generation()!=generation||standbyObserver.ticket()!=receiverTicket)standbyNotice.reset();
                standbyObserver.arm(generation,ticket,localCaller);
            }
            const auto members=r.at("joined").get<std::array<bool,2>>();joined=members[0]&&members[1];
            if(status=="Connecting to server"||status.starts_with("Server connected;"))
                status=joined?"Server connected; waiting for guest modem commands":"Server connected; waiting for second modem";
            const auto state=r.at("state").get<unsigned>();
            if(state>2)throw std::runtime_error("Invalid peer state");
            if(r.value("closed",false)){
                if(!peerClosed){peer=false;peerEnding=false;peerClosed=true;peerDialing=false;answering=ring=false;clockReady=false;
                    peerTX.clear();peerUart.reset();asyncReceive.reset();
                    if(peerSessionTicket)session->disconnectedIfCurrent(peerSessionTicket,frame);
                    status="Peer disconnected; preparing next match";}
                controlBusy=false;
                pending=r.value("recoverable",false)?Json{{"op","closed_ack"},{"generation",generation}}:Json{{"op","poll"}};
                if(!r.value("recoverable",false))status="Peer transport lost; restart pair required";
                return;
            }
            // A pending remote call must survive receiver initialization and FIFO
            // clears. Do not interleave RING with an AT reply/partial command.
            // Repeat on guest time until original ATA; never synthesize an answer.
            if(state==1&&caller<2&&unsigned(config.side)!=caller&&!answering&&!peer&&!session->carrier()&&
               session->state()==ModemCommandSession::State::command&&session->command().empty()&&
               bank.rx.empty()&&(!ring||frame>=nextRingFrame)){
                if(!reply("\r\nRING\r\n"))throw std::runtime_error("RING overflow");
                ring=true;nextRingFrame=frame+120;status="Incoming call";
                if(trace)std::fprintf(stderr,"XBAND_RING side=%d frame=%u generation=%llu\n",config.side,frame,
                    static_cast<unsigned long long>(generation));
            }
            if(state==2&&!peer&&!peerEnding&&!peerClosed){
                if(session->carrier())throw std::runtime_error("Service carrier active at peer handoff");
                bank.rx.clear();peerUart.reset();peer=true;clockReady=false;
                if(!session->connected(peerSessionTicket,frame))throw std::runtime_error("Peer CONNECT without pending dial/answer");
                if(standbyExperiment){standbyObserver.peerConnected();standbyNotice.reset();}
                peerDialing=false;answering=false;status=asynchronous?"Peer connected (asynchronous modem)":"Peer connected";
            }
            const auto bytes=r.at("bytes").get<std::vector<unsigned>>();
            if(asynchronous){
                std::vector<uint8_t> rx;for(auto b:bytes){if(b>255)throw std::runtime_error("Invalid peer byte");rx.push_back(uint8_t(b));}
                asyncReceive.accept(rx,elapsed,peerByteCycles);
            }else{
                if(bank.rx.size()+bytes.size()>256)throw std::runtime_error("Peer RX full");
                for(auto b:bytes){if(b>255)throw std::runtime_error("Invalid peer byte");bank.rx.push_back(uint8_t(b));++received;tracePeer("rx",peerReceived++,uint8_t(b));}
                if(peer&&!bytes.empty())peerUart.activity(elapsed);
                if(peer&&clockReady){end=r.at("grant_end").get<uint64_t>();if(end<elapsed||end-elapsed>4096)throw std::runtime_error("Invalid cycle grant");}
            }
            controlBusy=false;
        }
        if(control->state()!=Client::State::connected||controlBusy)return;
        if(!peer&&pending.at("op")=="poll"&&standbyNotice){pending=*standbyNotice;standbyNotice.reset();}
        if(peer){
            if(!clockReady||(!asynchronous&&sample))return;
            if(asynchronous){
                if(asyncSequence==UINT64_MAX)throw std::runtime_error("Async sequence exhausted");
                pending={{"op","exchange"},{"generation",generation},{"elapsed",elapsed},{"sequence",++asyncSequence},{"credit",asyncReceive.credit()},{"bytes",peerTX}};
            }else{
                if(!needStep&&end>elapsed)return;
                pending={{"op","step"},{"generation",generation},{"elapsed",elapsed},{"bytes",peerTX}};
            }
        }
        const auto wire=pending.dump();
        if(!control->transfer(std::span(reinterpret_cast<const uint8_t*>(wire.data()),wire.size()),++controlTick))throw std::runtime_error("Control transfer rejected");
        pending={{"op","poll"}};controlBusy=true;if(peer){peerTX.clear();needStep=false;}
    }
    void observeStandby(){
        if(!standbyExperiment||!enabled||!standbyObserver.ticket()||standbyNotice||peer||peerDialing||answering||!session)return;
        auto& cpu=saturn->masterSH2.GetProbe();
        // Only on emulator owner thread, between frames. MemPeek reads bypass
        // device side effects. Shared XOS ROM/RAM addresses, never writes.
        // Verify code before using this layout; no game ID allowlist.
        const bool verified=cpu.MemPeekWord(0x0602b164,true)==0x2f86&&
            cpu.MemPeekWord(0x060943ac,true)==0x2fe6&&
            cpu.MemPeekLong(0x06094378,true)==0x060ba5bc;
        const XOSStandbyObserver::Sample observation{cpu.MemPeekLong(0x060b59e4,true),
            cpu.MemPeekLong(0x060ba5b8,true),cpu.MemPeekLong(0x060ba5bc,true),
            cpu.MemPeekWord(0x060ba5b4,true)==1,cpu.MemPeekByte(0x060b732c,true)!=0,
            session->carrier(),false,verified};
        const auto notice=standbyObserver.observe(observation);
        if(notice!=XOSStandbyObserver::Notice::none){
            standbyNotice=Json{{"op",notice==XOSStandbyObserver::Notice::ready?"standby_ready":"standby_cancel"},
                {"generation",standbyObserver.generation()},{"ticket",standbyObserver.ticket()}};
            std::fprintf(stderr,"XBAND_STANDBY_NOTICE side=%d frame=%u notice=%s ticket=%llu clock=%u start=%u duration=%u active=%d monitor=%d\n",config.side,frame,
                notice==XOSStandbyObserver::Notice::ready?"ready":"cancel",static_cast<unsigned long long>(standbyObserver.ticket()),
                observation.clock,observation.start,observation.duration,int(observation.active),int(observation.monitor));
        }
    }
    bool pump(){
        try{
            std::optional<Config> next;{std::lock_guard lock(mutex);next=std::move(requested);requested.reset();}
            if(next)start(std::move(*next));
            if(!enabled)return true;
            const auto now=GetTickCount64();
            const bool serviceOK=service->step(now),controlOK=control->step(now);
            if(!serviceOK||!controlOK){
                std::fprintf(stderr,"XBAND_TRANSPORT_FAILURE side=%d frame=%u service_ok=%d control_ok=%d service_reason=%d control_reason=%d service_failure=%d service_state=%d control_failure=%d control_state=%d\n",
                    config.side,frame,int(serviceOK),int(controlOK),int(service->transportFailure()),int(control->transportFailure()),
                    int(service->failure()),int(service->failureState()),int(control->failure()),int(control->failureState()));
                // A local protocol/deadline fault is not proof the server died.
                // Preserve which channel failed without exposing payloads/keys.
                std::string reason;
                if(!serviceOK)reason="Service channel failed: "+std::string(service->failureName());
                if(!controlOK){if(!reason.empty())reason+="; ";reason+="Control channel failed: "+std::string(control->failureName());}
                reason+="; reconnect required";
                throw std::runtime_error(reason);
            }
            if(closeService){
                if(!service->requestClose(now))throw std::runtime_error("Service close failed; reconnect required");
                serviceBatch=false;
                closeService=false;
            }
            if(buffer.state()==ModemServiceBuffer::State::dialing){
                if(service->state()==Client::State::idle){serviceTicket=session->ticket();if(!service->open(config.phone,now))throw std::runtime_error("Service open rejected");}
                if(service->state()==Client::State::connected){buffer.confirmConnected();if(!session->connected(serviceTicket,frame))throw std::runtime_error("Stale CONNECT");status="Service connected";}
            }
            session->tick(frame);
            // A stalled control channel must not leave the guest dialing forever.
            if(peerDialGate.observe(generation,frame,unsigned(config.side),caller,joined,false,peerClosed||peerEnding)==PeerDialGate::Result::reject){
                peerDialing=false;session->disconnectedIfCurrent(peerSessionTicket,frame);
                status="Peer route confirmation timed out or rejected";
                if(trace)std::fprintf(stderr,"XBAND_PEER_DIAL_REJECT side=%d frame=%u caller=%u\n",config.side,frame,caller);
            }
            if(serviceBatch&&service->readyToRun()){
                service->drain([this](uint8_t b){if(bank.rx.size()==256)return false;bank.rx.push_back(b);++received;return true;},256);
                serviceBatch=false;
            }
            if(!peer&&session->dataMode()&&!serviceBatch&&serviceFrame!=frame){
                if(!service->transfer(buffer.outgoing(),frame))throw std::runtime_error("Service transfer rejected");
                buffer.transmitted();serviceFrame=frame;serviceBatch=true;
            }
            tickPeer();
            pollControl(now);
            if(interruptCode()!=1)saturn->SCU.TriggerExternalInterrupt(12);
            publish();
            traceState();
            if(peer)return asynchronous||!clockReady||sample||(!controlBusy&&end>elapsed);
            return !serviceBatch;
        }catch(const std::exception &e){fail(e.what());return true;}
    }
    uint64_t budget(uint64_t cycle,uint64_t rev)noexcept{
        if(!enabled)return ~uint64_t{0};
        if(!timelineKnown){timelineKnown=true;timeline=rev;}
        else if(timeline!=rev){fail("Guest reset/load-state: restart server pair and reconnect");return ~uint64_t{0};}
        if(!peer)return ~uint64_t{0};
        if(!clockReady){clockReady=true;origin=cycle;revision=rev;elapsed=end=0;needStep=true;return 0;}
        if(rev!=revision||cycle<origin||cycle-origin<elapsed){fail("Guest timeline changed; reconnect required");return ~uint64_t{0};}
        elapsed=cycle-origin;
        if(asynchronous){
            try{
                tickPeer();if(interruptCode()!=1)saturn->SCU.TriggerExternalInterrupt(12);
                if(elapsed>=asyncPollDue){asyncPollDue=elapsed+asyncPollCycles;return 0;}
                // Local I/O servicing only: no request completion or peer clock wait.
                  if(!deadlineTiming)return std::min<uint64_t>(256,asyncPollDue-elapsed);
                return std::min({asyncPollDue-elapsed,
                    peerUart.cyclesUntilEvent(elapsed,bank.ier,bank.fcr,bank.rx.size(),peerTimeoutCycles),
                    asyncReceive.cyclesUntilEvent(elapsed)});
            }catch(const std::exception&e){fail(e.what());return ~uint64_t{0};}
        }
        if(elapsed>end&&elapsed-end>64){fail("Guest exceeded peer grant");return ~uint64_t{0};}
        if(sample){sample=false;needStep=true;return 0;}
        if(elapsed>=end){needStep=true;return 0;}
        return end-elapsed;
    }
};
FrontendModem::FrontendModem():impl(std::make_unique<Impl>()){}
FrontendModem::~FrontendModem()=default;
void FrontendModem::attach(ymir::Saturn&s){impl->attach(s);}
void FrontendModem::configureStorage(const std::filesystem::path &path){
    if(impl->storage||impl->enabled)throw std::runtime_error("Configure modem storage only once before connecting");
    auto storage=std::make_unique<FlashStorage>(path);
    if(storage->loaded())impl->bank.flashData=*storage->loaded();
    impl->storage=std::move(storage);
}
void FrontendModem::request(Config c){std::lock_guard lock(impl->mutex);impl->requested=std::move(c);}
FrontendModem::Snapshot FrontendModem::snapshot()const{std::lock_guard lock(impl->mutex);return impl->published;}
bool FrontendModem::hasPendingRequest()const{std::lock_guard lock(impl->mutex);return impl->requested.has_value();}
bool FrontendModem::pump(){return impl->pump();}
void FrontendModem::waitForActivity(){
    if(!impl->enabled||!impl->service||!impl->control)return;
    fd_set readable,writable;FD_ZERO(&readable);FD_ZERO(&writable);
    const bool serviceWait=impl->service->appendWaitSockets(readable,writable);
    const bool controlWait=impl->control->appendWaitSockets(readable,writable);
    if(serviceWait&&controlWait&&(readable.fd_count||writable.fd_count)){
        timeval timeout{0,1000};
        if(select(0,&readable,&writable,nullptr,&timeout)==SOCKET_ERROR)
            impl->fail("Modem socket readiness wait failed");
    }
}
void FrontendModem::frameCompleted(){
    if(impl->enabled){++impl->frame;if(impl->peer)impl->sample=true;}
    impl->observeStandby();
    // Owner thread only, after completed guest frames/pages. No mapped reads.
    if(++impl->storageFrames>=120){impl->storageFrames=0;impl->saveFlash();}
}
uint64_t FrontendModem::budget(uint64_t c,uint64_t r)noexcept{return impl->budget(c,r);}
void FrontendModem::reset(const char*r){
    // A queued UI connect must not resurrect a pre-rewind network session.
    {std::lock_guard lock(impl->mutex);impl->requested.reset();}
    if(impl->enabled)impl->stop(r);
}
void FrontendModem::shutdown(){impl->saveFlash();impl->stop("Disabled");}
void FrontendModem::dumpFlash(std::ostream &out) const {
    impl->bank.dumpFlash(out);
}
}
