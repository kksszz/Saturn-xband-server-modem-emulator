#pragma once
#include <xband/mail_capture_snapshot.hpp>
#include <xband/mail_profile_restart_snapshot.hpp>
#include <filesystem>
#include <fstream>
#include <windows.h>
namespace diagnostic {
// Windows file adapter, separate from the portable codec/store. Explicit path
// only. A missing file needs explicit NEW permission; corrupt or lost restore
// files never silently become empty stores with reused identifiers.
inline xband::MailCaptureStore loadMailSnapshot(const std::filesystem::path &path,bool allowNew=false){
    if(path.empty())throw std::runtime_error("Empty snapshot path");
    if(!std::filesystem::exists(path)){
        if(!allowNew)throw std::runtime_error("Snapshot restore file missing; new-store creation must be explicit");
        return xband::MailCaptureStore{};
    }
    const auto size=std::filesystem::file_size(path);
    if(size<14||size>256*1024)throw std::runtime_error("Snapshot file size limit");
    std::ifstream file(path,std::ios::binary);if(!file)throw std::runtime_error("Snapshot read failed");
    std::vector<uint8_t> bytes(size);file.read(reinterpret_cast<char*>(bytes.data()),size);
    if(file.gcount()!=size||file.peek()!=std::char_traits<char>::eof())throw std::runtime_error("Snapshot file changed while reading");
    return xband::decodeMailCaptureSnapshot(bytes);
}
inline void commitMailSnapshot(const std::filesystem::path &path,const xband::MailCaptureStore &store){
    const auto bytes=xband::encodeMailCaptureSnapshot(store);
    auto pending=path;pending+=L".pending";
    const HANDLE file=CreateFileW(pending.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE)throw std::runtime_error("Snapshot pending-file creation failed (existing pending file is preserved)");
    DWORD written=0;const bool ready=WriteFile(file,bytes.data(),DWORD(bytes.size()),&written,nullptr)&&written==bytes.size()&&FlushFileBuffers(file);
    CloseHandle(file);
    if(!ready){DeleteFileW(pending.c_str());throw std::runtime_error("Snapshot write/flush failed; guest outbox not acknowledged");}
    if(!MoveFileExW(pending.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)){
        // Keep the completed pending file for diagnosis/recovery, never fall
        // back to an empty store or acknowledge a failed commit.
        throw std::runtime_error("Snapshot commit failed; pending preserved, guest outbox not acknowledged");
    }
}
inline void loadProfileRestartSnapshot(const std::filesystem::path &path,const std::string &token,
    const xband::MailCaptureStore &custody,xband::MailProfileNames &names,xband::MailProfileOffers &offers){
    if(path.empty()||!std::filesystem::exists(path))throw std::runtime_error("Profile restart file missing");
    const auto size=std::filesystem::file_size(path);
    if(size<21||size>128*1024)throw std::runtime_error("Profile restart file size");
    std::ifstream file(path,std::ios::binary);if(!file)throw std::runtime_error("Profile restart read failed");
    std::vector<uint8_t> bytes(size);file.read(reinterpret_cast<char*>(bytes.data()),size);
    if(file.gcount()!=size||file.peek()!=std::char_traits<char>::eof())throw std::runtime_error("Profile restart file changed during read");
    xband::decodeMailProfileRestartSnapshot(bytes,token,custody,names,offers);
}
inline void commitProfileRestartSnapshot(const std::filesystem::path &path,const std::string &token,
    const xband::MailCaptureStore &custody,const xband::MailProfileNames &names,const xband::MailProfileOffers &offers){
    const auto bytes=xband::encodeMailProfileRestartSnapshot(token,custody,names,offers);
    auto pending=path;pending+=L".pending";
    const HANDLE file=CreateFileW(pending.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE)throw std::runtime_error("Profile restart pending file exists or cannot be created");
    DWORD written=0;const bool ready=WriteFile(file,bytes.data(),DWORD(bytes.size()),&written,nullptr)&&written==bytes.size()&&FlushFileBuffers(file);
    CloseHandle(file);
    if(!ready){DeleteFileW(pending.c_str());throw std::runtime_error("Profile restart write/flush failed");}
    if(!MoveFileExW(pending.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("Profile restart commit failed; pending preserved");
}
}
