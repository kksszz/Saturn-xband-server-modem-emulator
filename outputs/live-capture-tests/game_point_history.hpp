#pragma once
#include "activity_history.hpp"
#include "local_tcp_probe.hpp"
#include "game_ranking_settings.hpp"
#include <bcrypt.h>
#pragma comment(lib,"bcrypt.lib")
namespace diagnostic {
// Audit only. Results report counters, not the ROM's saved ranking total.
inline std::string pointFingerprint(const LocalTCPProbe::Bytes& bytes){
    BCRYPT_ALG_HANDLE alg{};if(BCryptOpenAlgorithmProvider(&alg,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0)throw std::runtime_error("Point audit SHA256 provider");
    std::array<unsigned char,32> digest{};
    const auto status=BCryptHash(alg,nullptr,0,const_cast<PUCHAR>(bytes.data()),ULONG(bytes.size()),digest.data(),ULONG(digest.size()));BCryptCloseAlgorithmProvider(alg,0);
    if(status<0)throw std::runtime_error("Point audit SHA256");
    constexpr char hex[]="0123456789abcdef";std::string out;for(auto b:digest){out+=hex[b>>4];out+=hex[b&15];}return out;
}
inline bool recordGamePointResult(ActivityHistory& history,nlohmann::json context,const LocalTCPProbe::Bytes& request,const GameRankingSettings& settings,uint64_t reportedAt=0){
    const auto result=LocalTCPProbe::observedGameResult(request);if(result.empty())return false;
    const auto game=LocalTCPProbe::longword(result,4);const auto games=settings.snapshot();
    const auto configured=std::find_if(games.begin(),games.end(),[&](const auto&r){return r.gameID==game;});if(configured==games.end())return false;
    const auto fingerprint=pointFingerprint(result);
    const auto phone=context.value("phone",std::string{});const auto profile=context.value("profile",-1);
    if(phone.empty()||profile<0||profile>3)return false;
    nlohmann::json prepared;std::vector<nlohmann::json> prior;
    for(size_t page=0;;++page){const auto batch=history.page(page);
        for(const auto& row:batch.at("rows")){
            if(!reportedAt||row.at("unix_ms").get<uint64_t>()<=reportedAt)prior.push_back(row);
        }
        if((page+1)*50>=batch.at("total").get<size_t>())break;
    }
    // The ROM stores one terminal-wide result at 012A:0. A login's selected
    // slot is NOT its owner. Resolve the latest actual carrier on this terminal,
    // then the participant's policy snapshot preceding that carrier. Never
    // fall back to an older match just because the selected slot has a policy.
    nlohmann::json connection,owner;
    for(const auto& row:prior){
        if(row.value("event",std::string{})!="connected")continue;
        if(row.contains("participants")){
            for(const auto& participant:row.at("participants"))if(participant.value("phone",std::string{})==phone){owner=participant;break;}
        }else{
            // Compatibility for pre-v51 histories: only metadata recorded
            // BEFORE this carrier, in the same generation, can identify it.
            for(const auto& candidate:prior)if(candidate.at("id")<row.at("id")&&candidate.value("event",std::string{})=="points_prepared"&&
                candidate.value("phone",std::string{})==phone&&candidate.value("generation",uint64_t{})==row.value("generation",uint64_t{})){
                owner=candidate;break;
            }
        }
        if(!owner.is_null()){connection=row;break;}
    }
    uint64_t previousCarrier=0;
    if(!connection.is_null()){
        for(const auto& row:prior)if(row.value("event",std::string{})=="connected"&&row.at("id")<connection.at("id")){
            previousCarrier=row.at("id").get<uint64_t>();break;
        }
        if(owner.value("game",0u)==game&&owner.value("profile",-1)>=0&&owner.value("profile",-1)<4){
            for(const auto& row:prior)if(row.at("id")<connection.at("id")&&row.at("id").get<uint64_t>()>previousCarrier&&
                row.value("event",std::string{})=="points_prepared"&&row.value("phone",std::string{})==phone&&
                row.value("profile",-1)==owner.at("profile").get<int>()&&row.value("game",0u)==game&&
                row.value("generation",uint64_t{})==connection.value("generation",uint64_t{})){
                prepared=row;break;
            }
        }
    }
    context["attribution_version"]=2;
    context["reporting_profile"]=profile;context["reporting_name"]=context.value("name",std::string{});
    if(!prepared.is_null()){
        context["profile"]=owner.at("profile");context["name"]=owner.value("name",std::string{});
        context["match_connection_id"]=connection.at("id");context["match_generation"]=connection.value("generation",uint64_t{});
    }
    // Deduplicate within a real match, regardless of the reporting user.
    const uint64_t source=prepared.is_null()?0:prepared.at("id").get<uint64_t>();
    const uint32_t local=LocalTCPProbe::longword(result,12)+LocalTCPProbe::longword(result,16);
    const uint32_t remote=LocalTCPProbe::longword(result,20)+LocalTCPProbe::longword(result,24);
    context["game"]=game;context["title"]=configured->fields[0];context["event"]="points_result";
    context["report_fingerprint"]=fingerprint;context["local_result"]=local;context["remote_result"]=remote;
    context["result_outcome"]=local>remote?"勝ち":local<remote?"負け":"引き分け";
    context["point_status"]="結果受信・加算額未確定";
    if(source)context["award_source_id"]=source;
    bool connected=!prepared.is_null(),ended=false;
    if(connected)for(const auto& row:prior){
        if(row.value("game",0u)!=game||row.value("generation",uint64_t{})!=prepared.value("generation",uint64_t{}))continue;
        if(row.at("id")<=connection.at("id"))continue;
        ended|=row.value("event",std::string{})=="hangup";
    }
    if(connected&&ended){const auto win=prepared.value("configured_win_points",prepared.value("configured_points",-1));
        const auto lose=prepared.value("configured_lose_points",0);
        if(win>=0&&win<=999&&lose>=0&&lose<=999){
            const auto award=local>remote?win:local<remote?lose:0;
            context["configured_win_points"]=win;context["configured_lose_points"]=lose;
            context["configured_points"]=award;context["points_delta"]=award;
            context["point_status"]="サーバー設定による算出・ゲームDISC(ROM)反映未確認";context["award_source_id"]=prepared.at("id");}
    }
    for(const auto& row:prior)if(row.value("attribution_version",0)==2&&row.value("event",std::string{})=="points_result"&&
        row.value("phone",std::string{})==phone&&row.value("profile",-1)==context.at("profile").get<int>()&&row.value("game",0u)==game&&
        row.value("award_source_id",uint64_t{})==source&&row.value("report_fingerprint",std::string{})==fingerprint&&
        row.contains("points_delta")==context.contains("points_delta"))return false;
    if(reportedAt)context["reported_unix_ms"]=reportedAt;
    context["detail"]="端末共通012Aの結果を、直近の実際の対戦参加者と開始時設定に照合。接続時の選択ユーザーはreporting_profileに別記録。これはサーバー照合方針であり、結果内ユーザーIDや当時の本番仕様を解明したものではありません。照合不能・回線未終了は加算しません。";
    return history.append(std::move(context));
}
}
