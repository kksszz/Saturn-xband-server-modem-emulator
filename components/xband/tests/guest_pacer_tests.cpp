#include <xband/guest_byte_pacer.hpp>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <vector>
void check(bool ok,const char *why){if(!ok)throw std::runtime_error(why);}
int main(int argc,char **argv){
    try {
        if(argc!=2)return 2;
        const std::string_view name=argv[1];xband::GuestBytePacer p(10);std::vector<uint8_t> out;
        const auto sink=[&](uint8_t b){out.push_back(b);return true;};
        if(name=="pacer_timing"){
            check(p.offer(1,100)&&!p.offer(2,100),"single byte bound");
            check(!p.step(109,sink)&&out.empty(),"not early");
            check(p.step(110,sink)&&!p.step(110,sink),"exact deadline once");
            check(p.offer(2,1000)&&!p.step(1000,sink),"idle creates no accumulated credit");
            check(p.step(5000,sink)&&p.offer(3,5000)&&!p.step(5000,sink),"late polling cannot burst");
            check(out==std::vector<uint8_t>({1,2}),"ordered");
        }else if(name=="pacer_backpressure"){
            check(p.offer(0xa5,0),"offer");
            check(!p.step(10,[](uint8_t){return false;})&&p.busy(),"rejected byte retained");
            check(!p.offer(2,11)&&p.step(12,sink)&&out==std::vector<uint8_t>({0xa5}),"no replacement");
            p.offer(2,12);p.reset();check(!p.step(100,sink)&&out.size()==1,"reset drops old byte");
            check(p.offer(3,100)&&p.step(110,sink),"reuse after reset");
        }else if(name=="pacer_failure"){
            p.offer(1,20);check(!p.step(19,sink)&&p.failed()&&!p.busy(),"reverse time fails");
            p.reset();check(!p.offer(1,UINT64_MAX-9)&&p.failed(),"deadline overflow");
            p.reset();p.offer(1,0);p.step(10,[](uint8_t)->bool{throw std::runtime_error("sink");});
            check(p.failed()&&!p.busy(),"uncertain side effects never replay");
            xband::GuestBytePacer zero(0);check(zero.failed()&&!zero.offer(1,0),"zero period rejected");
        }else throw std::runtime_error("unknown test");
        std::cout<<"PASS "<<name<<'\n';return 0;
    }catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}
}
