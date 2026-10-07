#include <xband/uart16550_timing.hpp>
#include <xband/guest_byte_pacer.hpp>
#include <xband/peer_uart_timing.hpp>
#include <iostream>
#include <stdexcept>
#include <string_view>
void check(bool ok,const char *why){if(!ok)throw std::runtime_error(why);}
int main(int argc,char **argv){
    try {
        if(argc!=2)return 2;
        using xband::uart16550CharacterNanoseconds;
        const std::string_view name=argv[1];
        if(name=="peer_uart_timing") {
            xband::PeerUartTiming p;unsigned delivered=0;
            auto sink=[&](uint8_t b){check(b==42,"byte preserved");++delivered;return true;};
            check(p.txStatus()==0x60&&p.push(42),"idle and enqueue");
            p.tick(0,10,sink);check(p.txStatus()==0x20,"holding empty but shifting");
            check(p.cyclesUntilEvent(0,0,0,0,10)==10&&p.cyclesUntilEvent(9,0,0,0,10)==1,"TX deadline budget");
            p.tick(9,10,sink);check(delivered==0,"no early delivery");
            p.tick(10,10,[](uint8_t){return false;});check(p.txStatus()==0x20,"backpressure retains shift");
            check(p.cyclesUntilEvent(10,0,0,0,10)==UINT64_MAX,"blocked TX does not busy loop");
            check(p.push(99),"enqueue second");p.clearHolding();
            p.tick(11,10,sink);check(delivered==1&&p.txStatus()==0x60,"FIFO clear preserves in-flight byte");
            for(unsigned i=0;i<16;++i)check(p.push(0),"bounded FIFO accepts 16");
            check(!p.push(0),"FIFO rejects overflow");p.reset();
            p.activity(100);
            check(p.cyclesUntilEvent(105,1,0xc1,13,10)==5,"RX timeout deadline");
            check(p.cyclesUntilEvent(110,1,0xc1,13,10)==UINT64_MAX,"expired timeout does not busy loop");
            check(p.cyclesUntilEvent(105,0,0xc1,13,10)==UINT64_MAX&&p.cyclesUntilEvent(105,1,0xc1,14,10)==UINT64_MAX,"disabled or threshold IRQ needs no timer");
            check(p.interrupt(1,0xc1,13,109,10)==1,"below trigger waits");
            check(p.interrupt(1,0xc1,13,110,10)==12,"timeout interrupt");
            check(p.interrupt(1,0xc1,14,100,10)==4,"threshold interrupt");
            check(p.interrupt(0,1,1,200,10)==1&&p.interrupt(1,1,0,200,10)==1,"disabled or empty");
            p.activity(110);check(p.interrupt(1,0xc1,1,119,10)==1,"read restarts timeout");
        }else if(name=="uart_timing_formats") {
            check(uart16550CharacterNanoseconds(1843200,8,0,3)==694445,"8N1 at diagnostic 14400");
            // clock16MHz, divisor1 gives a 1us bit; test every length/parity/stop.
            for(unsigned lcr=0;lcr<64;++lcr){
                const unsigned data=5+(lcr&3), stop=!(lcr&4)?2:data==5?3:4;
                const uint64_t expected=(2+2*data+((lcr&8)?2:0)+stop)*500;
                check(uart16550CharacterNanoseconds(16000000,1,0,static_cast<uint8_t>(lcr))==expected,"all frame formats");
                check(uart16550CharacterNanoseconds(16000000,1,0,static_cast<uint8_t>(lcr|0x80))==expected,"DLAB changes access only");
            }
            check(uart16550CharacterNanoseconds(16000000,0,1,3)==2560000,"DLM included");
        }else if(name=="uart_timing_limits") {
            check(!uart16550CharacterNanoseconds(0,8,0,3)&&!uart16550CharacterNanoseconds(1,0,0,3),"unknown clock/divisor rejected");
            check(!uart16550CharacterNanoseconds(1843200,8,0,0x43),"break unsupported");
            check(uart16550CharacterNanoseconds(1,255,255,0x0f)==12582720000000000ULL,"maximum frame numerator");
        }else if(name=="uart_timing_update") {
            xband::GuestBytePacer p(10);unsigned delivered=0;
            const auto sink=[&](uint8_t){++delivered;return true;};
            check(p.offer(1,0)&&p.setPeriod(20),"change with held byte");
            check(!p.step(9,sink)&&p.step(10,sink),"old deadline preserved");
            check(p.offer(2,10)&&!p.step(29,sink)&&p.step(30,sink)&&delivered==2,"new period for next byte");
            check(!p.setPeriod(0)&&p.failed(),"invalid update fails closed");
        }else throw std::runtime_error("unknown test");
        std::cout<<"PASS "<<name<<'\n';return 0;
    }catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}
}
