#pragma once
#include "game_point_history.hpp"
#include "game_level_settings.hpp"

namespace diagnostic {
// Local server policy, not a reconstruction of historical XBAND rating rules.
// Starts at zero; immutable prior audit rows are never retroactively awarded.
class GamePointLedger {
    using J=nlohmann::json;
    std::filesystem::path path;mutable std::mutex mutex;
    J state;
    static std::string accountKey(const std::string& phone,unsigned profile,uint32_t game){
        if(profile>3)throw std::runtime_error("Point ledger profile outside0..3");
        std::string digits;for(char c:phone){if(c>='0'&&c<='9')digits+=c;else if(c!='-')throw std::runtime_error("Invalid point ledger phone");}
        if(digits.empty()||digits.size()>32)throw std::runtime_error("Invalid point ledger phone length");
        return digits+":"+std::to_string(profile)+":"+std::to_string(game);
    }
    void commit(const J& next){
        if(next.at("accounts").size()>4096||next.at("receipts").size()>65536)throw std::runtime_error("Point ledger capacity reached; preserve receipts");
        std::filesystem::create_directories(path.parent_path().empty()?std::filesystem::path("."):path.parent_path());
        auto pending=path;pending+=L".pending";
        if(std::filesystem::exists(pending))throw std::runtime_error("Pending point ledger requires recovery");
        {std::ofstream out(pending,std::ios::binary);out<<next.dump(2);out.flush();if(!out)throw std::runtime_error("Point ledger write failed");}
        if(std::filesystem::exists(path)){auto backup=path;backup+=L".bak";if(!CopyFileW(path.c_str(),backup.c_str(),FALSE))throw std::runtime_error("Point ledger backup failed");}
        if(!MoveFileExW(pending.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))throw std::runtime_error("Point ledger commit failed");
        state=next;
    }
public:
    explicit GamePointLedger(std::filesystem::path file,uint64_t historyStart):path(std::move(file)){
        auto pending=path;pending+=L".pending";if(std::filesystem::exists(pending))throw std::runtime_error("Preserve incomplete point ledger");
        if(std::filesystem::exists(path)){
            if(std::filesystem::file_size(path)>32*1024*1024)throw std::runtime_error("Oversized point ledger");
            std::ifstream in(path);in>>state;
            if(state.at("schema")!=1||!state.at("start_after_history_id").is_number_unsigned()||state.at("start_after_history_id").get<uint64_t>()>historyStart||
               !state.at("accounts").is_object()||!state.at("receipts").is_object()||!state.at("consumed_awards").is_object())throw std::runtime_error("Invalid point ledger schema/history");
            if(state.at("accounts").size()>4096||state.at("receipts").size()>65536||state.at("consumed_awards").size()>65536)throw std::runtime_error("Oversized saved point ledger");
            for(const auto& value:state.at("accounts"))if(!value.is_number_unsigned()||value.get<uint64_t>()>999999999)throw std::runtime_error("Invalid saved point total");
            for(const auto& value:state.at("receipts"))if(!value.is_object()||!value.at("delta").is_number_unsigned()||value.at("delta").get<uint64_t>()>999||!value.at("total").is_number_unsigned()||value.at("total").get<uint64_t>()>999999999)throw std::runtime_error("Invalid saved point receipt");
            for(const auto& value:state.at("consumed_awards"))if(!value.is_string()||value.get<std::string>().size()!=64)throw std::runtime_error("Invalid saved award fingerprint");
        }else{
            state={{"schema",1},{"policy","server-zero-baseline-win-loss-awards"},{"start_after_history_id",historyStart},
                {"accounts",J::object()},{"receipts",J::object()},{"consumed_awards",J::object()}};commit(state);
        }
    }
    uint32_t points(const std::string& phone,unsigned profile,uint32_t game)const{
        const auto key=accountKey(phone,profile,game);std::lock_guard lock(mutex);
        return state.at("accounts").value(key,uint32_t{});
    }
    void observe(ActivityHistory& history,J context,const LocalTCPProbe::Bytes& request,const GameRankingSettings& settings){
        const auto registrationGame=receivedGameID(request);
        const auto result=LocalTCPProbe::observedGameResult(request);
        // Command20 is terminal-wide and can survive a disc/user switch.
        // Score its own game, using the existing carrier/participant audit;
        // the current registration only selects the service response title.
        const auto game=result.empty()?registrationGame:std::optional<uint32_t>(LocalTCPProbe::longword(result,4));
        if(!game)return;
        const auto rows=settings.snapshot();if(std::none_of(rows.begin(),rows.end(),[&](const auto&r){return r.gameID==*game;}))return;
        if(registrationGame&&*registrationGame!=*game)context["reporting_game"]=*registrationGame;
        context["game"]=*game;
        for(const auto& row:rows)if(row.gameID==*game)context["title"]=row.fields[0];
        J audit;
        const auto hash=result.empty()?std::string{}:pointFingerprint(result);
        if(!hash.empty())for(size_t page=0;;++page){const auto batch=history.page(page);
            for(const auto& row:batch.at("rows"))if(row.value("event",std::string{})=="points_result"&&row.value("report_fingerprint",std::string{})==hash&&
                row.value("attribution_version",0)==2&&row.value("phone",std::string{})==context.at("phone").get<std::string>()&&row.value("game",0u)==*game){audit=row;break;}
            if(!audit.is_null()||(page+1)*50>=batch.at("total").get<size_t>())break;
        }
        if(!audit.is_null()){
            context["reporting_profile"]=context.at("profile");context["reporting_name"]=context.value("name",std::string{});
            context["profile"]=audit.at("profile");context["name"]=audit.value("name",std::string{});
        }
        const auto key=accountKey(context.at("phone").get<std::string>(),context.at("profile").get<unsigned>(),*game);
        std::lock_guard lock(mutex);auto next=state;const bool created=!next["accounts"].contains(key);
        uint32_t total=next["accounts"].value(key,uint32_t{}),delta=0;std::string event="points_baseline";
        bool changed=created;next["accounts"][key]=total;
        const auto receipt=key+":"+std::to_string(audit.is_null()?0:audit.value("award_source_id",uint64_t{}))+":"+hash+
            (audit.contains("points_delta")?":matched-v2":":unresolved-v2");
        if(!hash.empty()){
            if(next["receipts"].contains(receipt)){event="points_duplicate";}
            else{
                event="points_ledger";
                const uint64_t cutoff=next.at("start_after_history_id");
                if(!audit.is_null()&&audit.value("id",uint64_t{})>cutoff&&audit.value("award_source_id",uint64_t{})>cutoff&&audit.contains("points_delta")){
                    const auto awardKey=key+":"+std::to_string(audit.at("award_source_id").get<uint64_t>());
                    if(!next["consumed_awards"].contains(awardKey)){
                        const auto configured=audit.value("configured_points",-1);const auto candidate=audit.at("points_delta").get<uint32_t>();
                        if(configured<0||configured>999||candidate>uint32_t(configured))throw std::runtime_error("Invalid correlated point award");
                        if(candidate>999999999-total)throw std::runtime_error("Point total overflow");
                        delta=candidate;total+=delta;next["consumed_awards"][awardKey]=hash;
                    }
                }
                next["receipts"][receipt]={{"delta",delta},{"total",total}};next["accounts"][key]=total;changed=true;
            }
        }
        if(changed)commit(next);
        if(!created&&hash.empty())return;
        context["event"]=event;context["points_delta"]=delta;context["points_total"]=total;
        context["point_status"]="サーバー保存済み・ゲームDISC(ROM)反映未確認";
        if(!hash.empty()&&!audit.contains("points_delta"))context["point_status"]="結果の照合未完了・加算なし";
        context["detail"]="ユーザー・ゲーム別のサーバー累積。初期値0。旧テスト値・開始前の結果は加算しません。ゲームDISC(ROM)への更新応答を準備しますが保存完了通知ではありません。";
        if(!hash.empty())context["report_fingerprint"]=hash;
        if(!audit.is_null())for(const char* field:{"configured_points","configured_win_points","configured_lose_points","award_source_id","result_outcome","local_result","remote_result","match_connection_id","match_generation","attribution_version"})if(audit.contains(field))context[field]=audit[field];
        if(!history.append(std::move(context)))throw std::runtime_error("Point ledger committed but history unavailable; preserve ledger");
    }
    std::vector<uint8_t> wire(const std::string& phone,uint32_t game,const GameRankingSettings& settings)const{
        const auto rows=settings.snapshot();const auto row=std::find_if(rows.begin(),rows.end(),[&](const auto&r){return r.gameID==game;});if(row==rows.end())return {};
        const auto unset=rankingUTF8(L"\u672a\u8a2d\u5b9a");std::vector<uint8_t> out;
        for(unsigned profile=0;profile<4;++profile){
            std::vector<uint8_t> body{uint8_t(game>>24),uint8_t(game>>16),uint8_t(game>>8),uint8_t(game),uint8_t(profile),0};
            const auto total=points(phone,profile,game);
            const auto level=activeGameLevels?activeGameLevels->display(game,total):GameLevelSettings::Display{};
            const std::array<std::string,5> fields{row->fields[0],level.rank?activeGameLevels->label(game,level.rank):unset,std::to_string(total),
                level.next?activeGameLevels->label(game,level.next):rankingUTF8(L"なし"),std::to_string(level.required)};
            for(const auto& text:fields){const auto euc=rankingEUC(text);body.insert(body.end(),euc.begin(),euc.end());body.push_back(0);}
            const uint16_t id=uint16_t(profile*64+row->slot);
            out.insert(out.end(),{0x25,uint8_t(id>>8),uint8_t(id),uint8_t(body.size()>>8),uint8_t(body.size())});
            out.insert(out.end(),body.begin(),body.end());out.insert(out.end(),4,0);
        }
        return out;
    }
};
inline std::shared_ptr<GamePointLedger> activeGamePoints;
}
