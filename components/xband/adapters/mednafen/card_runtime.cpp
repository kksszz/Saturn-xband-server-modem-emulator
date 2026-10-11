#define NOMINMAX
#include "card_runtime.h"
#include <xband/virtual_media_card.hpp>
#include <xband/media_card_control.hpp>
#include <xband/windows_image_storage.hpp>
#include <memory>
#include <cstdlib>
struct XbandCardRuntime {
    xband::VirtualMediaCard card;
    std::unique_ptr<xband::windows_storage::VirtualCardStorage> storage;
    std::filesystem::path selectionPath;
    bool dirty=false,failed=false;
    uint64_t frame=0;
    std::optional<uint64_t> reinsertFrame;
    std::string token,error,report;
    explicit XbandCardRuntime(const std::filesystem::path& path):selectionPath(path.wstring()+L".selection.json") {
        auto selected=path;
        if(std::filesystem::exists(selectionPath)){
            std::ifstream in(selectionPath);const auto j=nlohmann::json::parse(in);
            if(j.at("version")!=1)throw std::runtime_error("Invalid saved card selection");
            selected=std::filesystem::u8path(j.at("image_path").get<std::string>());
            if(!selected.is_absolute()||!std::filesystem::exists(selected))throw std::runtime_error("Selected card missing; preserve selection");
        }
        storage=std::make_unique<xband::windows_storage::VirtualCardStorage>(selected);
        if(storage->loaded())card.data=*storage->loaded();
        else {
            // Exactly the existing Ymir synthetic-100 fixture; not a real ID.
            card.data={1,2,3,4,5,6,7,0,0,0,1,15,15};
            // Distinct synthetic IDs for the two local endpoints. Existing
            // images/selections are never changed or refilled on startup.
            const char* side=std::getenv("XBAND_MEDNAFEN_SIDE");
            if(side&&std::string(side)=="1")card.data={2,2,3,4,5,6,7,3,0,0,1,15,15};
            storage->save(card.data);
        }
        if(!card.remainingUnits())throw std::runtime_error("Unreadable trial card; preserved");
        card.setInserted(true);
    }
    void saveSelection(const std::filesystem::path& selected){
        auto temp=selectionPath;temp+=L".pending";
        if(std::filesystem::exists(temp))throw std::runtime_error("Incomplete card selection preserved");
        const auto utf8=selected.u8string();
        const nlohmann::json j{{"version",1},{"image_path",std::string(utf8.begin(),utf8.end())}};
        {std::ofstream out(temp,std::ios::binary);out<<j.dump(2);out.flush();if(!out)throw std::runtime_error("Card selection write failed");}
        if(std::filesystem::exists(selectionPath)){
            auto backup=selectionPath;backup+=L".bak";
            if(!CopyFileW(selectionPath.c_str(),backup.c_str(),FALSE))throw std::runtime_error("Card selection backup failed");
        }
        if(!MoveFileExW(temp.c_str(),selectionPath.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))throw std::runtime_error("Card selection commit failed");
    }
};
extern "C" XbandCardRuntime* xband_card_create(const wchar_t* path){
    try{
        if(!path||!std::filesystem::path(path).is_absolute())return nullptr;
        return new XbandCardRuntime(std::filesystem::path(path));
    }catch(...){return nullptr;}
}
extern "C" int xband_card_flush(XbandCardRuntime* c){
    if(!c||c->failed)return 0;
    try{if(c->dirty){c->storage->save(c->card.data);c->dirty=false;}return 1;}
    catch(...){c->failed=true;c->card.setInserted(false);return 0;}
}
extern "C" void xband_card_destroy(XbandCardRuntime* c){if(c)xband_card_flush(c);delete c;}
extern "C" void xband_card_reset(XbandCardRuntime* c){if(c)c->card.resetPins();}
extern "C" uint8_t xband_card_read(XbandCardRuntime* c,uint8_t fallback){
    return c?c->card.read(fallback):uint8_t(fallback&~0x11u);
}
extern "C" void xband_card_write(XbandCardRuntime* c,uint8_t value){
    if(!c||c->failed)return;
    const auto before=c->card.data;c->card.write(value);
    c->dirty=c->dirty||c->card.data!=before;
}
extern "C" int xband_card_units(const XbandCardRuntime* c){
    if(!c||c->failed)return -1;
    const auto units=c->card.remainingUnits();return units?*units:-1;
}
extern "C" const char* xband_card_report(XbandCardRuntime* c){
    if(!c)return nullptr;
    try{
        const auto units=c->failed?std::nullopt:c->card.remainingUnits();
        using J=nlohmann::json;
        c->report=J{{"version",1},{"configured",true},{"inserted",c->card.present},
            {"save_failed",c->failed},{"read_fault",c->card.readFault},
            {"units",units?J(*units):J(nullptr)},{"command_token",c->token},
            {"replace_supported",true},{"can_replace",!c->failed&&!c->reinsertFrame},
            {"command_error",c->error}}.dump();
        return c->report.c_str();
    }catch(...){return nullptr;}
}
extern "C" int xband_card_command(XbandCardRuntime* c,const char* wire){
    if(!c||!wire)return 0;
    try{
        const auto command=nlohmann::json::parse(wire);
        const auto token=xband::MediaCardControl::validateCommand(command);
        if(token==c->token)return c->error.empty(); // Repeated delivery never swaps again.
        c->token=token;
        try{
            if(c->failed||!xband_card_flush(c))throw std::runtime_error("Card persistence unavailable");
            if(command.contains("image_path")){
                if(c->reinsertFrame)throw std::runtime_error("Card replacement pending");
                const auto path=std::filesystem::u8path(command.at("image_path").get<std::string>());
                if(!path.is_absolute()||!std::filesystem::exists(path))throw std::runtime_error("Existing absolute card path required");
                auto storage=std::make_unique<xband::windows_storage::VirtualCardStorage>(path);
                if(!storage->loaded())throw std::runtime_error("Existing 13-byte card required");
                xband::VirtualMediaCard candidate;candidate.data=*storage->loaded();
                if(!candidate.remainingUnits())throw std::runtime_error("Unreadable replacement card");
                c->saveSelection(path);
                const bool wasInserted=c->card.present;
                c->card.setInserted(false);c->card=candidate;c->storage=std::move(storage);
                if(wasInserted)c->reinsertFrame=c->frame+2;
            }else{
                c->reinsertFrame.reset();
                const bool inserted=command.at("inserted").get<bool>();
                if(c->card.present!=inserted)c->card.setInserted(inserted);
                if(command.contains("read_fault"))c->card.setReadFault(command.at("read_fault").get<bool>());
            }
            c->error.clear();return 1;
        }catch(const std::exception& e){c->error=std::string(e.what()).substr(0,256);return 0;}
    }catch(...){return 0;}
}
extern "C" void xband_card_frame(XbandCardRuntime* c){
    if(!c)return;
    ++c->frame;
    if(c->reinsertFrame&&c->frame>=*c->reinsertFrame){
        c->reinsertFrame.reset();if(!c->failed)c->card.setInserted(true);
    }
}
