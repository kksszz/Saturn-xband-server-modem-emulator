#pragma once
#include <cstdint>
#include <vector>
namespace xband {
// Observed replay policy, not physical modem defaults: 60 emulated frames
// before/after +++. The owner drains out after each update.
struct ModemEscape {
    static constexpr unsigned guard=60;
    unsigned last=0,pending=0;
    std::vector<uint8_t> out;
    bool escaped=false;
    void reset(unsigned now){last=now;pending=0;out.clear();escaped=false;}
    void flush(){while(pending){out.push_back('+');--pending;}}
    void tick(unsigned now){
        if(pending&&now-last>=guard){
            if(pending==3){pending=0;escaped=true;}else flush();
        }
    }
    void feed(uint8_t value,unsigned now){
        tick(now);if(escaped)return;
        const bool idle=now-last>=guard;
        if(value=='+'&&((pending>0&&pending<3)||(!pending&&idle)))++pending;
        else{flush();out.push_back(value);}
        last=now;
    }
};
}
