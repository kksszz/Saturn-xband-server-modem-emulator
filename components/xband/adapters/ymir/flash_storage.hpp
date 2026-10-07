#pragma once
#include <windows.h>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace xband::ymir_adapter {
// One physical modem image per frontend profile, shared across its games.
// Exclusive profile lock prevents two clients overwriting the same modem.
class FlashStorage {
public:
    using Image=std::array<uint8_t,0x20000>;
    explicit FlashStorage(std::filesystem::path path):path_(std::move(path)) {
        std::filesystem::create_directories(path_.parent_path());
        auto lockPath=path_;lockPath+=L".lock";
        lock_=CreateFileW(lockPath.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(lock_==INVALID_HANDLE_VALUE)throw std::runtime_error("XBAND modem save is locked or inaccessible");
        try {
            if(std::filesystem::exists(path_)) {
                if(std::filesystem::file_size(path_)!=Image{}.size())throw std::runtime_error("Invalid XBAND modem save size; existing file preserved");
                Image image;
                std::ifstream in(path_,std::ios::binary);
                if(!in.read(reinterpret_cast<char*>(image.data()),image.size()))throw std::runtime_error("Cannot read XBAND modem save; existing file preserved");
                saved_=image;
            }
        }catch(...){CloseHandle(lock_);lock_=INVALID_HANDLE_VALUE;throw;}
    }
    ~FlashStorage(){if(lock_!=INVALID_HANDLE_VALUE)CloseHandle(lock_);}
    FlashStorage(const FlashStorage&)=delete;
    FlashStorage& operator=(const FlashStorage&)=delete;
    const std::optional<Image>& loaded()const{return saved_;}
    bool save(const Image &image) {
        if(saved_&&*saved_==image)return false;
        auto temporary=path_;temporary+=L".tmp";
        HANDLE file=CreateFileW(temporary.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(file==INVALID_HANDLE_VALUE)throw std::runtime_error("Cannot create temporary XBAND modem save");
        DWORD written=0;
        const bool complete=WriteFile(file,image.data(),DWORD(image.size()),&written,nullptr)&&written==image.size()&&FlushFileBuffers(file);
        CloseHandle(file);
        if(!complete)throw std::runtime_error("Cannot flush XBAND modem save; previous image preserved");
        if(std::filesystem::exists(path_)) {
            auto backup=path_;backup+=L".bak";
            if(!CopyFileW(path_.c_str(),backup.c_str(),FALSE))throw std::runtime_error("Cannot back up XBAND modem save; previous image preserved");
        }
        if(!MoveFileExW(temporary.c_str(),path_.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("Cannot replace XBAND modem save; previous image preserved");
        saved_=image;
        return true;
    }
private:
    std::filesystem::path path_;
    HANDLE lock_=INVALID_HANDLE_VALUE;
    std::optional<Image> saved_;
};
}
