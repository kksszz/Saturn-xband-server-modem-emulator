#include <xband/windows_tcp_host.hpp>
#include <xband/local_phone_policy.hpp>
#include <xband/local_match_roles.hpp>
#include <xband/standby_registration.hpp>
#include "local_service_endpoint.hpp"
#include "diagnostic_outgoing_request.hpp"
#include <xband/mail_capture_store.hpp>
#include "local_mail_journal.hpp"
#include "activity_history.hpp"
#include "game_point_history.hpp"
#include "game_result_database.hpp"
#ifdef PB3_SERVER_POINT_LEDGER
#include "game_point_ledger.hpp"
#endif
#include <xband/mail_account_routes.hpp>
#include <xband/mailbox_selection.hpp>
#include <xband/mail_profile_names.hpp>
#include <xband/mail_profile_mailbox.hpp>
#include <xband/mail_profile_offers.hpp>
#include "diagnostic_mail_forward.hpp"
#include "diagnostic_mail_date.hpp"
#include "diagnostic_mail_inventory.hpp"
#include "diagnostic_mail_snapshot_file.hpp"
#include <chrono>
#include <iostream>
#include <deque>
#include <cstdlib>
#include "pb3_wait.hpp"
#ifdef PB3_REGION_TABLE
#ifdef PB3_REGION_TOWN
#include "region_town_wire.hpp"
#endif
#include "jp_area_code_table.hpp"
#endif
#ifdef PB3_GAME_RANKING_SETTINGS
#include "game_ranking_settings.hpp"
#ifdef PB3_INTRO_COMPACT_STYLE
#include "intro_title_style.hpp"
#endif
#include "usage_area_settings.hpp"
#ifdef PB3_VF_POINT_LEDGER
#include "vf_point_ledger.hpp"
#endif
#endif
#ifdef PB3_SERVER_LOGIN
#include <xband/login_handshake.hpp>
#endif
#ifdef PB3_PAIR_CONTROL
#include "pb3_pair_control.hpp"
#ifdef PB3_STANDBY_DIALOG
#include "standby_dialog.hpp"
#endif
#endif
#ifdef PB3_SERVER_WINDOW
#ifdef XBAND_FRONTEND_SERVER
#include "xband_dashboard.hpp"
#include "diagnostic_test_progress.hpp"
#else
#include "probe_preview.hpp"
#endif
#include <sstream>
#endif
struct PB3Observation {
    std::string subscriber;
    uint64_t received=0,sent=0;
    bool verified=false;
    bool dummyMailEnabled=false,dummyMailReplyQueued=false;
    uint8_t serviceRequest=0;
    std::string mailCaptureState="disabled";
    uint64_t lastMailCaptureId=0;
    size_t forwardedMailQueued=0;
    size_t inventoryReported=0,inventoryWithheld=0;
    size_t mailMatched=0,mailDeferred=0;
    bool batchClearPrepared=false,snapshotCommitted=false;
};
struct PB3RelayQueues {std::array<std::deque<uint8_t>,2> bytes;std::array<uint64_t,2> sent{},received{};};
class PB3Relay final:public xband::ServiceEndpoint {
    std::shared_ptr<PB3RelayQueues> queues;
    unsigned side;
public:
    PB3Relay(std::shared_ptr<PB3RelayQueues> q,unsigned s):queues(std::move(q)),side(s){}
    bool transmit(uint8_t b,unsigned)override{
        auto &q=queues->bytes[1-side];if(q.size()>=256)return false;q.push_back(b);++queues->sent[side];return true;
    }
    void tick(unsigned)override{}
    bool peek(uint8_t &b)const override{const auto &q=queues->bytes[side];if(q.empty())return false;b=q.front();return true;}
    void consume()override{auto &q=queues->bytes[side];if(q.empty())throw std::runtime_error("empty relay");q.pop_front();++queues->received[side];}
    size_t pending()const override{return queues->bytes[side].size();}
    void reset()override{for(auto &q:queues->bytes)q.clear();}
    ~PB3Relay(){reset();}
};
// Preserves the established PB3 service response profile. Not historical auth.
class PB3Service final:public xband::ServiceEndpoint {
    LocalPPPProbe p;
    LocalServiceEndpoint endpoint{p};
#ifdef PB3_SERVER_LOGIN
    xband::LoginHandshake login{endpoint,1}; // observed PB3 CR -> HELO -> next-frame login
    xband::LoginHandshake::Phase lastPhase=xband::LoginHandshake::Phase::initial;
#endif
    unsigned side;
    std::shared_ptr<xband::LocalMatchRoles> roles;
    std::function<xband::StandbyRegistration::Decision(const std::string&,uint32_t)> standbyRequest;
    std::function<xband::StandbyRegistration::Decision(const std::string&,uint32_t,const std::string&,const std::string&)> namedStandbyRequest;
    std::function<void()> standbyAbort;
    std::function<bool(const std::string&)> namedTargetAccess;
    bool standbyEngaged=false;
#ifdef PB3_STANDBY_DIALOG
    std::optional<uint32_t> standbyWaitSnapshot;
#endif
#ifdef PB3_USAGE_TIME_POLICY
    bool prepareTimeDenied(uint8_t requestCode,bool peerRestricted=false){
        const auto phone=xband::registrationPhone(p.tcp.captured);
        if(standbyEngaged&&standbyAbort)standbyAbort();roles->withdraw(side);standbyEngaged=false;
        p.tcp.replyState1D=p.tcp.replyPeerNumber=p.tcp.replyReceiverCandidate=false;
        p.tcp.servicePolicyReply=diagnostic::activeUsageArea->wire(phone,diagnostic::receivedGameID(p.tcp.captured),LocalTCPProbe::standbyAreaPreference(p.tcp.captured));
        const auto notice=diagnostic::usageTimeDeniedNotice(peerRestricted);
        p.tcp.servicePolicyReply.insert(p.tcp.servicePolicyReply.end(),notice.begin(),notice.end());
        observation->mailCaptureState="time_restricted";
        recordActivity("time_denied",std::string(peerRestricted?"指名した相手":"この端末")+"が日本時間の利用時間外。接続を拒否。メール送信キューの消去・受信応答準備・待機登録は行いません。");
        std::cout<<"XBAND_TIME_DENIED side="<<side<<" service="<<unsigned(requestCode)
            <<" restricted="<<(peerRestricted?"named_peer":"self")<<" timezone=Asia/Tokyo; no mail clear or match role\n"<<std::flush;
        return true;
    }
#endif
    std::shared_ptr<diagnostic::ActivityHistory> activity;
    nlohmann::json activityContext=nlohmann::json::object();
    std::function<uint64_t(const nlohmann::json&)> activityIdentity;
    bool activityRequested=false,activityEnded=false,activityError=false;
    bool awardRecorded=false,pointResultRecorded=false;
#ifdef PB3_GAME_RANKING_SETTINGS
    std::optional<diagnostic::GameMatchAward> matchAwardSnapshot;
#endif
    void recordPoints(const char* event,int configured,std::optional<uint32_t> delta={},std::optional<uint32_t> total={}){
        if(!activity||!activityRequested)return;
        auto row=activityContext;row["side"]=side;row["event"]=event;row["configured_points"]=configured;
        if(delta)row["points_delta"]=*delta;if(total)row["points_total"]=*total;
        row["point_status"]=total?"サーバー台帳記録":"ROM加算未確認";
        row["detail"]=total?"VF専用サーバー台帳の結果。ROMでの加算確認ではありません。":"対戦前の付与値を応答に組み込みました。送信完了・対戦完了・ROM加算の証明ではありません。";
        activity->append(std::move(row));
    }
    void recordActivity(const char* event,const std::string& detail={}){
        if(!activity)return;auto row=activityContext;row["side"]=side;row["event"]=event;row["detail"]=detail;activity->append(std::move(row));
    }
    void recordRequest(uint8_t code,const char* requestType=nullptr){
        if(!activity||activityRequested)return;
        const auto& wire=p.tcp.captured;
        const auto start=xband::registrationOffset(wire,81);std::vector<uint8_t> name;
        for(size_t i=start;i<std::min(wire.size(),start+16)&&wire[i];++i)name.push_back(wire[i]);
        UINT cp=51932;int n=MultiByteToWideChar(cp,0,reinterpret_cast<const char*>(name.data()),int(name.size()),nullptr,0);
        if(!n){cp=20932;n=MultiByteToWideChar(cp,0,reinterpret_cast<const char*>(name.data()),int(name.size()),nullptr,0);}
        std::wstring wide(size_t(n),0);if(n)MultiByteToWideChar(cp,0,reinterpret_cast<const char*>(name.data()),int(name.size()),wide.data(),n);
        std::string display;
        if(n){const auto size=WideCharToMultiByte(CP_UTF8,0,wide.data(),n,nullptr,0,nullptr,nullptr);display.resize(size);WideCharToMultiByte(CP_UTF8,0,wide.data(),n,display.data(),size,nullptr,nullptr);}
        activityContext={{"phone",xband::registrationPhone(wire)},{"profile",wire[xband::registrationOffset(wire,43)]},
            {"name",display},{"request_type",code==2?"指名対戦":code==3?"自動対戦":code==4?"メール":"その他"}};
        if(requestType)activityContext["request_type"]=requestType;
        if(const auto accepts=LocalTCPProbe::standbyAcceptsChallenges(wire)){
            activityContext["accepts_challenges"]=*accepts;
            // Record the verified guest report without changing candidate or
            // native notice policy before refusal semantics are established.
            std::cout<<"MATCH_ACCEPTANCE_REPORT side="<<side<<" accepts="<<*accepts
                     <<" source=registration0B.flags.bit0 inverted; observation only\n"<<std::flush;
        }
        if(auto game=diagnostic::receivedGameID(wire)){
            activityContext["game"]=*game;
#ifdef PB3_GAME_RANKING_SETTINGS
            if(diagnostic::activeGameRankings)for(const auto& row:diagnostic::activeGameRankings->snapshot())if(row.gameID==*game)activityContext["title"]=row.fields[0];
#endif
        }
        if(code==2)if(auto named=LocalTCPProbe::namedRequestBody(wire))activityContext["target"]=named->second;
        if(activityIdentity)activityContext["generation"]=activityIdentity(activityContext);
        activityRequested=true;recordActivity("access");
#ifdef PB3_GAME_RANKING_SETTINGS
        if(diagnostic::activeGameRankings)try{diagnostic::recordGamePointResult(*activity,activityContext,wire,*diagnostic::activeGameRankings);}catch(const std::exception& e){std::cerr<<"POINT_HISTORY_ERROR "<<e.what()<<'\n';}
#ifdef PB3_SERVER_POINT_LEDGER
        if(diagnostic::activeGamePoints&&diagnostic::activeGameRankings){
            diagnostic::activeGamePoints->observe(*activity,activityContext,wire,*diagnostic::activeGameRankings);
            if(code==4||requestType){
                const auto game=diagnostic::receivedGameID(wire);
                if(game)p.tcp.rankingResultReply=diagnostic::activeGamePoints->wire(activityContext.at("phone"),*game,*diagnostic::activeGameRankings);
            }
        }
#endif
#endif
        if(diagnostic::activeGameResults)try{diagnostic::activeGameResults->observe(*activity,activityContext,wire);}catch(const std::exception& e){std::cerr<<"GAME_RESULT_DATABASE_ERROR "<<e.what()<<'\n';}
    }
    std::shared_ptr<diagnostic::LocalMailJournal> localMailJournal;
    bool journaled=false;
    bool journalReplyPrepared=false;
    std::vector<std::string> journalInboxKeys;
    std::optional<xband::StandbyRegistration::Result> lastNamedDecision;
    bool verified=false;
    const bool mailProbe;
    const bool dummyMail;
    std::shared_ptr<xband::MailCaptureStore> mailCaptures;
    std::shared_ptr<xband::MailAccountRoutes> mailRoutes;
    std::shared_ptr<xband::MailProfileNames> profileNames;
    const bool profileMailbox;
    std::shared_ptr<bool> profileRunValid;
    std::shared_ptr<xband::MailProfileOffers> profileOffers;
    const bool profileBatchClear;
    const bool profileSubmissionClear;
    const bool rememberHeld;
    bool incomingExamined=false;
    bool outgoingExamined=false;
    const bool clearAcceptedOutgoing;
    const bool inventoryExperiment;
    bool outgoingClearPrepared=false;
    size_t acceptedOutgoingCount=0;
    std::vector<uint8_t> committedSourcePlayers;
    std::function<void(const xband::MailCaptureStore&)> commitCustody;
    std::function<void()> commitProfileState;
    std::function<bool()> postMatchEligible;
    std::function<void()> markPostMatchServed;
    bool postMatchPrepared=false;
    std::shared_ptr<PB3Observation> observation;
public:
    void setLocalMailJournal(std::shared_ptr<diagnostic::LocalMailJournal> value){localMailJournal=std::move(value);}
    PB3Service(unsigned s,std::shared_ptr<PB3Observation> o,std::shared_ptr<xband::LocalMatchRoles> r,
               bool dummy=false,bool probe=[]{const char *v=std::getenv("XBAND_MAIL_PROBE");return v&&std::string_view(v)=="1";}(),
               std::shared_ptr<xband::MailCaptureStore> captures={},std::shared_ptr<xband::MailAccountRoutes> routes={},bool clearAccepted=false,bool inventory=false,
               std::function<void(const xband::MailCaptureStore&)> commit={},std::shared_ptr<xband::MailProfileNames> profiles={},
               bool receiveProfiles=false,std::shared_ptr<bool> runValid={},std::shared_ptr<xband::MailProfileOffers> offers={},bool batchClear=false,bool submissionClear=false,bool keepHeld=false,
               std::function<void()> commitProfile={},std::function<bool()> postMatch={},std::function<void()> markPostMatch={}):
        side(s),roles(std::move(r)),mailProbe(probe),dummyMail(dummy),mailCaptures(std::move(captures)),mailRoutes(std::move(routes)),profileNames(std::move(profiles)),profileMailbox(receiveProfiles),profileRunValid(std::move(runValid)),profileOffers(std::move(offers)),profileBatchClear(batchClear),profileSubmissionClear(submissionClear),rememberHeld(keepHeld),clearAcceptedOutgoing(clearAccepted),inventoryExperiment(inventory),commitCustody(std::move(commit)),commitProfileState(std::move(commitProfile)),postMatchEligible(std::move(postMatch)),markPostMatchServed(std::move(markPostMatch)),observation(std::move(o)){
        if(rememberHeld&&!profileOffers)throw std::runtime_error("Remembered held policy requires controlled profile inventory");
        if(dummyMail&&mailProbe)throw std::runtime_error("Dummy mail and empty mail probe cannot be combined");
        if(mailRoutes&&(!mailCaptures||dummyMail||mailProbe))throw std::runtime_error("Mail forwarding requires isolated capture mode");
        if(clearAcceptedOutgoing&&(!mailCaptures||dummyMail||mailProbe))throw std::runtime_error("Outgoing clear experiment requires isolated capture mode");
        if(inventoryExperiment&&(!mailRoutes||!clearAcceptedOutgoing))throw std::runtime_error("Inventory experiment requires forwarding and outgoing-clear COW fixture");
        if(profileBatchClear&&profileSubmissionClear)throw std::runtime_error("Choose exactly one profile clear experiment");
        if((profileBatchClear||profileSubmissionClear)&&(!profileNames||!profileMailbox||!profileRunValid||!profileOffers||!commitCustody||clearAcceptedOutgoing||inventoryExperiment))
            throw std::runtime_error("Mixed-profile clear requires controlled profile inventory and committed custody; no legacy clear");
        if(commitCustody&&(!mailCaptures||(!inventoryExperiment&&!profileBatchClear&&!profileSubmissionClear)))throw std::runtime_error("Snapshot experiment requires isolated inventory mode");
        if(profileNames&&(!mailCaptures||mailRoutes||dummyMail||mailProbe||clearAcceptedOutgoing||inventoryExperiment||(commitCustody&&!profileBatchClear&&!profileSubmissionClear)))
            throw std::runtime_error("Profile-name experiment requires fresh-COW capture ONLY; no forwarding, clear or snapshot");
        if(profileMailbox&&!profileNames)throw std::runtime_error("Profile mailbox requires isolated profile experiment");
        if(profileOffers&&(!profileMailbox||!profileRunValid))throw std::runtime_error("Profile inventory requires controlled profile mailbox context");
        *observation={};configure();
    }
    void configure(){
#ifdef PB3_REGION_TOWN
        p.tcp.regionTownReply=[](const std::string& phone,std::optional<uint32_t> game){
            return diagnostic::regionTownWire(*diagnostic::activeRegions,phone,game);
        };
#endif
#ifdef PB3_GAME_RANKING_SETTINGS
        matchAwardSnapshot.reset();
        // Production totals come only from the persisted server ledger.
        // Builds without that ledger keep rankings untouched; never send fixtures.
        p.tcp.gameRankingReply=[this](std::optional<uint32_t> game){
#ifdef PB3_SERVER_POINT_LEDGER
            if(diagnostic::activeGamePoints&&game&&activityRequested&&activityContext.contains("phone"))
                return diagnostic::activeGamePoints->wire(activityContext.at("phone"),*game,*diagnostic::activeGameRankings);
#endif
            return LocalTCPProbe::Bytes{};
        };
        p.tcp.gameIntroTitleReply=[](std::optional<uint32_t> id){
            auto title=diagnostic::activeGameRankings->introTitleWire(id);
#ifdef PB3_INTRO_COMPACT_STYLE
            if(!title.empty()){
                const auto style=diagnostic::introTitleCompactStyleWire(id);
                title.insert(title.begin(),style.begin(),style.end());
                if(!style.empty())std::cout<<"INTRO_TITLE_STYLE opcode=12 resource=0206:0000 font=1500 width=200; VF-only copied-profile experiment\n"<<std::flush;
            }
#endif
            return title;
        };
        p.tcp.gameMatchAwardReply=[this](std::optional<uint32_t> id){
            if(!matchAwardSnapshot)matchAwardSnapshot=diagnostic::activeGameRankings->matchAward(id);
            const auto& award=*matchAwardSnapshot;
            if(award.known&&activity&&activityRequested&&!awardRecorded){
                // Wire and both policies come from one immutable settings snapshot.
                auto row=activityContext;row["side"]=side;row["event"]="points_prepared";
                row["configured_points"]=award.winPoints;row["configured_win_points"]=award.winPoints;row["configured_lose_points"]=award.losePoints;
                row["point_status"]=award.winPoints<0?"サーバー付与なし":"結果待ち・ROM加算未確認";
                row["detail"]=award.winPoints<0?"当該対戦はサーバー付与なし。以前の付与設定を今回の結果に使わないため無効設定を記録。00B4/00B5の上書きはしません。":"勝者・敗者の付与設定を保存。対戦前の00B4/00B5表示は勝者の値。敗者分を含む累積は結果報告後に25で配信。対戦完了の証明ではありません。";
                activity->append(std::move(row));awardRecorded=true;
            }
            return award.wire;
        };
#ifdef PB3_USAGE_AREA_SETTINGS
        p.tcp.usageAreaPreferenceReply=[](const std::string& phone,std::optional<uint32_t> game,std::optional<uint8_t> area){diagnostic::activeUsageArea->observe(phone);return diagnostic::activeUsageArea->wire(phone,game,area);};
#endif
#endif
        p.tcp.levelDisplayFixture=false; // Retired even if an old environment variable is present.
#ifdef PB3_DATE_FIXTURE_TEST
        p.tcp.dateUpdateFixture=diagnostic::parseDateFixture(std::getenv("XBAND_DATE_FIXTURE"));
        if(p.tcp.dateUpdateFixture)
            std::cout<<"XBAND_DATE_FIXTURE enabled=1 raw=07EA9280:00000000; synthetic numeric26/10/5, unknown epoch/scalar meaning; match replies only\n"<<std::flush;
#endif
        postMatchPrepared=false;
        p.tcp.servicePolicyReply.clear();
        journaled=false;
        journalReplyPrepared=false;
        journalInboxKeys.clear();
        lastNamedDecision.reset();
#ifdef PB3_STANDBY_DIALOG
        standbyWaitSnapshot.reset();
#endif
        p.enableIPCP=p.enableDiscovery=p.enableTCP=true;
        p.tcp.replyEnd02=p.tcp.replyState1D=p.tcp.replyPeerNumber=true;
        p.tcp.replyPostMatchEnd02=bool(postMatchEligible);
        p.tcp.onServiceReplySent=[this]{
            if(localMailJournal&&!journalInboxKeys.empty()){
                localMailJournal->markPrepared(journalInboxKeys);
                std::cout<<"LOCAL_MAIL_INBOX_QUEUED side="<<side<<" records="<<journalInboxKeys.size()<<"; prepared_unconfirmed, no server retry, no guest receipt\n"<<std::flush;
                journalInboxKeys.clear();
            }
            if(postMatchPrepared&&markPostMatchServed){markPostMatchServed();postMatchPrepared=false;}
        };
        p.tcp.mailProbe=mailProbe;
        p.tcp.profileMailExperiment=bool(profileNames);
        p.tcp.replyDiagnosticIncomingRecord=false;
        p.tcp.diagnosticMailboxReply.clear();incomingExamined=false;observation->forwardedMailQueued=0;
        p.tcp.matchMailPrefix.clear();
        p.tcp.rankingResultReply.clear();
        p.tcp.standbyDialogReply.clear();
        observation->dummyMailEnabled=dummyMail;
        observation->dummyMailReplyQueued=false;observation->serviceRequest=0;
        outgoingExamined=false;observation->lastMailCaptureId=0;
        outgoingClearPrepared=false;acceptedOutgoingCount=0;
        committedSourcePlayers.clear();observation->batchClearPrepared=false;observation->snapshotCommitted=false;
        observation->inventoryReported=observation->inventoryWithheld=0;
        observation->mailMatched=observation->mailDeferred=0;
        observation->mailCaptureState=mailCaptures?"waiting":"disabled";
        if(mailProbe){p.tcp.replyState1D=false;p.tcp.replyPeerNumber=false;}
        p.tcp.diagnosticPeerNumber=side?"3336666666":"3336666665";
        p.tcp.prepareServiceReply=[this]{
            // Validate the supported registration before assigning any role.
            xband::registrationPhone(p.tcp.captured);
            if(!p.tcp.captured[xband::registrationOffset(p.tcp.captured,81)])
                throw std::runtime_error("PB3 original name empty");
            const auto postMatchCode=diagnostic::observedPostMatchRequest(p.tcp.captured);
#ifdef PB3_VF_POINT_LEDGER
            if(diagnostic::activeVFPoints){
                const auto result=LocalTCPProbe::observedVFResult(p.tcp.captured);
                if(!result.empty()){
                    const auto rows=diagnostic::activeGameRankings->snapshot();
                    const auto row=std::find_if(rows.begin(),rows.end(),[](const auto &r){return r.gameID==0x00010003;});
                    const auto profile=p.tcp.captured[xband::registrationOffset(p.tcp.captured,43)];
                    if(row!=rows.end()&&row->winPoints>=0&&profile<=3){
                        const auto phone=xband::registrationPhone(p.tcp.captured);
                        const bool eligible=postMatchEligible&&postMatchEligible();
                        const auto receipt=diagnostic::activeVFPoints->observe(phone,profile,result,0,row->winPoints,eligible);
                        if(!pointResultRecorded){
                            recordRequest(LocalTCPProbe::serviceRequestCode(p.tcp.captured,bool(profileNames)),"結果報告");
                            const auto requestContext=activityContext;
                            activityContext["game"]=row->gameID;activityContext["title"]=row->fields[0];
                            recordPoints(receipt.duplicate?"points_duplicate":"points_ledger",row->winPoints,receipt.awarded?1u:0u,receipt.points);
                            activityContext=requestContext;
                            pointResultRecorded=true;
                        }
                        // Consume this endpoint's completed-call allowance
                        // only after an atomic new receipt commit. Reconnects
                        // cannot submit altered variants for another award.
                        if(eligible&&!receipt.duplicate&&markPostMatchServed)markPostMatchServed();
                        if(LocalTCPProbe::serviceRequestCode(p.tcp.captured,bool(profileNames))==4)
                            p.tcp.rankingResultReply=p.tcp.gameRankingReply(0x00010003);
                        std::cout<<"VF_POINTS_REPORT side="<<side<<" profile="<<unsigned(profile)
                            <<" eligible="<<eligible<<" awarded="<<receipt.awarded<<" duplicate="<<receipt.duplicate
                            <<" points="<<receipt.points<<"; local custom ledger, unauthenticated registration phone\n"<<std::flush;
                    }
                }
            }
#endif
            // Code 02 is an observed result-only form. Code 03 still selects
            // matchmaking even when the request carries previous-match data.
            if(postMatchCode==2&&postMatchEligible&&postMatchEligible()){
                postMatchPrepared=true;
                observation->serviceRequest=postMatchCode;
                recordRequest(postMatchCode,"対戦後アクセス");
                p.tcp.replyState1D=p.tcp.replyPeerNumber=p.tcp.replyReceiverCandidate=false;
                std::cout<<"XBAND_POSTMATCH_EXPERIMENT side="<<side<<" code="<<unsigned(postMatchCode)
                         <<" bytes="<<p.tcp.captured.size()
                         <<" reply=02; observed VF-only bounded fixture, no result storage or authentication\n"<<std::flush;
                return true;
            }
            // The observed code-03 forms are matchmaking requests, including
            // the result-bearing forms submitted after a completed peer call.
            const auto requestCode=postMatchCode==3&&postMatchEligible&&
                (p.tcp.captured.size()==577||
                 (p.tcp.captured.size()>=592&&p.tcp.captured.size()<=596))
                    ? uint8_t{3}:LocalTCPProbe::serviceRequestCode(p.tcp.captured,bool(profileNames));
            observation->serviceRequest=requestCode;
            recordRequest(requestCode);
#ifdef PB3_USAGE_TIME_POLICY
            if((requestCode==2||requestCode==3||requestCode==4)&&diagnostic::activeUsageArea){
                const auto phone=xband::registrationPhone(p.tcp.captured);
                const auto named=LocalTCPProbe::namedRequestBody(p.tcp.captured);
                if(!diagnostic::activeUsageArea->accessAllowed(phone,requestCode==4))return prepareTimeDenied(requestCode);
                if(requestCode==2&&named&&namedTargetAccess&&!namedTargetAccess(named->second))return prepareTimeDenied(requestCode,true);
            }
#endif
            if(localMailJournal&&(requestCode==2||requestCode==3||requestCode==4)&&!journaled){
                const auto id=localMailJournal->append(p.tcp.captured,side);journaled=true;
                std::cout<<"LOCAL_MAIL_JOURNAL_COMMITTED side="<<side<<" submission="<<id<<" code="<<unsigned(requestCode)<<"; persistent observation, no receipt/clear\n"<<std::flush;
            }
            if(requestCode==4){
                retainOutgoingObservation();
                p.tcp.replyState1D=false;p.tcp.replyPeerNumber=false;p.tcp.replyReceiverCandidate=false;
                p.tcp.replyDiagnosticIncomingRecord=dummyMail;
                prepareForwardedMail();
                prepareJournalMail(false);
                prepareOutgoingClearExperiment();
                std::cout<<(dummyMail?"XBAND_DUMMY_MAIL_SELECTED":"XBAND_MAIL_ONLY")
                         <<" side="<<side<<" bytes="<<p.tcp.captured.size()
                         <<" additional_1d_entry="<<!LocalTCPProbe::completeDiagnosticRequest(p.tcp.captured)
                         <<(outgoingClearPrepared?"; outgoing clear experiment prepared; not delivery acknowledgement\n":observation->forwardedMailQueued?"; local mail response prepared; no match role; not acknowledged\n":dummyMail?"; fixed incoming sample; no match role assigned; not outgoing mail delivery\n":
                                      "; no match role assigned; empty mailbox terminator 02\n")<<std::flush;
                return true;
            }
            if((requestCode!=3&&requestCode!=2)||profileNames)throw std::runtime_error("unsupported XBAND service request code in current mode");
            if(requestCode==2&&!namedStandbyRequest)throw std::runtime_error("Named rival routing requires standby mode");
            p.tcp.replyDiagnosticIncomingRecord=false;
            if(mailProbe){
                const bool additionalEntry=!LocalTCPProbe::completeDiagnosticRequest(p.tcp.captured);
                std::cout<<"MAIL_PROBE_END02 side="<<side<<" bytes="<<p.tcp.captured.size()
                         <<" additional_1d_entry="<<additionalEntry
                         <<"; exchange terminated, no message stored or delivered\n"<<std::flush;
                return true;
            }
            p.tcp.replyState1D=true;
            p.tcp.replyPeerNumber=true;
            if(standbyRequest){
                const auto game=diagnostic::receivedGameID(p.tcp.captured);
                if(!game)throw std::runtime_error("Standby request missing verified game ID");
#ifdef PB3_STANDBY_DIALOG
                const auto preference=LocalTCPProbe::standbyWaitPreference(p.tcp.captured);
                if(!preference)throw std::runtime_error("Standby request missing verified wait preference");
                // Deferred polls belong to one request. A later Save must not
                // change that request's deadline or its matching notice.
                if(!standbyWaitSnapshot)standbyWaitSnapshot=diagnostic::vfStandbyWaitTicks(*preference);
                const auto waitTicks=*standbyWaitSnapshot;
                const auto rows=diagnostic::activeGameRankings->snapshot();
                const auto row=std::find_if(rows.begin(),rows.end(),[&](const auto&r){return r.gameID==*game;});
                if(row==rows.end())throw std::runtime_error("Standby title missing");
                const auto notice=diagnostic::standbyDialogWire(row->fields[0],waitTicks);
#endif
                standbyEngaged=true;
                const auto namePos=xband::registrationOffset(p.tcp.captured,81);
                const auto nameLimit=std::min(p.tcp.captured.size(),namePos+16);
                const auto nameEnd=std::find(p.tcp.captured.begin()+namePos,p.tcp.captured.begin()+nameLimit,0);
                if(nameEnd==p.tcp.captured.begin()+nameLimit)throw std::runtime_error("Unterminated local registration name");
                const std::string name(p.tcp.captured.begin()+namePos,nameEnd);
                const auto named=LocalTCPProbe::namedRequestBody(p.tcp.captured);
                xband::StandbyRegistration::Decision decision;
#ifdef PB3_USAGE_TIME_POLICY
                try{
#endif
                    decision=namedStandbyRequest
                        ?namedStandbyRequest(xband::registrationPhone(p.tcp.captured),*game,name,named?named->second:std::string{})
                        :standbyRequest(xband::registrationPhone(p.tcp.captured),*game);
#ifdef PB3_USAGE_TIME_POLICY
                }catch(const PB3TimeAdmissionDenied&){
                    // The wall-clock window may close between the initial
                    // check and role registration. End normally with 22/02,
                    // not a transport error; never clear the guest outbox.
                    return prepareTimeDenied(requestCode);
                }
#endif
                if(named&&lastNamedDecision!=decision.result){
                    std::cout<<"NAMED_RIVAL_REQUEST side="<<side<<" target="<<named->second
                        <<" result="<<unsigned(decision.result)<<"; local name lookup, not authentication\n"<<std::flush;
                    lastNamedDecision=decision.result;
                }
                if(decision.result==xband::StandbyRegistration::Result::defer)return false;
                p.tcp.replyReceiverCandidate=decision.result==xband::StandbyRegistration::Result::receiver;
                p.tcp.receiverWaitTicks=p.tcp.replyReceiverCandidate?6000:0;
#ifdef PB3_STANDBY_DIALOG
                p.tcp.standbyDialogReply.clear();
                if(p.tcp.replyReceiverCandidate){
                    p.tcp.receiverWaitTicks=waitTicks;
                    // A named wait needs only the original 0E timer. Do not
                    // invent an unavailable-rival message or inject notice62.
                    if(!named)p.tcp.standbyDialogReply=notice;
                    std::cout<<"STANDBY_DIALOG_PREPARED side="<<side<<" opcode="<<(named?"none":"22")<<" bytes="<<p.tcp.standbyDialogReply.size()
                        <<" when=menu min=120 max=300 preference="<<unsigned(*preference)
                        <<" wait_ticks="<<waitTicks<<" approximate_minutes="<<waitTicks/3600
                        <<"; local XOS policy, not original population estimate\n"<<std::flush;
                }
#endif
                std::cout<<"STANDBY_SERVICE_REPLY side="<<side<<" ticket="<<decision.ticket
                    <<" receiver="<<p.tcp.replyReceiverCandidate<<" wait="<<p.tcp.receiverWaitTicks
                    <<"; shared XBAND standby\n"<<std::flush;
                prepareJournalMail(true);
                return true;
            }
            const bool unassigned=roles->caller==2;
            const bool firstRequest=!roles->requested[side];
            const auto caller=roles->accept(side);
            if(firstRequest)std::cout<<"MATCH_REQUEST_READY side="<<side<<" waiting_for_other="<<(caller==2)<<'\n'<<std::flush;
            if(caller==2)return false;
            p.tcp.replyReceiverCandidate=side!=caller;
            p.tcp.receiverWaitTicks=side!=caller?1800:0;
            if(unassigned)std::cout<<"MATCH_ROLES source=complete_service_request caller="<<caller<<" callee="<<roles->callee()<<'\n'<<std::flush;
            prepareJournalMail(true);
            return true;
        };
    }
    void prepareJournalMail(bool match){
        if(!localMailJournal||journalReplyPrepared)return;
        if(!journaled)throw std::runtime_error("Mail response requires persisted whole request");
        const auto inbox=localMailJournal->prepareInbox(p.tcp.captured);
        std::cout<<"LOCAL_MAIL_INBOX_CAPACITY side="<<side<<" profile="<<unsigned(p.tcp.captured[xband::registrationOffset(p.tcp.captured,43)])
                 <<" capacity="<<diagnostic::nativeInboxCapacity<<" reported="<<inbox.reported
                 <<" prepared="<<inbox.keys.size()<<" remaining="<<inbox.remaining<<"; selected user only, overflow retained\n"<<std::flush;
        journalInboxKeys=inbox.keys;
        auto wire=inbox.wire;
        if(match&&!wire.empty())wire.pop_back(); // Remove inbox 02, retain match's final 02.
        if(!diagnostic::decodeObservedOutgoingRequest(p.tcp.captured,false,true).empty()){
            const LocalTCPProbe::Bytes clear{5,0,0,0,4};
            wire.insert(wire.begin(),clear.begin(),clear.end());
            observation->snapshotCommitted=true;observation->batchClearPrepared=true;
            std::cout<<"LOCAL_MAIL_ACCEPTANCE_CLEAR_PREPARED side="<<side<<" command=05 mask=00000004; whole request persisted, server acceptance policy, not delivery receipt\n"<<std::flush;
        }
        observation->forwardedMailQueued=inbox.keys.size();
        if(match)p.tcp.matchMailPrefix=std::move(wire);
        else if(!wire.empty()){
            if(inbox.wire.empty())wire.push_back(2);
            p.tcp.diagnosticMailboxReply=std::move(wire);
        }
        journalReplyPrepared=true;
    }
    void prepareForwardedMail(){
        if(profileMailbox){prepareProfileMailbox();return;}
        if(!mailRoutes||incomingExamined)return;
        incomingExamined=true;
        const auto endpoint="pb3-"+std::to_string(side);
        const auto begin=xband::registrationOffset(p.tcp.captured,81);
        std::vector<uint8_t> name;
        for(size_t i=begin;i<begin+32&&p.tcp.captured[i];++i)name.push_back(p.tcp.captured[i]);
        mailRoutes->observe(endpoint,name);
        const auto words=inventoryExperiment?diagnostic::reportedInventory(p.tcp.captured):std::vector<uint16_t>{};
        const auto heldIds=diagnostic::custodyIdsFromInventory(words);
        auto selection=xband::selectMailbox(*mailCaptures,*mailRoutes,endpoint,heldIds,4);
        auto &pending=selection.prepared;
        observation->mailMatched=selection.matched;
        observation->mailDeferred=selection.deferred;
        if(inventoryExperiment){
            observation->inventoryReported=words.size();
            observation->inventoryWithheld=selection.withheld;
            std::cout<<"XBAND_MAIL_INVENTORY_EXPERIMENT side="<<side<<" reported="<<words.size()<<" withheld="<<observation->inventoryWithheld<<" prepared="<<pending.size()<<" tokens=";
            for(const auto *capture:pending)std::cout<<diagnostic::inventoryToken(*capture)<<',';
            std::cout<<"; synthetic custody-ID inventory, not read/delivery acknowledgement\n"<<std::flush;
        }
        if(pending.empty())return;
        std::vector<diagnostic::IncomingRecord> records;
        for(const auto *capture:pending)records.push_back(inventoryExperiment?diagnostic::incomingWithInventoryToken(*capture):diagnostic::incomingFromCapture(*capture));
        p.tcp.diagnosticMailboxReply=diagnostic::encodeIncomingRecords(records);
        p.tcp.diagnosticMailboxReply.push_back(2);
        observation->forwardedMailQueued=pending.size();
        std::cout<<"XBAND_MAIL_FORWARD_PREPARED side="<<side<<" records="<<pending.size()
                 <<"; exact codename route, synthetic metadata, no delivery acknowledgement\n"<<std::flush;
    }
    void prepareProfileMailbox(){
        if(incomingExamined)return;
        incomingExamined=true;
        if(profileRunValid&&!*profileRunValid)return;
        const auto player=p.tcp.captured[xband::registrationOffset(p.tcp.captured,43)];
        if(player!=0&&player!=3)return;
        const xband::MailProfileNames::Target current{"pb3-"+std::to_string(side),"fresh-COW-server-run",player};
        std::vector<uint64_t> held;
        if(profileOffers){
            const auto words=diagnostic::reportedInventory(p.tcp.captured,true);
            const auto reported=diagnostic::custodyIdsFromInventory(words);
            held=profileOffers->heldFor(current,reported);
            if(rememberHeld){
                const auto now=held.size();held=profileOffers->rememberedHeldFor(current,reported);
                if(commitProfileState)commitProfileState();
                std::cout<<"XBAND_MAIL_PROFILE_HELD_HISTORY side="<<side<<" player="<<unsigned(player)
                         <<" current="<<now<<" remembered="<<held.size()<<"; scoped observed holdings, not delivery/read ACK\n"<<std::flush;
            }
            observation->inventoryReported=words.size();
        }
        const auto selected=xband::selectProfileMailbox(*mailCaptures,*profileNames,current,4,held);
        observation->mailMatched=selected.matched;observation->mailDeferred=selected.deferred;
        observation->inventoryWithheld=selected.withheld;
        if(profileOffers){
            std::cout<<"XBAND_MAIL_PROFILE_INVENTORY side="<<side<<" player="<<unsigned(player)
                     <<" reported="<<observation->inventoryReported<<" validated="<<held.size()
                     <<" withheld="<<selected.withheld<<" prepared="<<selected.prepared.size()
                     <<((profileBatchClear||profileSubmissionClear)?"; previously offered to this profile only, synthetic tokens, committed clear separate, no receipt\n":"; previously offered to this profile only, synthetic tokens, no receipt or clear\n")<<std::flush;
        }
        if(selected.prepared.empty())return;
        std::vector<diagnostic::IncomingRecord> records;
        std::vector<uint64_t> offered;
        for(const auto *capture:selected.prepared){
            records.push_back(profileOffers?diagnostic::incomingWithInventoryToken(*capture):diagnostic::incomingFromCapture(*capture));
            offered.push_back(capture->localId);
        }
        auto reply=diagnostic::encodeIncomingRecords(records);reply.push_back(2);
        if(profileOffers&&!profileOffers->offerBatch(current,offered)){
            std::cout<<"XBAND_MAIL_PROFILE_OFFER_REFUSED side="<<side<<"; no partial offer or mail reply\n"<<std::flush;return;
        }
        if(profileOffers&&commitProfileState)commitProfileState();
        p.tcp.diagnosticMailboxReply=std::move(reply);
        observation->forwardedMailQueued=records.size();
        std::cout<<"XBAND_MAIL_PROFILE_MAILBOX_PREPARED side="<<side<<" player="<<unsigned(player)
                 <<" records="<<records.size()<<" ids=";
        for(const auto *capture:selected.prepared)std::cout<<capture->localId<<',';
        std::cout<<((profileBatchClear||profileSubmissionClear)?"; current profile only, synthetic metadata, committed clear separate, no receipt\n":"; current profile only, synthetic metadata, no receipt or clear\n")<<std::flush;
    }
    void retainOutgoingObservation(){
        if(!mailCaptures||outgoingExamined)return;
        outgoingExamined=true;
        if(profileNames&&profileRunValid&&!*profileRunValid){
            observation->mailCaptureState="profile_epoch_lost";return;
        }
        try{
            const auto records=diagnostic::decodeObservedOutgoingRequest(p.tcp.captured,bool(profileNames));
            const auto endpoint="pb3-"+std::to_string(side);
            const auto nameBegin=xband::registrationOffset(p.tcp.captured,81);
            std::vector<uint8_t> currentName;
            for(size_t i=nameBegin;i<nameBegin+32&&p.tcp.captured[i];++i)currentName.push_back(p.tcp.captured[i]);
            std::vector<xband::MailProfileNames::Name> sourceNames;
            if(profileNames){
                const auto current=p.tcp.captured[xband::registrationOffset(p.tcp.captured,43)];
                if(current!=0&&current!=3)throw std::invalid_argument("Unobserved current profile");
                const auto learned=profileNames->observe(endpoint,"fresh-COW-server-run",current,currentName);
                if(learned==xband::MailProfileNames::Observation::capacity_refused)
                    throw std::invalid_argument("Profile-name capacity refused");
                if(commitProfileState)commitProfileState();
                std::vector<uint8_t> players;
                for(const auto &record:records)players.push_back(record.prefix[0]);
                const auto names=profileNames->resolveBatch(endpoint,"fresh-COW-server-run",players);
                if(!names){
                    observation->mailCaptureState="source_unresolved";
                    std::cout<<"XBAND_MAIL_PROFILE_HOLD side="<<side<<" current="<<unsigned(current)
                             <<" records="<<records.size()<<"; whole request withheld, no ID or clear\n"<<std::flush;
                    return;
                }
                sourceNames=*names;
                std::cout<<"XBAND_MAIL_PROFILE_RESOLVED side="<<side<<" current="<<unsigned(current)
                         <<" records="<<records.size()<<"; observed local names, not authentication\n"<<std::flush;
            }
            // Original mixed-player capture sends all outboxes but only the
            // current profile's codename. Until a per-player name map exists,
            // do not attach that name to a player-0 record sent by another
            // current profile. This restriction authorizes no new identities.
            if(!profileNames&&!records.empty()){
                const auto profile=xband::registrationOffset(p.tcp.captured,43);
                if(profile>=p.tcp.captured.size()||p.tcp.captured[profile]!=0)
                    throw std::invalid_argument("Unverified outgoing source/profile association");
            }
            observation->mailCaptureState=records.empty()?"empty":"pending";
            std::vector<xband::MailCapture> captured;
            const auto acceptedDate=diagnostic::mailAcceptanceDate(std::chrono::system_clock::now());
            for(size_t index=0;index<records.size();++index){
                const auto &record=records[index];
                xband::MailCapture capture;
                capture.serverAcceptedDate=acceptedDate;
                capture.sourceEndpoint=endpoint;
                capture.sourcePhone=xband::registrationPhone(p.tcp.captured);
                capture.sourceCodename=profileNames?sourceNames[index]:currentName;
                capture.wirePrefix=record.prefix;capture.fields=record.fields;capture.wireSuffix=record.suffix;
                captured.push_back(std::move(capture));
            }
            // Stage the complete supported list. Save once before publishing
            // RAM custody; refusal or persistence failure cannot leave a prefix
            // accepted or authorize whole-ROM outbox clear.
            auto candidate=*mailCaptures;
            const auto ids=candidate.retainBatch(captured);
            if(!captured.empty()&&ids.empty()){
                observation->mailCaptureState="capacity_refused";
                std::cout<<"XBAND_MAIL_CAPTURE side="<<side<<" local_id=0 state=capacity_refused; whole request refused, no outbox clear\n"<<std::flush;
                return;
            }
            if(!ids.empty()){
                if(commitCustody){
                    commitCustody(candidate);
                    observation->snapshotCommitted=true;
                    std::cout<<"XBAND_MAIL_SNAPSHOT_COMMITTED side="<<side<<" records="<<candidate.captures().size()<<"; before guest outbox clear\n"<<std::flush;
                }
                *mailCaptures=std::move(candidate);
                if(commitProfileState)commitProfileState();
                acceptedOutgoingCount=ids.size();
                if(profileBatchClear||profileSubmissionClear)for(const auto &record:records)committedSourcePlayers.push_back(record.prefix[0]);
                observation->mailCaptureState="retained";
            }
            for(const auto id:ids){
                observation->lastMailCaptureId=id;
                std::cout<<"XBAND_MAIL_CAPTURE side="<<side<<" local_id="<<id
                         <<" state="<<observation->mailCaptureState
                         <<(commitCustody?"; snapshot committed, no delivery or guest receipt\n":"; memory only, no delivery or guest receipt\n")<<std::flush;
            }
        }catch(const std::invalid_argument&){
            observation->mailCaptureState="unsupported";
            std::cout<<"XBAND_MAIL_CAPTURE side="<<side<<" state=unsupported; existing service reply unchanged\n"<<std::flush;
        }
    }
    void prepareOutgoingClearExperiment(){
        // Original VF ROM opcode 05 / bit 04 clears ALL four players' outgoing
        // resources. This is only for fresh COW diagnostic profiles, never a
        // production receipt or proof of delivery. Optional snapshot commits
        // happen before this point; ordinary experiment mode is memory-only.
        if((!clearAcceptedOutgoing&&!profileBatchClear&&!profileSubmissionClear)||outgoingClearPrepared||observation->mailCaptureState!="retained"||!observation->lastMailCaptureId)return;
        if(profileBatchClear||profileSubmissionClear){
            // Complete framing/decoding and name resolution happened before
            // staging. Restrict this NEW fixture to the observed full 0/3
            // mixed list, after its whole snapshot commit and RAM publication.
            // This is never a current-player-only or delivery acknowledgement.
            const bool mixed=acceptedOutgoingCount==2&&committedSourcePlayers==std::vector<uint8_t>{0,3};
            const bool single=profileSubmissionClear&&acceptedOutgoingCount==1&&committedSourcePlayers.size()==1&&
                (committedSourcePlayers[0]==0||committedSourcePlayers[0]==3);
            if(!*profileRunValid||!observation->snapshotCommitted||(!mixed&&!single))return;
        }else{
        // The existing clear experiment was verified only for single-entry
        // COW requests. Two-entry admission is not permission to expand clear.
        if(acceptedOutgoingCount!=1)return;
        }
        if(p.tcp.diagnosticMailboxReply.empty())p.tcp.diagnosticMailboxReply={2};
        if(p.tcp.diagnosticMailboxReply.back()!=2)throw std::runtime_error("Missing mail exchange terminator");
        p.tcp.diagnosticMailboxReply.pop_back();
        const LocalTCPProbe::Bytes clear{5,0,0,0,4,2};
        p.tcp.diagnosticMailboxReply.insert(p.tcp.diagnosticMailboxReply.end(),clear.begin(),clear.end());
        outgoingClearPrepared=true;
        observation->batchClearPrepared=profileBatchClear||profileSubmissionClear;
        if(profileSubmissionClear){
            std::cout<<"XBAND_MAIL_PROFILE_SUBMISSION_CLEAR_PREPARED side="<<side<<" records="<<acceptedOutgoingCount<<" last_id="<<observation->lastMailCaptureId
                     <<" command=05 mask=00000004; whole supported request snapshot committed; all-outbox clear, fresh COW only, not delivery receipt\n"<<std::flush;
            return;
        }
        if(profileBatchClear){
            std::cout<<"XBAND_MAIL_PROFILE_BATCH_CLEAR_PREPARED side="<<side<<" records=2 last_id="<<observation->lastMailCaptureId
                     <<" command=05 mask=00000004; whole 0/3 request snapshot committed; clears all outboxes, fresh COW only, not delivery receipt\n"<<std::flush;
            return;
        }
        std::cout<<"XBAND_MAIL_OUTGOING_CLEAR_EXPERIMENT side="<<side<<" retained_id="<<observation->lastMailCaptureId
                 <<" command=05 mask=00000004; clears all player outboxes; COW fixture only, not delivery receipt\n"<<std::flush;
    }
    xband::ServiceEndpoint &service(){
#ifdef PB3_SERVER_LOGIN
        return login;
#else
        return endpoint;
#endif
    }
    const xband::ServiceEndpoint &service()const{return const_cast<PB3Service*>(this)->service();}
    bool transmit(uint8_t b,unsigned f)override{try{const bool ok=service().transmit(b,f);if(ok)++observation->received;return ok;}
        catch(const std::exception& e){if(!activityError){activityError=true;recordActivity("error",e.what());}throw;}}
    void tick(unsigned f)override{
        try{service().tick(f);}catch(const std::exception& e){if(!activityError){activityError=true;recordActivity("error",e.what());}throw;}
        if(activityRequested&&p.tcp.end02Sent&&!activityEnded){activityEnded=true;recordActivity("service_end","Server response terminator queued; not a gameplay or mail delivery receipt");}
        if(p.tcp.replyDiagnosticIncomingRecord&&p.tcp.end02Sent&&!observation->dummyMailReplyQueued){
            observation->dummyMailReplyQueued=true;
            std::cout<<"XBAND_DUMMY_MAIL_REPLY_QUEUED side="<<side
                     <<" records=1; endpoint queue only, guest display not acknowledged\n"<<std::flush;
        }
#ifdef PB3_SERVER_LOGIN
        if(login.phase()!=lastPhase){
            lastPhase=login.phase();
            std::cout<<"PB3_LOGIN_PHASE side="<<side<<" phase="<<static_cast<unsigned>(lastPhase)<<" frame="<<f<<'\n'<<std::flush;
        }
#endif
        if(!verified&&LocalTCPProbe::completeMailProbeRequest(p.tcp.captured,bool(profileNames))){
            const auto &b=p.tcp.captured;
            const std::string number=xband::registrationPhone(b);
            std::vector<uint8_t> name;
            const auto nameStart=xband::registrationOffset(b,81);
            for(size_t i=nameStart;i<nameStart+32&&b[i];++i)name.push_back(b[i]);
            if(name.empty())throw std::runtime_error("PB3 original name empty");
            std::cout<<"PB3_IDENTITY "<<nlohmann::json{{"side",side},{"subscriber",number},{"name_bytes",name}}.dump()<<'\n'<<std::flush;
            verified=true;
            observation->verified=true;observation->subscriber=number;
        }
    }
    bool peek(uint8_t &b)const override{return service().peek(b);}
    void consume()override{service().consume();++observation->sent;}
    size_t pending()const override{return service().pending();}
    void reset()override{if(activityRequested&&!activityEnded){recordActivity(p.tcp.end02Sent?"service_end":"service_abort");}if(!p.tcp.end02Sent&&(!standbyRequest||standbyEngaged)){if(standbyEngaged&&standbyAbort)standbyAbort();roles->withdraw(side);}service().reset();standbyEngaged=false;configure();verified=false;activityRequested=activityEnded=activityError=awardRecorded=pointResultRecorded=false;activityContext=nlohmann::json::object();}
    ~PB3Service(){if(activityRequested&&!activityEnded)recordActivity(p.tcp.end02Sent?"service_end":"service_abort");if(!p.tcp.end02Sent&&(!standbyRequest||standbyEngaged)){if(standbyEngaged&&standbyAbort)standbyAbort();roles->withdraw(side);}}
    void setActivityHistory(std::shared_ptr<diagnostic::ActivityHistory> value,std::function<uint64_t(const nlohmann::json&)> identity={}){activity=std::move(value);activityIdentity=std::move(identity);}
    void setStandbyCallbacks(std::function<xband::StandbyRegistration::Decision(const std::string&,uint32_t)> request,
                             std::function<void()> abort){standbyRequest=std::move(request);standbyAbort=std::move(abort);}
    void setNamedStandbyCallback(std::function<xband::StandbyRegistration::Decision(const std::string&,uint32_t,const std::string&,const std::string&)> request){namedStandbyRequest=std::move(request);}
    void setNamedTargetAccess(std::function<bool(const std::string&)> check){namedTargetAccess=std::move(check);}
#ifdef PB3_SERVICE_SERVER_TEST
    LocalTCPProbe &testTCP(){return p.tcp;}
    void testPostMatchCallbacks(std::function<bool()> eligible,std::function<void()> served){
        postMatchEligible=std::move(eligible);markPostMatchServed=std::move(served);configure();
    }
#endif
};
#ifndef PB3_SERVICE_SERVER_TEST
int main(){try{
    std::shared_ptr<diagnostic::ActivityHistory> activity;
    try{const auto path=_wgetenv(L"XBAND_ACTIVITY_HISTORY_DIR");activity=std::make_shared<diagnostic::ActivityHistory>(path&&*path?std::filesystem::path(path):std::filesystem::path("activity-history"));}
    catch(const std::exception& e){std::cerr<<"XBAND_ACTIVITY_HISTORY_STARTUP_ERROR "<<e.what()<<'\n';}
    if(activity){const auto path=_wgetenv(L"XBAND_GAME_RESULT_DB_DIR");
        diagnostic::activeGameResults=std::make_shared<diagnostic::GameResultDatabase>(path&&*path?std::filesystem::path(path):std::filesystem::path("game-result-db"));}
#ifdef PB3_SERVER_POINT_LEDGER
    if(!activity)throw std::runtime_error("Server points require persistent activity history");
    const auto pointPath=_wgetenv(L"XBAND_GAME_POINT_LEDGER_FILE");
    diagnostic::activeGamePoints=std::make_shared<diagnostic::GamePointLedger>(pointPath&&*pointPath?std::filesystem::path(pointPath):std::filesystem::path("game-point-ledger.json"),activity->page(0).at("total").get<uint64_t>());
    const auto levelPath=_wgetenv(L"XBAND_GAME_LEVEL_FILE");
    diagnostic::activeGameLevels=std::make_shared<diagnostic::GameLevelSettings>(levelPath&&*levelPath?std::filesystem::path(levelPath):std::filesystem::path("game-level-settings.json"));
    std::cout<<"XBAND_SERVER_POINTS zero baseline; per phone/profile/game; persistent dedup; command25 totals; 30 configurable ranks initially200 spacing\n"<<std::flush;
#endif
    unsigned basePort=58240;
    if(const char* setting=std::getenv("XBAND_TEST_BASE_PORT")){
        const std::string value(setting);size_t consumed=0;
        basePort=std::stoul(value,&consumed);
        if(consumed!=value.size()||basePort<1024||basePort>65530)
            throw std::runtime_error("Invalid XBAND_TEST_BASE_PORT");
    }
#ifdef PB3_REGION_TABLE
    const char *regionPath=std::getenv("XBAND_REGION_TABLE_FILE");
    diagnostic::activeRegions=std::make_shared<diagnostic::JapanAreaCodeTable>(
        regionPath&&*regionPath?std::filesystem::path(regionPath):std::filesystem::path("jp-area-codes.json"));
    std::cout<<"XBAND_REGION_TABLE offline rows="<<diagnostic::activeRegions->rowCount()
        <<" prefixes="<<diagnostic::activeRegions->prefixCount()
#ifdef PB3_REGION_TOWN
        <<" allocation-region town delivery enabled for shared XOS\n"
#else
        <<" allocation-region only; no guest profile rewrite\n"
#endif
        <<std::flush;
#endif
#ifdef PB3_GAME_RANKING_SETTINGS
    const char *rankingPath=std::getenv("XBAND_GAME_RANKING_FILE");
    const auto gameSettingsFile=rankingPath&&*rankingPath?std::filesystem::path(rankingPath):std::filesystem::path("game-ranking-settings.json");
    diagnostic::activeGameRankings=std::make_shared<diagnostic::GameRankingSettings>(gameSettingsFile);
#ifdef PB3_VF_POINT_LEDGER
    const auto *ledgerPath=std::getenv("XBAND_VF_POINT_LEDGER_FILE");
    if(ledgerPath&&*ledgerPath){
        diagnostic::activeVFPoints=std::make_shared<diagnostic::VFPointLedger>(std::filesystem::path(ledgerPath));
        const auto legacyRows=diagnostic::activeGameRankings->snapshot();
        const auto legacyVF=std::find_if(legacyRows.begin(),legacyRows.end(),[](const auto& row){return row.gameID==0x00010003;});
        if(legacyVF==legacyRows.end())throw std::runtime_error("Missing VF ranking defaults for legacy level preservation");
        diagnostic::activeVFPoints->preserveLegacyLevels(legacyVF->fields);
        std::cout<<"VF_POINT_LEDGER enabled=1 custom winner+1; baseline initial reports; exact report dedup; VF only\n"<<std::flush;
        std::cout<<"VF_PROFILE_RANKINGS profiles=0..3 new_points=0 new_level=unregistered; existing totals/receipts/levels preserved; no promotion formula\n"<<std::flush;
    }
#endif
#ifdef PB3_USAGE_AREA_SETTINGS
    const char *usagePath=std::getenv("XBAND_USAGE_AREA_FILE");
    diagnostic::activeUsageArea=std::make_shared<diagnostic::UsageAreaSettings>(
        usagePath&&*usagePath?std::filesystem::path(usagePath):std::filesystem::path("usage-area-settings.json"));
    std::cout<<"XBAND_USAGE_AREA fixed/selectable display; keyed by registration phone; next matchmaking login; area-based admission unchanged\n"<<std::flush;
#ifdef PB3_USAGE_TIME_POLICY
    std::cout<<"XBAND_USAGE_TIME_POLICY enabled; per-phone all4 users/all games; saved JST weekday/weekend windows; new calls only; active games finish; mail scope configurable\n"<<std::flush;
#endif
#endif
#ifdef PB3_STANDBY_DIALOG
    const char* waitPath=std::getenv("XBAND_STANDBY_WAIT_FILE");
    diagnostic::activeStandbyWait=std::make_shared<diagnostic::StandbyWaitSettings>(
        waitPath&&*waitPath?std::filesystem::path(waitPath):std::filesystem::path("standby-wait-settings.json"));
    const auto waitValues=diagnostic::activeStandbyWait->snapshot().minutes;
    std::cout<<"XBAND_STANDBY_WAIT server-global minutes="<<waitValues[0]<<','<<waitValues[1]<<','<<waitValues[2]
        <<" short,normal,long; all phones/profiles/games; per-request snapshot\n"<<std::flush;
#endif
    if(!std::filesystem::exists(gameSettingsFile)){
        const auto initial=diagnostic::activeGameRankings->snapshot().front();
        diagnostic::activeGameRankings->update(initial.gameID,initial.fields);
    }
#ifdef PB3_SERVER_POINT_LEDGER
    std::cout<<"XBAND_GAME_RANKINGS computed server totals; initial0; all4 profiles; no test totals; configurable local30-rank policy"
#else
    std::cout<<"XBAND_GAME_RANKINGS fixture distribution disabled; ROM rankings preserved; title/award settings only"
#endif
#ifdef PB3_VF_POINT_LEDGER
        <<"; optional legacy VF ledger retained for internal audit only (no ranking override)"
#endif
        <<'\n'<<std::flush;
#endif
    const char *mailSetting=std::getenv("XBAND_DUMMY_MAIL");
    const bool dummyMail=mailSetting&&std::string_view(mailSetting)=="1";
    const char *probeSetting=std::getenv("XBAND_MAIL_PROBE");
    if(dummyMail&&probeSetting&&std::string_view(probeSetting)=="1")
        throw std::runtime_error("Set XBAND_MAIL_PROBE=0 before enabling XBAND_DUMMY_MAIL");
    auto roles=std::make_shared<xband::LocalMatchRoles>();
    const char *postMatchSetting=std::getenv("XBAND_POSTMATCH_EXPERIMENT");
    const bool postMatchExperiment=postMatchSetting&&std::string_view(postMatchSetting)=="1";
#ifdef PB3_VF_POINT_LEDGER
    if(diagnostic::activeVFPoints&&!postMatchExperiment)throw std::runtime_error("VF ledger requires completed-call eligibility experiment");
#endif
#ifdef PB3_PAIR_CONTROL
    auto pair=std::make_shared<PB3PairControl>();
#ifdef PB3_USAGE_TIME_POLICY
    pair->matchAdmission=[](const std::string& phone){return !diagnostic::activeUsageArea||diagnostic::activeUsageArea->accessAllowed(phone,false);};
#endif
    pair->roles=roles;
    pair->activity=activity;
    const char* standbySetting=std::getenv("XBAND_STANDBY");
    if(!standbySetting)standbySetting=std::getenv("XBAND_VF_STANDBY_EXPERIMENT");
    pair->standbyEnabled=!standbySetting||std::string_view(standbySetting)!="0";
    if(pair->standbyEnabled)std::cout<<"XBAND_STANDBY shared across game IDs; read-only XOS observer; finite guest-counter window (see per-request wait log)\n"<<std::flush;
#else
    if(postMatchExperiment)throw std::runtime_error("Post-match experiment requires pair control");
#endif
    const char *captureSetting=std::getenv("XBAND_MAIL_CAPTURE");
    std::shared_ptr<diagnostic::LocalMailJournal> localMailJournal;
#ifdef PB3_LOCAL_MAIL_JOURNAL
    if(const auto path=_wgetenv(L"XBAND_LOCAL_MAIL_JOURNAL_DIR");path&&*path){
        localMailJournal=std::make_shared<diagnostic::LocalMailJournal>(std::filesystem::path(path));
        std::cout<<"LOCAL_MAIL_JOURNAL_READY persistent=1 inbox=local-name-only clear=after-whole-request-commit; no server retry; no fixed total-count cap\n"<<std::flush;
    }
#endif
    const auto mailCaptures=captureSetting&&std::string_view(captureSetting)=="1"?
        std::make_shared<xband::MailCaptureStore>():std::shared_ptr<xband::MailCaptureStore>{};
    const char *forwardSetting=std::getenv("XBAND_MAIL_FORWARD");
    const bool forwarding=forwardSetting&&std::string_view(forwardSetting)=="1";
    const char *clearSetting=std::getenv("XBAND_MAIL_CLEAR_EXPERIMENT");
    const bool clearAccepted=clearSetting&&std::string_view(clearSetting)=="1";
    const char *inventorySetting=std::getenv("XBAND_MAIL_INVENTORY_EXPERIMENT");
    const bool inventory=inventorySetting&&std::string_view(inventorySetting)=="1";
    if(inventory&&(!forwarding||!clearAccepted))throw std::runtime_error("Inventory experiment requires forwarding=1 and clear experiment=1, fresh COW profiles only");
    if(clearAccepted&&(!mailCaptures||dummyMail||(probeSetting&&std::string_view(probeSetting)=="1")))
        throw std::runtime_error("Outgoing clear experiment requires capture=1, dummy=0, probe=0");
    const auto *snapshotSetting=_wgetenv(L"XBAND_MAIL_SNAPSHOT_FILE");
    const std::filesystem::path snapshotPath=snapshotSetting?snapshotSetting:L"";
    if(!snapshotPath.empty()&&!inventory)throw std::runtime_error("Snapshot experiment requires inventory=1 and isolated test profiles");
    std::function<void(const xband::MailCaptureStore&)> commit;
    if(!snapshotPath.empty()){
        const char *newSetting=std::getenv("XBAND_MAIL_SNAPSHOT_NEW");
        const bool createNew=newSetting&&std::string_view(newSetting)=="1";
        if(createNew&&std::filesystem::exists(snapshotPath))throw std::runtime_error("New snapshot path already exists; use restore mode instead");
        *mailCaptures=diagnostic::loadMailSnapshot(snapshotPath,createNew);
        commit=[snapshotPath](const auto &store){diagnostic::commitMailSnapshot(snapshotPath,store);};
        std::cout<<"XBAND_MAIL_SNAPSHOT_LOADED records="<<mailCaptures->captures().size()<<"; preserved local IDs, corrupt state refuses startup\n"<<std::flush;
    }
    if(forwarding&&(!mailCaptures||dummyMail||(probeSetting&&std::string_view(probeSetting)=="1")))
        throw std::runtime_error("Mail forwarding requires capture=1, dummy=0, probe=0");
    const auto mailRoutes=forwarding?std::make_shared<xband::MailAccountRoutes>():std::shared_ptr<xband::MailAccountRoutes>{};
    const char *profileSetting=std::getenv("XBAND_MAIL_PROFILE_EXPERIMENT");
    const bool profileExperiment=profileSetting&&std::string_view(profileSetting)=="1";
    if(profileExperiment&&(!mailCaptures||dummyMail||forwarding||clearAccepted||inventory||commit||
       (probeSetting&&std::string_view(probeSetting)=="1")))
        throw std::runtime_error("Profile experiment requires fresh-COW capture ONLY, no forwarding/clear/snapshot");
    const auto profileNames=profileExperiment?std::make_shared<xband::MailProfileNames>():std::shared_ptr<xband::MailProfileNames>{};
    const char *profileMailboxSetting=std::getenv("XBAND_MAIL_PROFILE_MAILBOX_EXPERIMENT");
    const bool profileMailbox=profileMailboxSetting&&std::string_view(profileMailboxSetting)=="1";
    if(profileMailbox&&!profileExperiment)throw std::runtime_error("Profile mailbox requires fresh-COW profile experiment");
    const auto profileRunValid=profileExperiment?std::make_shared<bool>(true):std::shared_ptr<bool>{};
    const char *profileInventorySetting=std::getenv("XBAND_MAIL_PROFILE_INVENTORY_EXPERIMENT");
    const bool profileInventory=profileInventorySetting&&std::string_view(profileInventorySetting)=="1";
    if(profileInventory&&!profileMailbox)throw std::runtime_error("Profile inventory requires isolated profile mailbox");
    const auto profileOffers=profileInventory?std::make_shared<xband::MailProfileOffers>():std::shared_ptr<xband::MailProfileOffers>{};
    const auto *rememberSetting=std::getenv("XBAND_MAIL_PROFILE_REMEMBER_HELD_EXPERIMENT");
    const bool rememberHeld=rememberSetting&&std::string_view(rememberSetting)=="1";
    if(rememberHeld&&!profileInventory)throw std::runtime_error("Remembered held experiment requires controlled profile inventory");
    const auto *batchClearSetting=std::getenv("XBAND_MAIL_PROFILE_BATCH_CLEAR_EXPERIMENT");
    const bool profileBatchClear=batchClearSetting&&std::string_view(batchClearSetting)=="1";
    const auto *submissionSetting=std::getenv("XBAND_MAIL_PROFILE_SUBMISSION_CLEAR_EXPERIMENT");
    const bool profileSubmissionClear=submissionSetting&&std::string_view(submissionSetting)=="1";
    const auto *profileRestoreSetting=std::getenv("XBAND_MAIL_PROFILE_RESTART_PROBE");
    const bool profileRestartProbe=profileRestoreSetting&&std::string_view(profileRestoreSetting)=="1";
    if(profileRestartProbe&&(!profileSubmissionClear||!rememberHeld))
        throw std::runtime_error("Profile restart probe requires explicit submission-clear and held-history experiments");
    if(profileBatchClear&&profileSubmissionClear)throw std::runtime_error("Profile clear experiments are mutually exclusive");
    if(profileBatchClear||profileSubmissionClear){
        if(!profileInventory||!profileMailbox||!profileRunValid||commit)
            throw std::runtime_error("Mixed-profile clear requires fresh controlled profile inventory, no legacy snapshot");
        const auto *batchPathSetting=_wgetenv(profileSubmissionClear?L"XBAND_MAIL_PROFILE_SUBMISSION_SNAPSHOT_FILE":L"XBAND_MAIL_PROFILE_BATCH_SNAPSHOT_FILE");
        const std::filesystem::path batchPath=batchPathSetting?batchPathSetting:L"";
        if(batchPath.empty()||!batchPath.is_absolute()||(!profileRestartProbe&&std::filesystem::exists(batchPath))||
           (profileRestartProbe&&!std::filesystem::exists(batchPath)))
            throw std::runtime_error("Mixed-profile clear needs an absolute NEW custody snapshot path, except explicit restart probe");
        auto pending=batchPath;pending+=L".pending";
        if(std::filesystem::exists(pending))throw std::runtime_error("Preserve existing mixed-profile pending snapshot");
        if(profileRestartProbe){
            *mailCaptures=diagnostic::loadMailSnapshot(batchPath,false);
            std::cout<<"XBAND_MAIL_PROFILE_RESTART_PROBE captures="<<mailCaptures->captures().size()
                     <<"; custody restored, profile-state checkpoint validation follows\n"<<std::flush;
        }
        commit=[batchPath](const auto &store){diagnostic::commitMailSnapshot(batchPath,store);};
    }
    const auto *profileStatePathSetting=_wgetenv(L"XBAND_MAIL_PROFILE_RESTART_STATE_FILE");
    const std::filesystem::path profileStatePath=profileStatePathSetting?profileStatePathSetting:L"";
    const auto *profileTokenSetting=std::getenv("XBAND_MAIL_PROFILE_RESTART_TOKEN");
    const std::string profileToken=profileTokenSetting?profileTokenSetting:"";
    if(profileStatePath.empty()!=profileToken.empty())
        throw std::runtime_error("Profile restart state path and token must be provided together");
    if(profileRestartProbe&&profileStatePath.empty())throw std::runtime_error("Profile restart probe requires the state checkpoint");
    if(!profileStatePath.empty()&&(!profileStatePath.is_absolute()||!profileSubmissionClear||!rememberHeld))
        throw std::runtime_error("Profile restart state requires the bounded submission/held experiment");
    std::function<void()> commitProfileState;
    if(!profileStatePath.empty()){
        auto pending=profileStatePath;pending+=L".pending";
        if(std::filesystem::exists(pending))throw std::runtime_error("Preserve existing profile restart pending file");
        if(profileRestartProbe){
            diagnostic::loadProfileRestartSnapshot(profileStatePath,profileToken,*mailCaptures,*profileNames,*profileOffers);
            std::cout<<"XBAND_MAIL_PROFILE_STATE_RESTORED names="<<profileNames->size()<<" offers="<<profileOffers->size()
                     <<"; one controlled fresh-COW replay, not authentication\n"<<std::flush;
        }else if(std::filesystem::exists(profileStatePath)){
            throw std::runtime_error("New profile restart state path already exists");
        }
        commitProfileState=[profileStatePath,profileToken,mailCaptures,profileNames,profileOffers]{
            diagnostic::commitProfileRestartSnapshot(profileStatePath,profileToken,*mailCaptures,*profileNames,*profileOffers);
        };
        if(!profileRestartProbe)commitProfileState();
    }
    using Host=xband::windows::TcpHost;
    std::array<std::unique_ptr<Host>,
#ifdef PB3_PAIR_CONTROL
        6
#else
        4
#endif
    > hosts;
    std::array<std::shared_ptr<PB3Observation>,2> observations{std::make_shared<PB3Observation>(),std::make_shared<PB3Observation>()};
    for(unsigned i=0;i<2;++i)hosts[i]=std::make_unique<Host>(xband::protocol::ServerConfig{
        "127.0.0.1",static_cast<uint16_t>(basePort+i),false,{{"pb3-"+std::to_string(i),std::string(64,'a')}}},
        xband::FrameClock(1,1),[i,o=observations[i],roles,dummyMail,mailCaptures,mailRoutes,clearAccepted,inventory,commit,profileNames,profileMailbox,profileRunValid,profileOffers,profileBatchClear,profileSubmissionClear,rememberHeld,commitProfileState,postMatchExperiment,localMailJournal
#ifdef PB3_PAIR_CONTROL
        ,pair
#endif
        ,activity](uint64_t hz)->std::unique_ptr<xband::ServiceEndpoint>{
            if(hz!=60)throw std::runtime_error("PB3 frame clock mismatch");
            const char *probe=std::getenv("XBAND_MAIL_PROBE");
            std::function<bool()> postMatch;
            std::function<void()> markPostMatch;
#ifdef PB3_PAIR_CONTROL
            if(postMatchExperiment){
                postMatch=[pair,i]{return pair->generation>1&&!pair->postMatchServed[i];};
                markPostMatch=[pair,i]{
                    pair->postMatchServed[i]=true;
                    std::cout<<"XBAND_POSTMATCH_SERVED side="<<i<<" generation="<<pair->generation<<'\n'<<std::flush;
                };
            }
#endif
            auto endpoint=std::make_unique<PB3Service>(i,o,roles,dummyMail,probe&&std::string_view(probe)=="1",mailCaptures,mailRoutes,clearAccepted,inventory,commit,profileNames,profileMailbox,profileRunValid,profileOffers,profileBatchClear,profileSubmissionClear,rememberHeld,commitProfileState,std::move(postMatch),std::move(markPostMatch));
            endpoint->setLocalMailJournal(localMailJournal);
            endpoint->setActivityHistory(activity
#ifdef PB3_PAIR_CONTROL
                ,[pair,i](const nlohmann::json& context){pair->activityContext[i]=context;return pair->generation;}
#endif
            );
#ifdef PB3_PAIR_CONTROL
            if(pair->standbyEnabled)endpoint->setStandbyCallbacks(
                [pair,i](const std::string& phone,uint32_t game){return pair->registerStandby(i,phone,game);},
                [pair,i]{pair->abortStandbyService(i);});
            if(pair->standbyEnabled)endpoint->setNamedStandbyCallback(
                [pair,i](const std::string& phone,uint32_t game,const std::string& name,const std::string& target){return pair->registerStandby(i,phone,game,name,target);});
#ifdef PB3_USAGE_TIME_POLICY
            endpoint->setNamedTargetAccess([pair,i](const std::string& target){
                const auto& peer=pair->standby.entry(1-i);
                return !peer||pair->standbyNames[1-i]!=target||!pair->matchAdmission||pair->matchAdmission(peer->phone);
            });
#endif
#endif
            return endpoint;});
    auto relay=std::make_shared<PB3RelayQueues>();
    for(unsigned i=0;i<2;++i)hosts[i+2]=std::make_unique<Host>(xband::protocol::ServerConfig{
        "127.0.0.1",static_cast<uint16_t>(basePort+2+i),false,{{"peer-"+std::to_string(i),std::string(64,'a')}}},
        xband::FrameClock(1,1),[i,relay](uint64_t hz)->std::unique_ptr<xband::ServiceEndpoint>{
            if(hz!=60)throw std::runtime_error("relay clock mismatch");return std::make_unique<PB3Relay>(relay,i);});
#ifdef PB3_SERVER_WINDOW
#ifdef XBAND_FRONTEND_SERVER
    XbandDashboard monitor;
    monitor.setActivityHistory(activity);
    if(diagnostic::activeGameResults)monitor.setGameResultHistory(diagnostic::activeGameResults->history());
    monitor.setLocalMailJournal(localMailJournal);
#ifdef PB3_STANDBY_DIALOG
    monitor.setStandbyWaitSettings(diagnostic::activeStandbyWait);
#endif
#ifdef PB3_GAME_RANKING_SETTINGS
    monitor.setRankingSettings(diagnostic::activeGameRankings);
#ifdef PB3_USAGE_AREA_SETTINGS
    monitor.setUsageAreaSettings(diagnostic::activeUsageArea);
#endif
#endif
#else
    ProbePreview monitor(true,L"XBAND Server - PB3 migration / local TCP");
#endif
#endif
#ifdef PB3_PAIR_CONTROL
    for(unsigned i=0;i<2;++i)hosts[i+4]=std::make_unique<Host>(xband::protocol::ServerConfig{
        "127.0.0.1",static_cast<uint16_t>(basePort+4+i),false,{{"call-"+std::to_string(i),std::string(64,'a')}}},
        xband::FrameClock(1,1),[i,pair](uint64_t hz)->std::unique_ptr<xband::ServiceEndpoint>{
            if(hz!=60)throw std::runtime_error("pair control clock mismatch");return std::make_unique<PB3PairControlEndpoint>(pair,i);});
    std::cout<<"PB3_CALL_CONTROL_READY ports="<<basePort+4<<".."<<basePort+5<<" migration-only\n"<<std::flush;
#endif
    std::cout<<"PB3_SERVER_READY ports="<<basePort<<".."<<basePort+3<<" loopback-only v2\n"<<std::flush;
    std::cout<<"XBAND_POSTMATCH_EXPERIMENT enabled="<<postMatchExperiment<<"; observed VF post-call code 03 routes to matchmaking, code 02 gets terminator only; no result storage or authentication\n"<<std::flush;
    std::cout<<"XBAND_DUMMY_MAIL enabled="<<dummyMail<<"; mail-only request 04, fixed test sample, no outgoing delivery\n"<<std::flush;
    std::cout<<"XBAND_MAIL_CAPTURE enabled="<<bool(mailCaptures)<<(commit?"; snapshot experiment, no delivery or receipt\n":"; memory only, no delivery or receipt\n")<<std::flush;
    std::cout<<"XBAND_MAIL_FORWARD enabled="<<forwarding<<"; local experimental routing, no historical auth or receipt\n"<<std::flush;
    std::cout<<"XBAND_MAIL_CLEAR_EXPERIMENT enabled="<<clearAccepted<<"; all player outboxes, fresh COW diagnostic profile only\n"<<std::flush;
    std::cout<<"XBAND_MAIL_INVENTORY_EXPERIMENT enabled="<<inventory<<"; synthetic custody-ID tokens; fresh COW profiles only\n"<<std::flush;
    std::cout<<"XBAND_MAIL_PROFILE_EXPERIMENT enabled="<<profileExperiment
             <<((profileBatchClear||profileSubmissionClear)?"; fresh-COW player 0/3 with committed request clear, no authentication\n":profileMailbox?"; fresh-COW player 0/3 with isolated mailbox, no authentication/clear\n":"; fresh-COW player 0/3 capture only, no authentication/forward/clear\n")<<std::flush;
    std::cout<<"XBAND_MAIL_PROFILE_MAILBOX_EXPERIMENT enabled="<<profileMailbox<<((profileBatchClear||profileSubmissionClear)?"; current local profile only, committed clear separate, no receipt\n":"; current local profile only, no receipt or clear\n")<<std::flush;
    std::array<bool,2> hadProfileTransport{};
    std::cout<<"XBAND_MAIL_PROFILE_INVENTORY_EXPERIMENT enabled="<<profileInventory<<((profileBatchClear||profileSubmissionClear)?"; per-profile offers, committed clear separate, no receipt\n":"; per-profile offers, no clear or receipt\n")<<std::flush;
    std::cout<<"XBAND_MAIL_PROFILE_REMEMBER_HELD_EXPERIMENT enabled="<<rememberHeld<<"; scoped observed holdings, volatile epoch; no delivery/read ACK\n"<<std::flush;
    std::cout<<"XBAND_MAIL_PROFILE_BATCH_CLEAR_EXPERIMENT enabled="<<profileBatchClear<<"; fresh-COW mixed 0/3 only, snapshot before all-outbox clear, not delivery receipt\n"<<std::flush;
    std::cout<<"XBAND_MAIL_PROFILE_SUBMISSION_CLEAR_EXPERIMENT enabled="<<profileSubmissionClear<<"; fresh-COW single 0/3 or mixed 0,3 only; snapshot before all-outbox clear, not delivery receipt\n"<<std::flush;
    // Headless probes retain a finite safety limit. The interactive monitor
    // must remain available until its window is closed, including idle time.
#ifndef PB3_SERVER_WINDOW
    const auto start=GetTickCount64();
#endif
    uint64_t next=0;
    PB3Wait pacing;
#ifdef PB3_SERVER_WINDOW
    while(true){
#else
    while(GetTickCount64()-start<3000000){
#endif
#ifdef PB3_SERVER_WINDOW
        if(monitor.isClosed())break;
#endif
        const auto now=GetTickCount64();
        for(auto &h:hosts)if(!h->step(now))throw std::runtime_error("PB3 host stopped");
        if(profileNames)for(unsigned i=0;i<2;++i){
            const bool connected=hosts[i]->clientCount()!=0;
            if(hadProfileTransport[i]&&!connected){
                profileNames->invalidateEndpoint("pb3-"+std::to_string(i));
                *profileRunValid=false; // Stored mail must never migrate to a replacement COW profile.
                if(profileOffers)profileOffers->invalidateAll();
                std::cout<<"XBAND_MAIL_PROFILE_INVALIDATED side="<<i<<" reason=transport_disconnected\n"<<std::flush;
            }
            hadProfileTransport[i]=connected;
        }
        if(now>=next){
            auto status=hosts[0]->status();status["endpoints"].push_back(hosts[1]->status()["endpoints"][0]);
            status["pending_connections"]=hosts[0]->status()["pending_connections"].get<size_t>()+hosts[1]->status()["pending_connections"].get<size_t>();
            status["pb3_relay"]={{"sent",relay->sent},{"received",relay->received},
                {"pending",{relay->bytes[0].size(),relay->bytes[1].size()}}};
            status["mail_capture_store"]={{"enabled",bool(mailCaptures)},
                {"records",mailCaptures?mailCaptures->captures().size():0},
                {"bytes",mailCaptures?mailCaptures->retainedBytes():0},{"storage",commit?"snapshot-experiment":"memory"},
                {"forwarding_enabled",forwarding||profileMailbox},{"delivery_acknowledgement_implemented",false},{"delivery_implemented",false}};
            status["local_mail_journal"]=localMailJournal?localMailJournal->summary():nlohmann::json{{"enabled",false}};
            status["mail_profile_names"]={{"enabled",profileExperiment},{"entries",profileNames?profileNames->size():0},
                {"scope","fresh-COW server run; invalidated on transport disconnect"},{"authentication",false},
                {"mailbox_experiment",profileMailbox},{"epoch_valid",profileRunValid&&*profileRunValid}};
#ifdef PB3_PAIR_CONTROL
            status["pair_control"]={{"state",pair->state},{"caller",roles->caller},{"callee",roles->callee()},{"requested",roles->requested},{"generation",pair->generation},{"transport",pair->asynchronous?"async-v1":"lockstep"},
                {"joined",pair->joined},{"finished",pair->finished},{"failed",pair->failed||pair->closing},
                {"cycles",pair->elapsed},{"sent",pair->sent},{"received",pair->received}};
#endif
            for(unsigned i=0;i<2;++i){
                auto &row=status["endpoints"][i];const auto &o=*observations[i];
                row["pb3_observation"]={{"subscriber",o.subscriber},{"verified",o.verified},
                    {"received",std::to_string(o.received)},{"sent",std::to_string(o.sent)},
                    {"card_state","unknown"},{"dummy_mail_enabled",o.dummyMailEnabled},
                    {"dummy_mail_reply_queued",o.dummyMailReplyQueued},{"service_request",o.serviceRequest},
                    {"mail_capture_state",o.mailCaptureState},{"last_mail_capture_id",o.lastMailCaptureId}};
                row["pb3_observation"]["mail_forward_prepared"]=o.forwardedMailQueued;
#ifdef PB3_REGION_TABLE
                const auto region=diagnostic::activeRegions->lookup(o.subscriber);
                row["pb3_observation"]["registration_region"]={{"status",region.status},{"display",region.display},
                    {"prefectures",region.prefectures},{"area_codes",region.areas},{"matched_digits",region.matchedDigits},
                    {"scope","current fixed-line allocation; inferred, not authenticated"}};
#ifdef PB3_REGION_TOWN
                row["pb3_observation"]["registration_region"]["town_delivery_scope"]="VF00010003 match-login command38; enabled, not proof of display or receipt";
#else
                row["pb3_observation"]["registration_region"]["town_delivery_scope"]="lookup only; no guest rewrite";
#endif
#endif
                row["pb3_observation"]["mail_snapshot_committed"]=o.snapshotCommitted;
                row["pb3_observation"]["batch_clear_experiment"]={{"enabled",profileBatchClear},{"prepared",profileBatchClear&&o.batchClearPrepared},{"snapshot_committed",o.snapshotCommitted},{"scope","last service call; all outboxes, not delivery receipt"}};
                row["pb3_observation"]["submission_clear_experiment"]={{"enabled",profileSubmissionClear},{"prepared",profileSubmissionClear&&o.batchClearPrepared},{"snapshot_committed",o.snapshotCommitted},{"scope","last call; whole supported request, not delivery receipt"}};
                row["pb3_observation"]["mail_selection"]={{"matched",o.mailMatched},{"deferred",o.mailDeferred},
                    {"prepared",o.forwardedMailQueued},{"withheld",o.inventoryWithheld},
                    {"scope","last_service_call"},{"receipt",false}};
                row["pb3_observation"]["inventory_experiment"]={{"enabled",inventory||profileInventory},{"reported",o.inventoryReported},{"withheld",o.inventoryWithheld}};
            }
#ifdef PB3_SERVER_WINDOW
#ifdef XBAND_FRONTEND_SERVER
            auto display=status;
            display["diagnostic_test_progress"]=xband::monitor::readTestProgressLine(std::getenv("XBAND_DIAGNOSTIC_TEST_PROGRESS_FILE"));
            for(unsigned i=0;i<2;++i){std::ostringstream hex;hex<<std::hex<<std::uppercase<<std::setfill('0');for(auto b:pair->recent[i])hex<<std::setw(2)<<unsigned(b)<<' ';display[i?"hex1":"hex0"]=hex.str();}
            monitor.publish(display);
#else
            std::ostringstream view;
            view<<"XBAND SERVER / PB3 MIGRATION\n\nLoopback TCP ports "<<basePort<<".."<<basePort+3<<"\nService and peer payload relay in this EXE.\n\n";
            for(unsigned i=0;i<2;++i){
                const auto &row=status["endpoints"][i];const auto &o=*observations[i];
                view<<"MODEM "<<i+1<<" : "<<(row["connected"].get<bool>()?"Connected":"Disconnected")
                    <<(row["call_active"].get<bool>()?" / Service call":" / Idle")<<"\n"
                    <<"Phone: "<<(o.verified?o.subscriber:"Not yet verified from game")<<"\n"
                    <<"Service RX: "<<o.received<<" bytes / TX: "<<o.sent<<" bytes\n"
                    <<"Card: UNKNOWN (no card telemetry; not zero balance)\n\n";
            }
            view<<"Peer relay connections: "<<hosts[2]->clientCount()<<" / "<<hosts[3]->clientCount()<<"\n";
#ifdef PB3_PAIR_CONTROL
            view<<"Independent call: "<<pair->state<<" (0 idle / 1 ring / 2 connected)\n"
                <<"Modem timing: "<<(pair->asynchronous?"Independent clocks (async-v1)":"Legacy lockstep")<<"\n"
                <<"Call ports: "<<basePort+4<<".."<<basePort+5<<" / generation "<<pair->generation<<"\n"
                <<"Independent guest cycles: "<<pair->elapsed[0]<<" / "<<pair->elapsed[1]<<"\n"
                <<"Game A->B: "<<pair->sent[0]<<" / B->A: "<<pair->sent[1]<<" bytes\n";
#endif
            view<<"Relayed A->B: "<<relay->sent[0]<<" / B->A: "<<relay->sent[1]<<" bytes\n";
            view<<"Close this window to stop the server.\nNo public network, media writes or card debits.\n";
            monitor.publishText(view.str());
#endif
#endif
            const auto wall=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
            std::cout<<"HOST_STATUS "<<nlohmann::json{{"emitted_at_ms",wall},{"host",status}}.dump()<<'\n'<<std::flush;
            next=now+500;
        }
        fd_set readable,writable;FD_ZERO(&readable);FD_ZERO(&writable);
        bool canWait=true;
        for(auto &h:hosts)canWait=h->appendWaitSockets(readable,writable)&&canWait;
        if(canWait){timeval timeout{0,1000};
            if(select(0,&readable,&writable,nullptr,&timeout)==SOCKET_ERROR)throw std::runtime_error("PB3 host readiness wait failed");
        }
    }
    for(auto &h:hosts)h->stop();
    return 0;
}catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}}
#endif
