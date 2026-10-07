#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

// One pending snapshot (latest wins). The worker never touches emulator objects.
// This is monitoring transport, not an event history or a command API.
class ModemStatusChannel {
public:
    using Sink=std::function<void(const std::string&)>;
    explicit ModemStatusChannel(Sink sink):sink(std::move(sink)),worker([this]{run();}) {}
    ModemStatusChannel(const ModemStatusChannel&)=delete;
    ModemStatusChannel& operator=(const ModemStatusChannel&)=delete;
    ~ModemStatusChannel(){close();}
    bool publish(std::string value) {
        if(value.empty()||value.size()>16384)return false;
        {std::lock_guard lock(mutex);if(stopping)return false;pending=std::move(value);}
        wake.notify_one();return true;
    }
    void close() {
        {std::lock_guard lock(mutex);stopping=true;}
        wake.notify_one();if(worker.joinable())worker.join();
    }
    uint64_t failures() const {return failed.load();}
    uint64_t writes() const {return written.load();}
    static Sink fileSink(std::filesystem::path path) {
        if(path.extension()!=L".status")throw std::invalid_argument("monitor output must use .status extension");
        return [path=std::move(path)](const std::string &value) {
            static std::atomic<unsigned long> serial{0};
            auto temporary=path;
            temporary+=L"."+std::to_wstring(GetCurrentProcessId())+L"."+std::to_wstring(++serial)+L".tmp";
            HANDLE file=CreateFileW(temporary.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
            if(file==INVALID_HANDLE_VALUE)throw std::runtime_error("cannot create monitor snapshot");
            DWORD count=0;
            const bool ok=WriteFile(file,value.data(),DWORD(value.size()),&count,nullptr)&&count==value.size();
            CloseHandle(file);
            bool replaced=false;
            if(ok)for(unsigned attempt=0;attempt<9;++attempt) {
                if(MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING)) {replaced=true;break;}
                const auto error=GetLastError();
                if(attempt==8 || (error!=ERROR_SHARING_VIOLATION && error!=ERROR_ACCESS_DENIED))break;
                // A reader may briefly open without FILE_SHARE_DELETE. Retry only
                // on the I/O worker, with a bounded 80ms total delay.
                Sleep(10);
            }
            if(!ok || !replaced) {
                DeleteFileW(temporary.c_str()); // Only our CREATE_NEW temporary file.
                throw std::runtime_error("cannot publish monitor snapshot");
            }
        };
    }
private:
    Sink sink;
    std::mutex mutex;
    std::condition_variable wake;
    std::optional<std::string> pending;
    bool stopping=false;
    std::atomic<uint64_t> failed{0},written{0};
    std::thread worker;
    void run() {
        for(;;) {
            std::string value;
            {std::unique_lock lock(mutex);wake.wait(lock,[&]{return stopping||pending.has_value();});
             if(!pending)return;
             value=std::move(*pending);pending.reset();}
            try {sink(value);++written;}catch(...){++failed;}
        }
    }
};
