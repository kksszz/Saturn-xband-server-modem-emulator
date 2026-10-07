#include <xband/local_phone_policy.hpp>
#include <array>
#include <algorithm>
#include <iostream>
void check(bool b){if(!b)throw std::runtime_error("phone policy test failed");}
int main(){try{
    using namespace xband;
    for(auto n:{"03-12345678","03-33333333","12345678","33333333","3336666665","3336666666"})
        for(unsigned side=0;side<2;++side)check(localDialRole(n,side,false)==LocalDialRole::selfCheck);
    check(localDialRole("0120-717360",0,false)==LocalDialRole::service);
    check(localDialRole("0120-717360",1,true)==LocalDialRole::service);
    check(localDialRole("3336666665",0,true)==LocalDialRole::peer);
    check(localDialRole("3336666666",1,true)==LocalDialRole::peer);
    for(auto n:{"03-12345678","03-33333333","1","0000000000","99999999999"})
        for(unsigned side=0;side<2;++side){check(localDialRole(n,side,true)==LocalDialRole::peer);check(localDialRole(n,side,false)==LocalDialRole::selfCheck);}
    for(auto n:{"","---","123X","123 456"})check(localDialRole(n,0,true)==LocalDialRole::reject);
    check(localDialRole("123X",0,false)==LocalDialRole::reject);
    std::array<uint8_t,489> b{};
    for(auto n:{"1","03-12345678","03-33333333","333-6666666","00-444444444444"}){b.fill(0);b[22]=uint8_t(std::char_traits<char>::length(n)+1);std::copy(n,n+std::char_traits<char>::length(n),b.begin()+23);check(registrationPhone(b)==n);}
    b.fill('1');bool caught=false;try{registrationPhone(b);}catch(...){caught=true;}check(caught);
    std::cout<<"Local phone policy passed\n";return 0;
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
