#include <xband/async_modem_link.hpp>
#include <iostream>
void check(bool value){if(!value)throw std::runtime_error("async modem test failed");}
template<class F> void rejects(F f){bool caught=false;try{f();}catch(const std::exception&){caught=true;}check(caught);}
int main(){try{
    xband::AsyncModemRelay r(9);const std::array<uint8_t,3> data{1,2,3};
    check(r.exchange(0,9,1,1000000000,data,256).empty()); // no peer-clock ceiling
    check(r.exchange(1,9,1,1,{},2)==std::vector<uint8_t>({1,2}));
    rejects([&]{r.exchange(1,9,1,1,{},256);}); // duplicate is not replayed
    rejects([&]{r.exchange(1,8,2,1,{},256);});
    rejects([&]{r.exchange(1,9,2,0,{},256);});
    check(r.exchange(1,9,2,2,{},256)==std::vector<uint8_t>({3}));
    rejects([&]{r.exchange(0,9,3,1000000000,{},256);});
    std::array<uint8_t,256> full{};
    for(unsigned i=2;i<18;++i)r.exchange(0,9,i,1000000000,full,0);
    check(r.pending(1)==4096);rejects([&]{r.exchange(0,9,18,1000000000,data,0);});
    check(r.exchange(1,9,3,2,{},256).size()==256);
    r.exchange(0,9,18,1000000000,data,0); // rejected request did not consume sequence
    xband::AsyncModemReceive rx;rx.accept(data,100,10);std::vector<uint8_t> delivered;
    auto sink=[&](uint8_t b){delivered.push_back(b);return true;};
    check(rx.cyclesUntilEvent(100)==10&&rx.cyclesUntilEvent(109)==1);
    rx.tick(109,10,sink);check(delivered.empty());rx.tick(110,10,sink);check(delivered==std::vector<uint8_t>{1});
    check(rx.cyclesUntilEvent(110)==10);
    rx.tick(1000,10,sink);rx.tick(1000,10,sink);check(delivered==std::vector<uint8_t>({1,2})); // no catch-up burst
    rx.tick(1010,10,[](uint8_t){return false;});check(rx.pending()==1&&rx.cyclesUntilEvent(1010)==UINT64_MAX);
    rx.tick(1010,10,sink);check(delivered==std::vector<uint8_t>({1,2,3})&&rx.cyclesUntilEvent(1010)==UINT64_MAX);
    rx.reset();for(unsigned i=0;i<16;++i)rx.accept(full,0,10);check(rx.credit()==0);
    rejects([&]{rx.accept(data,0,10);});rx.reset();check(rx.pending()==0&&rx.credit()==256);
    std::cout<<"PASS async modem independent clocks/order/generation/credit/receive pacing\n";return 0;
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
