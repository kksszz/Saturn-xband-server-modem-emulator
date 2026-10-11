#include "requested_bus_bridge.hpp"
#include "uart_receive_irq.hpp"
#include <iostream>
#include <stdexcept>
#include <deque>
int main(){
    using namespace xband;
    unsigned checks=0;
    auto check=[&](bool ok){++checks;if(!ok)throw std::runtime_error("Mednafen requested-bus contract failed");};
    ModemRegisterBank bank;
    mednafen_probe::RequestedBusBridge bridge(bank);
    bridge.mode.identify=bridge.mode.uart=bridge.mode.flash=bridge.mode.storage=true;
    std::deque<uint8_t> rx{0x31,0x32};
    unsigned readCount=0,writeCount=0;uint32_t lastAddress=0;uint8_t lastValue=0;
    bridge.uartRead=[&](uint32_t a){++readCount;lastAddress=a;auto v=rx.front();rx.pop_front();return v;};
    bridge.uartWrite=[&](uint32_t a,uint8_t v){++writeCount;lastAddress=a;lastValue=v;};
    uint32_t db=0xaabbccdd;
    check(bridge.ownsRead(0x05895001,1)&&bridge.ownsRead(0x05895001,1)&&readCount==0&&rx.size()==2);
    check(!bridge.ownsRead(0x05895001,1,true)&&!bridge.ownsWrite(0x05895001,1,true));
    check(!bridge.ownsRead(0x05895000,4)&&!bridge.ownsWrite(0x05895000,4));
    check(!bridge.ownsRead(0x05800018,2)&&!bridge.ownsWrite(0x05800018,2));
    check(bridge.read(0x05885029,1,db)&&db==0xaa11ccdd&&readCount==0);
    db=0xaabbccdd;
    check(bridge.read(0x05895001,1,db)&&db==0xaa31ccdd&&readCount==1&&rx.size()==1);
    db=0xaabbccdd;
    check(!bridge.read(0x05895001,1,db,true)&&db==0xaabbccdd&&readCount==1);
    check(!bridge.read(0x05895000,2,db)&&readCount==1);
    check(!bridge.read(0x05895000,4,db)&&readCount==1);
    check(!bridge.read(0x05895000,1,db)&&readCount==1);
    check(!bridge.read(0x05800018,2,db)&&db==0xaabbccdd); // CD fallback retained.
    check(bridge.write(0x05895001,1,0xaabeccdd)&&writeCount==1&&lastValue==0xbe);
    check(!bridge.write(0x05895001,1,0xaa99ccdd,true)&&writeCount==1);
    check(!bridge.write(0x05895000,4,0xffffffff)&&writeCount==1);
    for(unsigned i=0;i<4;++i)bank.flashData[i]=uint8_t(0x10+i);
    for(unsigned i=0;i<4;++i){
        db=0xaabbccdd;const auto shift=(3-i)*8;
        check(bridge.read(0x04000000+i,1,db)&&
            db==((0xaabbccddu&~(0xffu<<shift))|((0x10u+i)<<shift)));
    }
    db=0xaabbccdd;check(bridge.read(0x04000000,2,db)&&db==0x1011ccdd);
    db=0xaabbccdd;check(bridge.read(0x04000002,2,db)&&db==0xaabb1213);
    check(bridge.read(0x04000000,4,db)&&db==0x10111213);
    check(!bridge.read(0x04000001,2,db));
    check(!bridge.read(0x04000000,3,db));
    check(!bridge.read(0x04020000,1,db));
    bridge.mode={};db=0xaabbccdd;
    check(!bridge.read(0x05895001,1,db)&&db==0xaabbccdd&&readCount==1);
    check(!bridge.write(0x05895001,1,0xaa99ccdd)&&writeCount==1);
    // Exercise the common UART through the adapter, not just mock callbacks.
    bridge.mode.identify=bridge.mode.uart=bridge.mode.flash=bridge.mode.storage=true;
    bridge.uartRead=[&](uint32_t a){return bank.readRegister(a,true,0x01,false,0x60,[](uint8_t){});};
    unsigned txCount=0;uint8_t txByte=0;
    bridge.uartWrite=[&](uint32_t a,uint8_t v){
        bank.writeRegister(a,v,true,[&](uint8_t b){++txCount;txByte=b;},[]{},[]{});
    };
    check(bridge.write(0x0589500d,1,0x00800000)&&bank.lcr==0x80);
    check(bridge.write(0x05895001,1,0x00340000)&&bank.dll==0x34&&txCount==0);
    db=0xffffffff;check(bridge.read(0x05895001,1,db)&&db==0xff34ffff);
    check(bridge.write(0x0589500d,1,0x00030000)&&bank.lcr==3);
    check(bridge.write(0x05895001,1,0x00410000)&&txCount==1&&txByte==0x41);
    bank.rx={0x42,0x43};db=0;
    check(bridge.read(0x05895015,1,db)&&db==0x00610000&&bank.rx.size()==2);
    db=0;check(bridge.read(0x05895001,1,db)&&db==0x00420000&&bank.rxConsumed==1);
    check(bridge.write(0x05895009,1,0x00020000)&&bank.rx.empty());
    // Flash command byte selection, identification, program and reset.
    auto flashWrite=[&](uint32_t a,uint8_t v){
        const auto shift=(3-(a&3))*8;
        return bridge.write(a,1,uint32_t(v)<<shift);
    };
    check(flashWrite(0x04005555,0xaa)&&flashWrite(0x04002aaa,0x55)&&
          flashWrite(0x04005555,0x90)&&bank.flashIdentify);
    db=0;check(bridge.read(0x04000000,1,db)&&db==0x1f000000);
    db=0;check(bridge.read(0x04000001,1,db)&&db==0x00d50000);
    check(flashWrite(0x04000000,0xf0)&&!bank.flashIdentify);
    check(flashWrite(0x04005555,0xaa)&&flashWrite(0x04002aaa,0x55)&&
          flashWrite(0x04005555,0xa0)&&bank.programBytes==128);
    check(flashWrite(0x04000010,0x5a)&&bank.flashData[0x10]==0x5a&&bank.programBytes==127);
    const auto remaining=bank.programBytes;
    check(!bridge.write(0x04000010,1,0xffffffff,true)&&bank.programBytes==remaining);
    bridge.uartRead={};bridge.uartWrite={};db=0xabcdef12;
    check(!bridge.read(0x05895001,1,db)&&db==0xabcdef12);
    check(!bridge.write(0x05895001,1,0xffffffff));
    bank.resetUART();
    check(!xband::mednafen_probe::receiveIRQ(bank));
    bank.rx={0x42};check(!xband::mednafen_probe::receiveIRQ(bank));
    bank.ier=1;check(xband::mednafen_probe::receiveIRQ(bank));
    unsigned input=99;bool active=false;
    xband::mednafen_probe::updateReceiveIRQ(bank,[&](unsigned i,bool a){input=i;active=a;});
    check(input==12&&active);
    bank.rx.clear();check(!xband::mednafen_probe::receiveIRQ(bank));
    bank.rx={0x42};bank.ier=2;check(!xband::mednafen_probe::receiveIRQ(bank));
    bank.resetUART();
    xband::mednafen_probe::updateReceiveIRQ(bank,[&](unsigned i,bool a){input=i;active=a;});
    check(input==12&&!active);
    unsigned cardReads=0,cardWrites=0;uint8_t lastCardWrite=0;
    bridge.cardRead=[&](uint8_t fallback){++cardReads;return uint8_t(fallback|0x11);};
    bridge.cardWrite=[&](uint8_t value){++cardWrites;lastCardWrite=value;};
    check(bridge.ownsRead(0x05885025,1)&&bridge.ownsWrite(0x05885021,1));
    check(cardReads==0&&cardWrites==0);
    db=0xabcdef12;check(bridge.read(0x05885025,1,db)&&db==0xabddef12&&cardReads==1);
    check(bridge.write(0x05885021,1,0x008d0000)&&cardWrites==1&&lastCardWrite==0x8d);
    check(!bridge.ownsRead(0x05885024,2)&&!bridge.ownsRead(0x05885024,4));
    check(!bridge.ownsWrite(0x05885020,2)&&!bridge.ownsWrite(0x05885020,4));
    check(!bridge.read(0x05885025,1,db,true)&&cardReads==1);
    check(!bridge.write(0x05885021,1,0,true)&&cardWrites==1);
    bridge.cardRead={};bridge.cardWrite={};
    check(!bridge.ownsRead(0x05885025,1)&&!bridge.ownsWrite(0x05885021,1));
    std::cout<<"PASS "<<checks<<" requested-width/byte-lane/FIFO/debug/fallback contracts; not a full emulator test\n";
}
