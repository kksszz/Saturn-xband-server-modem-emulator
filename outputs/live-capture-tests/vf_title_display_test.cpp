#define NOMINMAX
#include "game_ranking_settings.hpp"
#include <iostream>

int main(){try{
    auto check=[](bool ok){if(!ok)throw std::runtime_error("VF title assertion");};
    const auto title=diagnostic::rankingUTF8(L"VF\u2122 Remix");
    const auto rankingTitle=diagnostic::rankingUTF8(L"Virtua Fighter\u2122 Remix");
    const std::string encoded="VF\x85 Remix";
    const auto directory=std::filesystem::temp_directory_path()/
        ("xband-vf-title-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64()));
    diagnostic::GameRankingSettings defaults(directory/"absent.json");
    auto rows=defaults.snapshot();
    check(rows[0].gameID==0x10003&&rows[0].slot==1&&rows[0].fields[0]==rankingTitle);
    check(diagnostic::rankingEUC(title)==encoded);
    auto intro=defaults.introTitleWire(0x10003);
    check(intro==std::vector<uint8_t>({0x32,0,0x8b,0,0,0,10,'V','F',0x85,' ','R','e','m','i','x',0}));
    const auto ranking=defaults.wire(0x10003);
    check(std::string(reinterpret_cast<const char*>(ranking.data()+11))==std::string("Virtua Fighter\x85 Remix"));
    for(size_t i=1;i<rows.size();++i){
        const auto other=defaults.introTitleWire(rows[i].gameID);
        check(std::string(reinterpret_cast<const char*>(other.data()+7))==diagnostic::rankingEUC(rows[i].fields[0]));
    }
    std::filesystem::create_directories(directory);
    nlohmann::json games=nlohmann::json::array();
    for(auto row:rows){
        if(row.gameID==0x10003){row.fields[0]=diagnostic::rankingUTF8(L"Virtua Fighter\u2122 Remix");
            row.fields[2]="1234";row.fields[4]="5678";row.winPoints=7;row.losePoints=2;}
        games.push_back({{"game_id",row.gameID},{"fields",row.fields},{"win_points",row.winPoints},{"lose_points",row.losePoints}});
    }
    auto path=directory/"legacy.json";
    nlohmann::json document={{"schema",1},{"games",games}};
    auto write=[&]{std::ofstream out(path,std::ios::binary);out<<document.dump();out.close();check(bool(out));};
    write();
    diagnostic::GameRankingSettings migrated(path);
    const auto actual=migrated.snapshot();
    check(actual[0].fields[0]==rankingTitle&&actual[0].fields[2]=="1234"&&actual[0].fields[4]=="5678");
    check(actual[0].winPoints==7&&actual[0].losePoints==2&&actual[0].slot==1);
    for(size_t i=1;i<rows.size();++i)check(actual[i].fields==rows[i].fields&&actual[i].winPoints==rows[i].winPoints&&actual[i].losePoints==rows[i].losePoints);
    {nlohmann::json saved;std::ifstream in(path);in>>saved;check(saved==document);}
    check(migrated.introTitleWire(0x10003)==intro);
    const auto savedRanking=migrated.wire(0x10003);
    check(std::string(reinterpret_cast<const char*>(savedRanking.data()+11))==std::string("Virtua Fighter\x85 Remix"));
    migrated.update(0x10003,actual[0].fields);
    diagnostic::GameRankingSettings reloaded(path);
    check(reloaded.snapshot()[0].fields==actual[0].fields&&reloaded.snapshot()[0].winPoints==7&&reloaded.snapshot()[0].losePoints==2);
    document["games"][0]["fields"][0]="CUSTOM VF";write();
    diagnostic::GameRankingSettings custom(path);
    check(custom.snapshot()[0].fields[0]=="CUSTOM VF");
    const auto customIntro=custom.introTitleWire(0x10003);
    check(std::string(reinterpret_cast<const char*>(customIntro.data()+7))=="CUSTOM VF");
    // Clean up only files in this test's unique temporary directory.
    std::filesystem::remove(path);auto backup=path;backup+=L".bak";std::filesystem::remove(backup);std::filesystem::remove(directory);
    std::cout<<"PASS VF intro-only special TM title; rankings, other titles, custom names, points and saved-file preservation\n";
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
