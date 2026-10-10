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
#include <xband/virtual_media_card.hpp>
#include <xband/media_card_control.hpp>
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
    std::optional<bool> requestedCardInsertion;
    std::optional<bool> requestedCardReadFault;
    std::optional<std::filesystem::path> requestedCardImage;
    Snapshot published;
    Config config;
    ymir::Saturn *saturn=nullptr;
    ModemRegisterBank bank;
    std::unique_ptr<FlashStorage> storage;
    std::unique_ptr<VirtualCardStorage> cardStorage;
    VirtualMediaCard card;
    bool cardSaveFailed=false;
    std::string cardCommandToken;
    std::string cardCommandError;
    std::optional<unsigned> cardReinsertFrame;
    bool canReplaceCard()const{
        return !cardSaveFailed&&!cardReinsertFrame;
    }
    void replaceCard(const std::filesystem::path& path){
        if(!canReplaceCard())throw std::runtime_error("Card persistence failed or replacement is pending");
        saveCard();
        auto next=std::make_unique<VirtualCardStorage>(path);
        if(!next->loaded())throw std::runtime_error("Existing13-byte card required");
        VirtualMediaCard candidate;candidate.data=*next->loaded();
        if(!candidate.remainingUnits())throw std::runtime_error("Unreadable replacement card");
        const bool inserted=card.present;
        card.setInserted(false);card=candidate;cardStorage=std::move(next);
        // Keep the modem/call powered and expose an actual card-OFF interval
        // to the guest before reinserting the new card on a later frame.
        if(inserted)cardReinsertFrame=frame;
        cardCommandError.clear();publish();
    }
    Json cardReport()const{
        const auto units=cardStorage&&!cardSaveFailed?card.remainingUnits():std::nullopt;
        return {{"version",1},{"configured",bool(cardStorage)},{"inserted",card.present},
            {"save_failed",cardSaveFailed},{"read_fault",card.readFault},{"units",units?Json(*units):Json(nullptr)},{"command_token",cardCommandToken},
            {"replace_supported",true},{"can_replace",canReplaceCard()},{"command_error",cardCommandError}};
    }
    void setCardInsertion(bool inserted){
        if(!cardStorage||cardSaveFailed)throw std::runtime_error("Virtual card unavailable");
        saveCard(); // Flush changes before removal; same path as the local UI.
        cardReinsertFrame.reset();if(card.present!=inserted)card.setInserted(inserted);
        publish();
    }
    void setCardReadFault(bool fault){
        if(!cardStorage||cardSaveFailed)throw std::runtime_error("Virtual card unavailable");
        saveCard();card.setReadFault(fault);publish();
    }
    void loadCard(const std::filesystem::path &path){
        if(cardStorage||enabled)throw std::runtime_error("Load virtual card once while modem is disconnected");
        auto storage=std::make_unique<VirtualCardStorage>(path);
        if(!storage->loaded())throw std::runtime_error("Explicit existing13-byte virtual card image required");
        card.data=*storage->loaded();card.setInserted(false);cardStorage=std::move(storage);publish();
    }
    void saveCard(){
        if(cardStorage&&!cardSaveFailed){
            try{cardStorage->save(card.data);}
            catch(...){cardSaveFailed=true;card.setInserted(false);throw;}
        }
    }
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
    bool telephoneLineConnected=true;
    uint64_t serviceFailureSince=0;
    uint64_t controlRetryAt=0;
    bool peerEnding=false,peerClosed=false,peerDialing=false;
    bool resetButtonPressed=false,resetButtonArmed=false,resetBoardProbe=false;
    uint64_t resetButtonTicket=0;
    uint64_t resetButtonSoftResets=0;
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
    void publish(){std::lock_guard lock(mutex);published={enabled,peer||(session&&session->carrier()),frame,sent,received,status,bool(cardStorage),card.present,telephoneLineConnected,cardStorage&&!cardSaveFailed?card.remainingUnits():std::nullopt,cardSaveFailed,card.readFault};}
    std::unique_ptr<Client> serviceConnection(uint64_t now){
        return std::make_unique<Client>(windows::ClientConfig{
            config.address,uint16_t(config.port+config.side),config.allowLAN,"pb3-"+std::to_string(config.side),std::string(64,'a'),60},now);
    }
    void dropTelephoneCall(){
        service.reset();buffer.reset();serviceBatch=closeService=false;
        peer=false;peerDialing=answering=ring=false;clockReady=needStep=sample=false;
        peerTX.clear();peerUart.reset();asyncReceive.reset();peerDialGate.reset();
        standbyObserver.reset();standbyNotice.reset();
        session->disconnected(frame);
    }
    void finishResetButtonCall(){
        if(!resetButtonArmed)return;
        if(!peer||!session||session->ticket()!=resetButtonTicket){
            resetButtonArmed=resetBoardProbe=false;return;
        }
        const bool systemReset=saturn->GetResetDiagnostics().softResets!=resetButtonSoftResets;
        if(!resetBoardProbe&&!systemReset)return;
        resetButtonArmed=resetBoardProbe=false;
        dropTelephoneCall();peerEnding=true;
        // A genuine console reset has a new timeline. Save-state changes do
        // not increment softResets and retain the existing safety behavior.
        if(systemReset)timelineKnown=false;
        pending={{"op","hangup"},{"generation",generation}};
        status="Reset-button guest reinitialization: closing previous peer call";
        if(trace)std::fprintf(stderr,"XBAND_RESET_BUTTON_CLOSE side=%d generation=%llu frame=%u system_reset=%d\n",
            config.side,static_cast<unsigned long long>(generation),frame,int(systemReset));
    }
    void applyTelephoneLine(bool connected,uint64_t now){
        if(telephoneLineConnected==connected)return;
        telephoneLineConnected=connected;serviceFailureSince=0;
        if(!connected){
            // Remove only telephone carriers. Board/flash/card/profile survive.
            service.reset();buffer.reset();serviceBatch=closeService=false;
            peer=false;peerDialing=answering=ring=false;clockReady=needStep=sample=false;
            peerTX.clear();peerUart.reset();asyncReceive.reset();peerDialGate.reset();
            standbyObserver.reset();standbyNotice.reset();
            session->disconnected(frame);
            pending={{"op","poll"}};
            status="電話線が切断されています（OFF）";
        }else{
            service=serviceConnection(now);serviceFrame=~0u;
            status="電話線を接続しました。ゲーム側から再接続してください";
        }
    }
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
        std::fflush(stderr);
        // Even a diagnostic protocol/storage fault is not a physical modem
        // power switch. Disable unsafe network work, retaining board/AT/card.
        if(enabled&&session){dropTelephoneCall();control.reset();status=reason;failed=true;publish();}
        else{stop(reason);failed=true;}
    }
    void start(Config c){
        stop("Starting");config=std::move(c);
        if(!config.enabled)return;
        if(cardSaveFailed)throw std::runtime_error("Virtual card save failed; restart required, no retry");
        if(config.side<0||config.side>1||config.port<1||config.port>65530)throw std::invalid_argument("Invalid endpoint or port");
        const auto expected=config.side?"3336666665":"3336666666";
        if(digits(config.phone)!=expected)throw std::invalid_argument("Preview server requires phone 3336666666 (1) or 3336666665 (2)");
        bank.resetUART();frame=0;sent=received=controlTick=generation=elapsed=end=0;
        idReads=uartReads=uartWrites=commands=0;traceFrame=~0u;
        peerSent=peerReceived=0;
        serviceBegun=false;caller=2;peerEnding=peerClosed=peerDialing=false;
        resetButtonPressed=resetButtonArmed=resetBoardProbe=false;resetButtonTicket=0;
        telephoneLineConnected=true;serviceFailureSince=controlRetryAt=0;
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
            if(failed||!telephoneLineConnected)return ModemCommandSession::Dial::rejected;
            const auto role=dialRole(number);
            if(role==LocalDialRole::selfCheck){
                status="Local phone setting check (BUSY)";
                if(trace)std::fprintf(stderr,"XBAND_PHONE_SELF_CHECK side=%d frame=%u after_service=%d response=BUSY\n",config.side,frame,int(serviceBegun));
                return ModemCommandSession::Dial::busy;
            }
            if(role!=LocalDialRole::service||!buffer.dial(number))return ModemCommandSession::Dial::rejected;
            // New guest ATD only: never replay a failed service batch/debit.
            if(!service)service=serviceConnection(GetTickCount64());
            serviceBegun=true;
            return ModemCommandSession::Dial::pending;
        };
        hooks.intercept=[this](const std::string &command){
            const auto decoded=decodeObservedAT(command,true);
            if((failed||!telephoneLineConnected)&&(decoded.kind==ATKind::dial||decoded.kind==ATKind::answer)){
                reply("\r\nNO CARRIER\r\n");return true;
            }
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
        hooks.overlayRead=[this](uint32_t a,unsigned width,uint32_t value){
            return cardStorage&&a==0x05885025&&width==1?uint32_t(card.read(uint8_t(value))):value;
        };
        hooks.overlayWrite=[this](uint32_t a,unsigned width,uint32_t value){
            if(!cardStorage||a!=0x05885021||width!=1)return false;
            card.write(uint8_t(value));return true;
        };
        hooks.observedRead=[this](uint32_t a,unsigned width,uint32_t){
            if(a==0x05885029&&width==1){
                if(trace)++idReads;
                // A real mapped board-ID read after Reset-button NMI marks
                // guest reinitialization. Debug peeks never call this hook.
                if(resetButtonArmed)resetBoardProbe=true;
            }
        };
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
            if(r.contains("media_card_command")&&!r.at("media_card_command").is_null()){
                const auto& command=r.at("media_card_command");const auto id=MediaCardControl::validateCommand(command);
                if(id!=cardCommandToken){
                    try{
                        if(command.contains("image_path"))replaceCard(std::filesystem::u8path(command.at("image_path").get<std::string>()));
                        else{setCardInsertion(command.at("inserted").get<bool>());
                            if(command.contains("read_fault"))setCardReadFault(command.at("read_fault").get<bool>());}
                        cardCommandError.clear();
                    }catch(const std::exception& e){cardCommandError=e.what();}
                    cardCommandToken=id;
                }
            }
            if(r.contains("telephone_line")){
                const auto lines=r.at("telephone_line").get<std::array<bool,2>>();
                applyTelephoneLine(lines.at(unsigned(config.side)),now);
            }
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
            if(telephoneLineConnected&&state==1&&caller<2&&unsigned(config.side)!=caller&&!answering&&!peer&&!session->carrier()&&
               session->state()==ModemCommandSession::State::command&&session->command().empty()&&
               bank.rx.empty()&&(!ring||frame>=nextRingFrame)){
                if(!reply("\r\nRING\r\n"))throw std::runtime_error("RING overflow");
                ring=true;nextRingFrame=frame+120;status="Incoming call";
                if(trace)std::fprintf(stderr,"XBAND_RING side=%d frame=%u generation=%llu\n",config.side,frame,
                    static_cast<unsigned long long>(generation));
            }
            if(telephoneLineConnected&&state==2&&!peer&&!peerEnding&&!peerClosed){
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
        pending["media_card"]=cardReport();
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
            if(failed){
                std::optional<Config> restart;
                {std::lock_guard lock(mutex);restart=std::move(requested);requested.reset();}
                if(restart)start(std::move(*restart));
                else{if(enabled&&interruptCode()!=1)saturn->SCU.TriggerExternalInterrupt(12);publish();return true;}
            }
            // Persist card changes before releasing any queued service response.
            // No filesystem calls occur from mapped register callbacks.
            saveCard();
            if(cardReinsertFrame&&frame!=*cardReinsertFrame){
                card.setInserted(true);cardReinsertFrame.reset();publish();
            }
            std::optional<bool> insertion;
            std::optional<bool> readFault;
            std::optional<std::filesystem::path> image;
            {std::lock_guard lock(mutex);insertion=requestedCardInsertion;requestedCardInsertion.reset();
             readFault=requestedCardReadFault;requestedCardReadFault.reset();
             image=std::move(requestedCardImage);requestedCardImage.reset();}
            if(image){try{if(cardStorage)replaceCard(*image);else loadCard(*image);cardCommandError.clear();}
                catch(const std::exception& e){cardCommandError=e.what();publish();}}
            if(insertion){
                setCardInsertion(*insertion);
            }
            if(readFault)setCardReadFault(*readFault);
            std::optional<Config> next;{std::lock_guard lock(mutex);next=std::move(requested);requested.reset();}
            if(next)start(std::move(*next));
            if(!enabled)return true;
            finishResetButtonCall();
            const auto now=GetTickCount64();
            if(now<controlRetryAt){session->tick(frame);
                if(interruptCode()!=1)saturn->SCU.TriggerExternalInterrupt(12);
                publish();return true;}
            if(!control->step(now)){
                if(control->failure()!=Client::Failure::transport||
                   (control->transportFailure()!=windows::TcpClient::Failure::io&&
                    control->transportFailure()!=windows::TcpClient::Failure::connect_timeout))
                    throw std::runtime_error("Invalid control transport; reconnect required");
                dropTelephoneCall();generation=0;caller=2;joined=false;peerClosed=peerEnding=false;
                peerSessionTicket=0;elapsed=end=origin=0;nextRingFrame=0;
                peerSent=peerReceived=asyncPollDue=asyncSequence=0;
                controlBusy=false;pending={{"op","join"}};
                if(asynchronous)pending["transport"]="async-v1";
                if(standbyExperiment)pending["standby_protocol"]="xband-readonly-v1";
                control=std::make_unique<Client>(windows::ClientConfig{
                    config.address,uint16_t(config.port+4+config.side),config.allowLAN,"call-"+std::to_string(config.side),std::string(64,'a'),60},now);
                controlRetryAt=now+250;
                status="通信が切断されました。ゲーム画面から再接続してください";
                if(interruptCode()!=1)saturn->SCU.TriggerExternalInterrupt(12);
                publish();return true;
            }
            // Management reports OFF even when the telephone socket has just
            // closed. Never turn a deliberate line cut into modem power-off.
            pollControl(now);
            if(!telephoneLineConnected){
                session->tick(frame);
                if(interruptCode()!=1)saturn->SCU.TriggerExternalInterrupt(12);
                publish();return true;
            }
            if(service&&!service->step(now)){
                if(!serviceFailureSince)serviceFailureSince=now;
                if(now-serviceFailureSince<2000){publish();return true;}
                dropTelephoneCall();serviceFailureSince=0;
                status="通信が切断されました。ゲーム画面から再接続してください";
                if(interruptCode()!=1)saturn->SCU.TriggerExternalInterrupt(12);
                publish();return true;
            }
            serviceFailureSince=0;
            if(!service){closeService=false;session->tick(frame);
                if(interruptCode()!=1)saturn->SCU.TriggerExternalInterrupt(12);
                publish();return true;}
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
            if(interruptCode()!=1)saturn->SCU.TriggerExternalInterrupt(12);
            publish();
            traceState();
            if(peer)return asynchronous||!clockReady||sample||(!controlBusy&&end>elapsed);
            return !serviceBatch;
        }catch(const std::exception &e){fail(e.what());return true;}
    }
    uint64_t budget(uint64_t cycle,uint64_t rev)noexcept{
        if(!enabled||failed)return ~uint64_t{0};
        try{finishResetButtonCall();}catch(const std::exception& e){fail(e.what());return ~uint64_t{0};}
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
void FrontendModem::configureVirtualCard(const std::filesystem::path &path,bool inserted){
    impl->loadCard(path);
    impl->card.setInserted(inserted); // Explicit startup trial only; default remains ejected.
    impl->publish();
}
void FrontendModem::requestCardImage(std::filesystem::path path){
    std::lock_guard lock(impl->mutex);impl->requestedCardImage=std::move(path);
}
void FrontendModem::requestCardInsertion(bool inserted){
    std::lock_guard lock(impl->mutex);impl->requestedCardInsertion=inserted;
}
void FrontendModem::requestCardReadFault(bool fault){
    std::lock_guard lock(impl->mutex);impl->requestedCardReadFault=fault;
}
void FrontendModem::request(Config c){std::lock_guard lock(impl->mutex);impl->requested=std::move(c);}
FrontendModem::Snapshot FrontendModem::snapshot()const{std::lock_guard lock(impl->mutex);return impl->published;}
bool FrontendModem::hasPendingRequest()const{
    std::lock_guard lock(impl->mutex);
    return impl->requested.has_value()||impl->requestedCardInsertion.has_value()||impl->requestedCardReadFault.has_value()||impl->requestedCardImage.has_value();
}
bool FrontendModem::pump(){return impl->pump();}
void FrontendModem::waitForActivity(){
    if(!impl->enabled||!impl->control)return;
    fd_set readable,writable;FD_ZERO(&readable);FD_ZERO(&writable);
    const bool serviceWait=!impl->service||impl->service->appendWaitSockets(readable,writable);
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
    {std::lock_guard lock(impl->mutex);impl->requested.reset();impl->requestedCardInsertion.reset();impl->requestedCardReadFault.reset();impl->requestedCardImage.reset();}
    impl->card.resetPins(); // Never rewind persistent card bytes with guest state.
    if(impl->enabled)impl->stop(r);
}
void FrontendModem::softReset(){
    {std::lock_guard lock(impl->mutex);impl->requested.reset();impl->requestedCardInsertion.reset();impl->requestedCardReadFault.reset();impl->requestedCardImage.reset();}
    impl->card.resetPins(); // Persistent card bytes must not be rewound.
    if(!impl->enabled)return;
    const auto config=impl->config;
    const bool connected=impl->telephoneLineConnected;
    try{
        impl->saveCard();impl->saveFlash();
        // Discard the old UART/session/tickets. Fresh internal registration
        // does not dial, replay a service request, or resume the old match.
        impl->start(config);
        if(!connected)impl->applyTelephoneLine(false,GetTickCount64());
        impl->publish();
    }catch(const std::exception& e){impl->fail(e.what());}
}
void FrontendModem::hardReset(){softReset();}
void FrontendModem::consoleResetButton(bool pressed){
    if(pressed&&!impl->resetButtonPressed&&impl->enabled&&impl->peer&&impl->session){
        impl->resetButtonArmed=true;impl->resetBoardProbe=false;
        impl->resetButtonTicket=impl->session->ticket();
        impl->resetButtonSoftResets=impl->saturn->GetResetDiagnostics().softResets;
        if(impl->trace)std::fprintf(stderr,"XBAND_RESET_BUTTON_ARM side=%d generation=%llu frame=%u\n",
            impl->config.side,static_cast<unsigned long long>(impl->generation),impl->frame);
    }
    impl->resetButtonPressed=pressed;
}
void FrontendModem::shutdown(){
    impl->saveFlash();
    try{impl->saveCard();}catch(const std::exception&e){impl->fail(e.what());}
    impl->stop("Disabled");
}
void FrontendModem::dumpFlash(std::ostream &out) const {
    impl->bank.dumpFlash(out);
}
}
