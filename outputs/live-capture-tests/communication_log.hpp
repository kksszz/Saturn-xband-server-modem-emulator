#pragma once
#include <xband/service_endpoint.hpp>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <atomic>

namespace diagnostic {
// Diagnostic journal only. No file IO or network waits in endpoint callbacks.
class CommunicationLog {
    using J=nlohmann::json;
    struct Buffer { std::string channel,direction; unsigned side=0,frame=0; uint64_t session=0,time=0; std::vector<uint8_t> bytes; };
    std::filesystem::path root;
    mutable std::mutex mutex;
    std::condition_variable wake;
    std::map<std::string,Buffer> buffers;
    std::deque<J> queue;
    std::vector<std::filesystem::path> pages;
    std::thread writer;
    bool closing=false;
    std::atomic<bool> failed{false};
    uint64_t records=0,nextSession=0;
    std::string error;
    static uint64_t now(){return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();}
    void enqueue(Buffer& b){
        if(b.bytes.empty())return;
        if(queue.size()>=4096){failed=true;error="Capture stopped: writer queue overflow (log is incomplete)";b.bytes.clear();return;}
        std::ostringstream hex,text;
        hex<<std::hex<<std::uppercase<<std::setfill('0');
        constexpr char digits[]="0123456789ABCDEF";
        for(auto v:b.bytes){hex<<std::setw(2)<<unsigned(v)<<' ';if(v=='\\')text<<"\\\\";else if(v>=32&&v<=126)text<<char(v);else text<<"\\x"<<digits[v>>4]<<digits[v&15];}
        queue.push_back(J{{"version",1},{"unix_ms",b.time},{"channel",b.channel},{"side",b.side},{"direction",b.direction},{"session",b.session},{"guest_frame",b.frame},{"hex",hex.str()},{"text",text.str()}});
        b.bytes.clear();
    }
    void run() noexcept {
        try{
            std::ofstream out;size_t pageRows=200;
            for(;;){
                std::deque<J> batch;bool finish;
                {std::unique_lock lock(mutex);wake.wait_for(lock,std::chrono::milliseconds(100),[&]{return closing||!queue.empty();});
                    for(auto& [key,b]:buffers)enqueue(b);
                    batch.swap(queue);finish=closing;}
                for(auto& row:batch){
                    if(pageRows==200){
                        if(out.is_open())out.close();
                        std::filesystem::path path;
                        {std::lock_guard lock(mutex);std::ostringstream name;name<<std::setw(10)<<std::setfill('0')<<pages.size()+1<<".jsonl";path=root/name.str();}
                        out.open(path,std::ios::binary|std::ios::out|std::ios::trunc);if(!out)throw std::runtime_error("Cannot open communication log");
                        {std::lock_guard lock(mutex);pages.push_back(path);}pageRows=0;
                    }
                    out<<row.dump()<<'\n';if(!out)throw std::runtime_error("Cannot write communication log");
                    ++pageRows;{std::lock_guard lock(mutex);++records;}
                }
                if(out.is_open()){out.flush();if(!out)throw std::runtime_error("Cannot flush communication log");}
                if(finish)break;
            }
        }catch(const std::exception& e){std::lock_guard lock(mutex);failed=true;error=std::string("Capture stopped: ")+e.what()+" (log is incomplete)";}
    }
public:
    explicit CommunicationLog(const std::filesystem::path& base){
        // Never truncate an earlier run, even with the same clock value.
        std::filesystem::create_directories(base);
        for(unsigned n=0;;++n){root=base/("run-"+std::to_string(now())+"-"+std::to_string(n));if(std::filesystem::create_directory(root))break;}
        writer=std::thread([this]{run();});
    }
    ~CommunicationLog(){stop();}
    void stop(){if(!writer.joinable())return;{std::lock_guard lock(mutex);closing=true;}wake.notify_one();writer.join();}
    uint64_t newSession(){std::lock_guard lock(mutex);return ++nextSession;}
    void byte(const std::string& channel,unsigned side,const std::string& direction,uint8_t value,unsigned frame,uint64_t session)noexcept{
        if(failed)return;
        try{
            std::lock_guard lock(mutex);if(closing)return;
            auto& b=buffers[channel+"/"+std::to_string(side)+"/"+direction];
            if(!b.bytes.empty()&&(b.frame!=frame||b.session!=session))enqueue(b);
            if(failed)return;
            if(b.bytes.empty()){b.channel=channel;b.side=side;b.direction=direction;b.frame=frame;b.session=session;b.time=now();}
            b.bytes.push_back(value);if(b.bytes.size()==64){enqueue(b);wake.notify_one();}
        }catch(...){failed=true;}
    }
    struct Snapshot {std::filesystem::path directory;std::vector<std::filesystem::path> pages;uint64_t records=0;std::string error;};
    Snapshot snapshot()const{std::lock_guard lock(mutex);return {root,pages,records,failed?(error.empty()?"Capture stopped: log is incomplete":error):std::string{}};}
    static std::vector<J> readPage(const std::filesystem::path& path){
        std::ifstream in(path,std::ios::binary);std::vector<J> rows;std::string line;
        // Each page is bounded; a concurrently written incomplete final row is
        // ignored until the next refresh, never used as protocol input.
        while(std::getline(in,line)){
            if(line.size()>4096)continue;
            auto row=J::parse(line,nullptr,false);
            if(row.is_object()&&row.contains("version")&&row["version"]==1&&row.contains("channel")&&row["channel"].is_string()&&
               row.contains("direction")&&row["direction"].is_string()&&row.contains("hex")&&row["hex"].is_string()&&
               row.contains("text")&&row["text"].is_string()&&row.contains("side")&&row["side"].is_number_unsigned()&&
               row.contains("session")&&row["session"].is_number_unsigned()&&row.contains("guest_frame")&&row["guest_frame"].is_number_unsigned()&&
               row.contains("unix_ms")&&row["unix_ms"].is_number_unsigned())rows.push_back(std::move(row));
            if(rows.size()==200)break;
        }
        return rows;
    }
};
class LoggedEndpoint final:public xband::ServiceEndpoint {
    std::unique_ptr<xband::ServiceEndpoint> inner;
    std::shared_ptr<CommunicationLog> log;
    std::string channel;unsigned side,frame=0;uint64_t session;
public:
    LoggedEndpoint(std::unique_ptr<xband::ServiceEndpoint> endpoint,std::shared_ptr<CommunicationLog> journal,std::string category,unsigned endpointSide)
        :inner(std::move(endpoint)),log(std::move(journal)),channel(std::move(category)),side(endpointSide),session(log->newSession()){}
    bool transmit(uint8_t b,unsigned f)override{if(!inner->transmit(b,f))return false;frame=f;log->byte(channel,side,"TX",b,f,session);return true;}
    void tick(unsigned f)override{frame=f;inner->tick(f);}
    bool peek(uint8_t& b)const override{return inner->peek(b);}
    void consume()override{uint8_t b;if(inner->peek(b))log->byte(channel,side,"RX",b,frame,session);inner->consume();}
    size_t pending()const override{return inner->pending();}
    void reset()override{inner->reset();session=log->newSession();}
};
inline std::unique_ptr<xband::ServiceEndpoint> logEndpoint(std::unique_ptr<xband::ServiceEndpoint> endpoint,
    const std::shared_ptr<CommunicationLog>& journal,std::string category,unsigned side){
    if(!journal)return endpoint;
    return std::make_unique<LoggedEndpoint>(std::move(endpoint),journal,std::move(category),side);
}
}
