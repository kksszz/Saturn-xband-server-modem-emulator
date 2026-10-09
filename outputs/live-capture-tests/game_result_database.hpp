#pragma once
#include "game_point_history.hpp"
#include "deferred_credit_result_observation.hpp"
namespace diagnostic {
// Append-only local database of reports, not a claim of historical production rules.
// Raw result blocks (20 or 23) are authoritative; verified Saturn offsets only.
class GameResultDatabase {
    using J=nlohmann::json;
    std::shared_ptr<ActivityHistory> reports;
    std::mutex mutex;
public:
    explicit GameResultDatabase(const std::filesystem::path& path):reports(std::make_shared<ActivityHistory>(path,true)){
        for(size_t page=0;;++page){const auto batch=reports->page(page);
            for(const auto& row:batch.at("rows")){const auto raw=row.at("raw_command20").get<LocalTCPProbe::Bytes>();
                if(raw.size()!=84||LocalTCPProbe::longword(raw,0)!=84||row.at("report_fingerprint")!=pointFingerprint(raw)||row.at("event")!="game_result_report"||!row.at("report_key").is_string())
                    throw std::runtime_error("Invalid saved game result report");}
            if((page+1)*50>=batch.at("total").get<size_t>())break;
        }
    }
    std::shared_ptr<ActivityHistory> history()const{return reports;}
    void observe(ActivityHistory& activity,J context,const LocalTCPProbe::Bytes& request){
        const auto raw=LocalTCPProbe::observedGameResult(request,true);if(raw.empty())return;
        std::lock_guard lock(mutex);
        const auto fingerprint=pointFingerprint(raw);const auto game=LocalTCPProbe::longword(raw,4);
        J audit=nullptr,ledger=nullptr,connection=nullptr;uint64_t latestObservedCarrier=0;
        for(size_t page=0;;++page){const auto batch=activity.page(page);
            for(const auto& row:batch.at("rows")){
                if(!latestObservedCarrier&&row.value("event",std::string{})=="connected"){
                    if(row.contains("participants"))for(const auto& participant:row.at("participants")){
                        if(participant.value("phone",std::string{})==context.value("phone",std::string{})){latestObservedCarrier=row.at("id").get<uint64_t>();break;}}
                    else if(row.value("phone",std::string{})==context.value("phone",std::string{}))latestObservedCarrier=row.at("id").get<uint64_t>();
                }
                if(row.value("phone",std::string{})!=context.value("phone",std::string{}))continue;
                if(row.value("report_fingerprint",std::string{})!=fingerprint||row.value("game",0u)!=game)continue;
                if(audit.is_null()&&row.value("event",std::string{})=="points_result"&&row.value("attribution_version",0)==2)audit=row;
                if(ledger.is_null()&&row.value("event",std::string{})=="points_ledger")ledger=row;
            }
            if((page+1)*50>=batch.at("total").get<size_t>())break;
        }
        const uint64_t carrier=audit.is_null()?0:audit.value("match_connection_id",uint64_t{});
        if(!ledger.is_null()&&(!carrier||ledger.value("match_connection_id",uint64_t{})!=carrier))ledger=nullptr;
        if(carrier)for(size_t page=0;;++page){const auto batch=activity.page(page);
            for(const auto& row:batch.at("rows"))if(row.at("id")==carrier&&row.value("event",std::string{})=="connected")connection=row;
            if(!connection.is_null()||(page+1)*50>=batch.at("total").get<size_t>())break;
        }
        context["event"]="game_result_report";context["game"]=game;
        if(const auto registrationGame=receivedGameID(request);registrationGame&&*registrationGame!=game){
            context["reporting_game"]=*registrationGame;
            context["title"]=audit.is_null()?J("不明（結果と登録のゲームIDが異なる）"):audit.value("title",J("不明"));
        }
        context["reporting_profile"]=context.value("profile",-1);context["reporting_name"]=context.value("name",std::string{});
        context["profile"]=nullptr;context["name"]="不明";
        // Preserve the legacy key for existing DB/UI readers, including 23.
        context["raw_command20"]=raw;context["raw_result_block"]=raw;context["report_fingerprint"]=fingerprint;
        context["game_error"]=LocalTCPProbe::longword(raw,8);
        context["credit_observation"]=deferredCreditResultObservation(request);
        context["local_player1_result"]=LocalTCPProbe::longword(raw,12);context["local_player2_result"]=LocalTCPProbe::longword(raw,16);
        context["remote_player1_result"]=LocalTCPProbe::longword(raw,20);context["remote_player2_result"]=LocalTCPProbe::longword(raw,24);
        const uint64_t local=uint64_t(LocalTCPProbe::longword(raw,12))+LocalTCPProbe::longword(raw,16);
        const uint64_t remote=uint64_t(LocalTCPProbe::longword(raw,20))+LocalTCPProbe::longword(raw,24);
        context["local_result"]=local;context["remote_result"]=remote;
        context["result_outcome"]=context["game_error"]!=0?"エラー報告":local>remote?"勝ち":local<remote?"負け":"引き分け";
        context["play_time"]=nullptr;context["play_time_unit"]=nullptr; // old GameResult layout is not assumed identical
        context["participants"]=connection.is_null()?J(nullptr):connection.value("participants",J(nullptr));
        context["observed_latest_connection_id"]=latestObservedCarrier?J(latestObservedCarrier):J(nullptr);
        context["match_connection_id"]=carrier?J(carrier):J(nullptr);
        if(carrier){context["profile"]=audit.at("profile");context["name"]=audit.value("name",std::string{});}
        context["correlation_status"]=carrier?"既存サーバー照合・両端末報告の一致は未検証":"参加者未特定";
        context["points_delta"]=ledger.is_null()?J(nullptr):ledger.value("points_delta",J(nullptr));
        context["points_total"]=ledger.is_null()?J(nullptr):ledger.value("points_total",J(nullptr));
        context["point_audit"]=audit;context["point_receipt"]=ledger;
        // Later correlation/ledger evidence is a new immutable revision, not an overwrite.
        const auto key=context.value("phone",std::string{})+":"+fingerprint+":"+std::to_string(latestObservedCarrier)+":"+std::to_string(carrier)+":"+
            std::to_string(audit.is_null()?0:audit.value("id",uint64_t{}))+":"+std::to_string(ledger.is_null()?0:ledger.value("id",uint64_t{}));
        for(size_t page=0;;++page){const auto batch=reports->page(page);
            for(const auto& row:batch.at("rows"))if(row.value("report_key",std::string{})==key)return;
            if((page+1)*50>=batch.at("total").get<size_t>())break;
        }
        context["report_key"]=key;context["record_kind"]="端末結果報告（対戦集約ではない）";
        if(!reports->append(std::move(context)))throw std::runtime_error("Game result database save failed; raw request journal remains available");
    }
};
inline std::shared_ptr<GameResultDatabase> activeGameResults;
}
