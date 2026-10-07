#pragma once
#include <windows.h>
// Diagnostic-process pacing only. No global timer-resolution change and no
// guest-clock change. A normal Sleep(1) can round up to a scheduler tick.
class PB3Wait {
    HANDLE timer=CreateWaitableTimerExW(nullptr,nullptr,0x00000002,TIMER_ALL_ACCESS);
public:
    ~PB3Wait(){if(timer)CloseHandle(timer);}
    PB3Wait()=default;
    PB3Wait(const PB3Wait&)=delete;
    void pause(){
        LARGE_INTEGER due;due.QuadPart=-10000; // relative one millisecond
        if(timer&&SetWaitableTimer(timer,&due,0,nullptr,nullptr,FALSE))
            WaitForSingleObject(timer,10);
        else Sleep(1);
    }
};
