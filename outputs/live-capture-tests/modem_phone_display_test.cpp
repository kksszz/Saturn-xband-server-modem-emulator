#include "modem_phone_display.hpp"
#include <iostream>
int main(){try{
    using J=nlohmann::json;
    auto check=[](bool ok){if(!ok)throw std::runtime_error("Modem phone display assertion");};
    check(diagnostic::modemSubscriberDisplay(J::object())=="Not yet reported");
    check(diagnostic::modemSubscriberDisplay({{"verified",false},{"subscriber","stale-number"}})=="Not yet reported");
    check(diagnostic::modemSubscriberDisplay({{"verified",true},{"subscriber",""}})=="Not yet reported");
    check(diagnostic::modemSubscriberDisplay({{"verified",true},{"subscriber","012-345"}})=="012-345");
    J endpoints=J::array({{{"pb3_observation",{{"verified",true},{"subscriber","012-345"}}}},{{"pb3_observation",{{"verified",false},{"subscriber",""}}}}});
    check(diagnostic::modemRouteDisplay(endpoints,0)=="Game-request route: 1 -> 2\nCaller 012-345\nTarget Not yet reported");
    check(diagnostic::modemRouteDisplay(endpoints,1)=="Game-request route: 2 -> 1\nCaller Not yet reported\nTarget 012-345");
    check(diagnostic::modemRouteDisplay(endpoints,2)=="Route not assigned\nWaiting for game request");
    std::cout<<"PASS 7 modem phone display cases: no synthetic default, unverified hidden, reported numbers, both routes\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
