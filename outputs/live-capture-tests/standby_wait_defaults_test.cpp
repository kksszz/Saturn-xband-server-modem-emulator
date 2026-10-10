#define NOMINMAX
#include "standby_dialog.hpp"
#include <iostream>

int main(){try{
    auto check=[](bool ok){if(!ok)throw std::runtime_error("Standby defaults assertion");};
    const auto directory=std::filesystem::temp_directory_path()/
        ("xband-wait-defaults-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64()));
    const auto path=directory/"settings.json";
    diagnostic::StandbyWaitSettings settings(path);
    const diagnostic::StandbyWaitValues defaults{},oldSaved{{1,2,4}};
    check(defaults.minutes==std::array<unsigned,3>{5,10,12});
    check(settings.snapshot()==defaults&&!std::filesystem::exists(path));
    diagnostic::activeStandbyWait.reset();
    for(uint8_t index=0;index<3;++index){
        const auto ticks=defaults.minutes[index]*3600;
        check(settings.ticks(index)==ticks&&diagnostic::vfStandbyWaitTicks(index)==ticks);
        const auto dialog=diagnostic::standbyDialogWire("GAME",ticks);
        const std::string body(reinterpret_cast<const char*>(dialog.data()+13));
        const auto duration=diagnostic::rankingStandardEUC(diagnostic::rankingUTF8(L"\u7d04"))+
            std::to_string(defaults.minutes[index])+diagnostic::rankingStandardEUC(diagnostic::rankingUTF8(L"\u5206\u9593"));
        check(body.find(duration)!=body.npos);
    }
    settings.save(oldSaved);
    diagnostic::StandbyWaitSettings restored(path);
    check(restored.snapshot()==oldSaved);
    for(uint8_t index=0;index<3;++index)check(restored.ticks(index)==oldSaved.minutes[index]*3600);
    restored.save(defaults);
    check(diagnostic::StandbyWaitSettings(path).snapshot()==defaults);
    auto backup=path;backup+=L".bak";
    check(diagnostic::StandbyWaitSettings(backup).snapshot()==oldSaved);
    // Only remove files created in this test's unique temporary directory.
    std::filesystem::remove(path);std::filesystem::remove(backup);std::filesystem::remove(directory);
    std::cout<<"PASS defaults5/10/12; preference ticks and notice minutes; saved settings preserved; explicit save and backup\n";
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
