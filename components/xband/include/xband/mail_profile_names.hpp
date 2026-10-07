#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace xband {
// Observed LOCAL profile names, not authenticated accounts. The owner must
// supply a new epoch on profile replacement/load-state/reset of save identity.
// Never infer an epoch from phone numbers, names, or a network reconnect.
class MailProfileNames {
public:
    using Name=std::vector<uint8_t>;
    enum class Observation { added, repeated, conflict, capacity_refused };
    struct Target {
        std::string endpoint,epoch;
        uint8_t player;
        bool operator==(const Target&)const=default;
    };
private:
    using Key=std::tuple<std::string,std::string,uint8_t>;
    struct Entry { Name name; bool conflicted=false; };
    std::map<Key,Entry> entries_;
    size_t limit_;
    static void validate(const std::string &endpoint,const std::string &epoch,uint8_t player){
        if(endpoint.empty()||endpoint.size()>64||endpoint.find('\0')!=std::string::npos||
           epoch.empty()||epoch.size()>64||epoch.find('\0')!=std::string::npos||player>3)
            throw std::invalid_argument("Invalid local mail profile key");
    }
public:
    explicit MailProfileNames(size_t limit=128):limit_(limit){}
    struct SnapshotEntry { Target target; Name name; bool conflicted=false; };
    std::vector<SnapshotEntry> snapshotEntries()const{
        std::vector<SnapshotEntry> result;
        for(const auto &[key,entry]:entries_)
            result.push_back({{std::get<0>(key),std::get<1>(key),std::get<2>(key)},entry.name,entry.conflicted});
        return result;
    }
    void restoreEntries(std::span<const SnapshotEntry> saved){
        if(saved.size()>limit_)throw std::invalid_argument("Profile snapshot capacity");
        MailProfileNames staged(limit_);
        for(const auto &entry:saved){
            validate(entry.target.endpoint,entry.target.epoch,entry.target.player);
            if(entry.conflicted)throw std::invalid_argument("Conflicted profile cannot be restored");
            if(staged.observe(entry.target.endpoint,entry.target.epoch,entry.target.player,entry.name)!=Observation::added)
                throw std::invalid_argument("Duplicate profile snapshot entry");
        }
        entries_.swap(staged.entries_);
    }
    Observation observe(const std::string &endpoint,const std::string &epoch,uint8_t player,
                        std::span<const uint8_t> name){
        validate(endpoint,epoch,player);
        if(name.empty()||name.size()>32||std::find(name.begin(),name.end(),0)!=name.end())
            throw std::invalid_argument("Invalid observed profile name");
        const Key key{endpoint,epoch,player};
        const auto found=entries_.find(key);
        if(found!=entries_.end()){
            auto &entry=found->second;
            if(entry.conflicted)return Observation::conflict;
            if(entry.name.size()==name.size()&&std::equal(name.begin(),name.end(),entry.name.begin()))
                return Observation::repeated;
            // Rename versus replaced identity is not established. Do not
            // silently overwrite or recover a conflicted identity on retry.
            entry.name.clear();entry.conflicted=true;return Observation::conflict;
        }
        if(entries_.size()>=limit_)return Observation::capacity_refused;
        entries_.emplace(key,Entry{Name(name.begin(),name.end()),false});
        return Observation::added;
    }
    std::optional<Name> resolve(const std::string &endpoint,const std::string &epoch,uint8_t player)const{
        validate(endpoint,epoch,player);
        const auto found=entries_.find(Key{endpoint,epoch,player});
        if(found==entries_.end()||found->second.conflicted)return {};
        return found->second.name;
    }
    // All-or-none association in record order; no custody, ID, or deletion.
    std::optional<std::vector<Name>> resolveBatch(const std::string &endpoint,const std::string &epoch,
                                                 std::span<const uint8_t> players)const{
        validate(endpoint,epoch,0);
        if(players.size()>128)throw std::invalid_argument("Profile batch exceeds bound");
        for(auto player:players)validate(endpoint,epoch,player);
        std::vector<Name> names;
        for(auto player:players){
            auto name=resolve(endpoint,epoch,player);
            if(!name)return {};
            names.push_back(std::move(*name));
        }
        return names;
    }
    void invalidateEndpoint(const std::string &endpoint){
        validate(endpoint,"invalidate",0);
        for(auto i=entries_.begin();i!=entries_.end();){
            if(std::get<0>(i->first)==endpoint)i=entries_.erase(i);else ++i;
        }
    }
    // Unique within the observed epoch only; not global account discovery.
    // Any conflicted name in the epoch blocks routing, even if its previous
    // value has been discarded. Never infer a target from phone/other modem.
    std::optional<Target> uniqueTarget(const std::string &epoch,std::span<const uint8_t> name)const{
        validate("lookup",epoch,0);
        if(name.empty()||name.size()>32||std::find(name.begin(),name.end(),0)!=name.end())
            throw std::invalid_argument("Invalid recipient name");
        std::optional<Target> result;
        for(const auto &[key,entry]:entries_){
            if(std::get<1>(key)!=epoch)continue;
            if(entry.conflicted)return {};
            if(entry.name.size()!=name.size()||!std::equal(name.begin(),name.end(),entry.name.begin()))continue;
            if(result)return {};
            result=Target{std::get<0>(key),epoch,std::get<2>(key)};
        }
        return result;
    }
    size_t size()const noexcept{return entries_.size();}
};
}
