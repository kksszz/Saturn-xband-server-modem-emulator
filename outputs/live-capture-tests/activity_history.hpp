#pragma once
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <chrono>
#include <cstdio>
#include <vector>
#include <algorithm>
namespace diagnostic {
// Metadata only, independent of mail custody and protocol decisions.
// One immutable JSON file per event. No payload, secret or gameplay verdict.
class ActivityHistory {
    using J=nlohmann::json;
    std::filesystem::path root;mutable std::mutex mutex;
    std::vector<J> rows;std::string failure;
    bool durable=false;
public:
    explicit ActivityHistory(std::filesystem::path path,bool flushToDisk=false):root(std::move(path)),durable(flushToDisk){
        std::filesystem::create_directories(root);
        std::vector<std::filesystem::path> files;
        for(const auto& e:std::filesystem::directory_iterator(root)){
            if(e.path().extension()==L".pending")throw std::runtime_error("Incomplete activity event; preserve and inspect history");
            if(e.path().extension()==L".json")files.push_back(e.path());
        }
        std::sort(files.begin(),files.end());
        for(const auto& file:files){
            if(std::filesystem::file_size(file)>65536)throw std::runtime_error("Oversized activity event");
            std::ifstream in(file);J row;in>>row;
            if(row.at("version")!=1||row.at("id")!=rows.size()+1||!row.at("event").is_string()||!row.at("unix_ms").is_number_unsigned())
                throw std::runtime_error("Invalid activity event sequence");
            rows.push_back(std::move(row));
        }
    }
    bool append(J row)noexcept{
        try{
            std::lock_guard lock(mutex);if(!failure.empty())return false;
            const uint64_t id=rows.size()+1;
            row["version"]=1;row["id"]=id;
            row["unix_ms"]=uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
            auto name=std::to_wstring(id);name=std::wstring(20-name.size(),L'0')+name;
            const auto target=root/(name+L".json"),pending=root/(name+L".pending");
            if(std::filesystem::exists(target)||std::filesystem::exists(pending))throw std::runtime_error("Activity event target exists");
            {std::ofstream out(pending,std::ios::binary);out<<row.dump();out.flush();if(!out)throw std::runtime_error("Activity event write failed");}
            if(durable){const auto file=CreateFileW(pending.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
                if(file==INVALID_HANDLE_VALUE)throw std::runtime_error("Result database flush open failed");
                const bool flushed=FlushFileBuffers(file)!=FALSE;CloseHandle(file);
                if(!flushed)throw std::runtime_error("Result database flush failed");
                if(!MoveFileExW(pending.c_str(),target.c_str(),MOVEFILE_WRITE_THROUGH))throw std::runtime_error("Result database commit failed");
            }else std::filesystem::rename(pending,target);
            rows.push_back(std::move(row));return true;
        }catch(const std::exception& e){std::lock_guard lock(mutex);failure=e.what();std::fprintf(stderr,"XBAND_ACTIVITY_HISTORY_FAILURE %s\n",e.what());return false;}
        catch(...){return false;}
    }
    J page(size_t index,size_t size=50)const{
        if(!size||size>200)throw std::runtime_error("Invalid activity page size");
        std::lock_guard lock(mutex);const size_t last=rows.empty()?0:(rows.size()-1)/size;
        index=std::min(index,last);J result=J::array();
        for(size_t n=index*size;n<rows.size()&&n<(index+1)*size;++n)result.push_back(rows[rows.size()-1-n]);
        return {{"rows",result},{"total",rows.size()},{"page",index},{"page_size",size},{"error",failure}};
    }
};
}
