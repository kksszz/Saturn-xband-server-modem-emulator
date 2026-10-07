#include <xband/mail_profile_names.hpp>
#include <array>
#include <iostream>

int main(){try{
    using Profiles=xband::MailProfileNames;
    using Name=Profiles::Name;
    using O=Profiles::Observation;
    auto check=[](bool ok){if(!ok)throw std::runtime_error("Profile names assertion failed");};
    auto invalid=[&](auto action){bool threw=false;try{action();}catch(const std::invalid_argument&){threw=true;}check(threw);};
    Profiles profiles(3);
    const Name zero{'8','8'},three{'N','3'},other{'N','0'};
    const std::array<uint8_t,2> mixed{0,3},reverse{3,0},duplicates{0,0};
    check(profiles.observe("modem-1","save-A",3,three)==O::added);
    check(profiles.resolve("modem-1","save-A",3)==three);
    check(!profiles.resolveBatch("modem-1","save-A",mixed));
    check(!profiles.resolve("modem-2","save-A",3));
    check(!profiles.resolve("modem-1","save-B",3));
    check(profiles.observe("modem-1","save-A",0,zero)==O::added);
    auto names=profiles.resolveBatch("modem-1","save-A",mixed);
    check(names&&*names==std::vector<Name>{zero,three});
    check(profiles.resolveBatch("modem-1","save-A",reverse)==std::vector<Name>{three,zero});
    check(profiles.resolveBatch("modem-1","save-A",duplicates)==std::vector<Name>{zero,zero});
    check(profiles.observe("modem-1","save-A",0,zero)==O::repeated&&profiles.size()==2);
    check(profiles.observe("modem-2","save-A",0,other)==O::added);
    check(profiles.observe("modem-1","save-B",0,other)==O::capacity_refused);
    check(!profiles.resolve("modem-1","save-B",0));
    check(profiles.observe("modem-1","save-A",0,other)==O::conflict);
    check(profiles.observe("modem-1","save-A",0,zero)==O::conflict);
    check(!profiles.resolveBatch("modem-1","save-A",mixed));
    check(profiles.resolve("modem-2","save-A",0)==other);
    check(profiles.resolve("modem-1","save-A",3)==three);
    invalid([&]{profiles.observe("","save-A",0,zero);});
    invalid([&]{profiles.observe("modem-1","",0,zero);});
    invalid([&]{profiles.observe(std::string("m\0x",3),"save-A",0,zero);});
    invalid([&]{profiles.observe("modem-1",std::string("e\0x",3),0,zero);});
    invalid([&]{profiles.observe("modem-1","save-A",4,zero);});
    invalid([&]{profiles.observe("modem-1","save-A",1,Name{});});
    invalid([&]{profiles.observe("modem-1","save-A",1,Name{1,0});});
    invalid([&]{profiles.observe("modem-1","save-A",1,Name(33,1));});
    invalid([&]{profiles.resolveBatch("modem-1","save-A",std::array<uint8_t,2>{1,255});});
    invalid([&]{profiles.resolveBatch("modem-1","save-A",Name(129,0));});
    check(profiles.size()==3);
    profiles.invalidateEndpoint("modem-1");check(profiles.size()==1);
    check(!profiles.resolve("modem-1","save-A",3));
    check(profiles.resolve("modem-2","save-A",0)==other);
    check(profiles.observe("modem-1","save-B",0,other)==O::added);
    const auto empty=profiles.resolveBatch("modem-1","save-B",std::span<const uint8_t>{});
    check(empty&&empty->empty());
    Profiles noCapacity(0);check(noCapacity.observe("m","e",0,zero)==O::capacity_refused);
    std::cout<<"PASS profile-name association: endpoint/epoch/player isolation, all-or-none order, unknown/conflict refusal, bounds, invalidation; synthetic fixtures, no authentication or live mail admission\n";
    return 0;
}catch(const std::exception &e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
