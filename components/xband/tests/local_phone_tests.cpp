#include <xband/local_phone_policy.hpp>
#include <xband/peer_route.hpp>
#include <array>
#include <algorithm>
#include <iostream>
void check(bool b){if(!b)throw std::runtime_error("phone policy test failed");}
int main(){try{
    using namespace xband;
    check(localSubscriber({},0)=="1"&&localSubscriber({},1)=="2");
    check(localSubscriber("03-12345678",0)=="0312345678");
    check(localSubscriber("0312345679",1)=="0312345679"); // Existing explicit subscribers remain valid.
    check(pairedPeerEndpoint(0)=="peer-1"&&pairedPeerEndpoint(1)=="peer-0");
    for(auto n:{"---","123X","123 456","123456789012345678901"}){
        bool denied=false;try{(void)localSubscriber(n,0);}catch(const std::invalid_argument&){denied=true;}check(denied);
    }
    bool badSide=false;try{(void)pairedPeerEndpoint(2);}catch(const std::invalid_argument&){badSide=true;}check(badSide);
    for(auto n:{"03-12345678","03-33333333","12345678","33333333","0312345679","0312345678"})
        for(unsigned side=0;side<2;++side)check(localDialRole(n,side,false)==LocalDialRole::selfCheck);
    check(localDialRole("0120-717360",0,false)==LocalDialRole::service);
    check(localDialRole("0120-717360",1,true)==LocalDialRole::service);
    check(localDialRole("0312345679",0,true)==LocalDialRole::peer);
    check(localDialRole("0312345678",1,true)==LocalDialRole::peer);
    for(auto n:{"03-12345678","03-33333333","1","0000000000","99999999999"})
        for(unsigned side=0;side<2;++side){check(localDialRole(n,side,true)==LocalDialRole::peer);check(localDialRole(n,side,false)==LocalDialRole::selfCheck);}
    for(auto n:{"","---","123X","123 456"})check(localDialRole(n,0,true)==LocalDialRole::reject);
    check(localDialRole("123X",0,false)==LocalDialRole::reject);
    std::array<uint8_t,489> b{};
    for(auto n:{"1","03-12345678","03-33333333","059-1234567","00-444444444444"}){b.fill(0);b[22]=uint8_t(std::char_traits<char>::length(n)+1);std::copy(n,n+std::char_traits<char>::length(n),b.begin()+23);check(registrationPhone(b)==n);}
    b.fill('1');bool caught=false;try{registrationPhone(b);}catch(...){caught=true;}check(caught);
    std::cout<<"Local phone policy passed\n";return 0;
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
