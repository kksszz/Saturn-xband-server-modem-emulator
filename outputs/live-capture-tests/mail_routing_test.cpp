#include "local_mail_journal.hpp"
#include <source_location>
#include <iostream>
namespace {
using B=LocalTCPProbe::Bytes;using J=nlohmann::json;using namespace diagnostic;
void check(bool ok,std::source_location at=std::source_location::current()){
    if(!ok)throw std::runtime_error("Mail routing assertion line "+std::to_string(at.line()));
}
B request(const std::string& phone,const std::string& name,unsigned slot,const std::string& to={}){
    B b(197,0);const B prefix{0x1f,0x74,0x6a,0x30,0x34,0x0b};std::copy(prefix.begin(),prefix.end(),b.begin());
    b[22]=12;b[81]='T';b[135]=0x1e;LocalTCPProbe::putLong(b,139,13);
    b[156]=0x0c;LocalTCPProbe::putLong(b,157,0x10003);b[172]=0x0e;b[173]=4;b[174]=0x1c;b[175]=16;b[188]=2;b[189]=1;
    b.insert(b.end(),{0x15,0,2,0,1,0,0,1,0x14});b.insert(b.end(),276,0);
    b.insert(b.end(),{2,1,0,0,1,0x44});b.insert(b.end(),324,0);
    b.insert(b.end(),{0x16,0,0,0x1d,0,uint8_t(!to.empty())});
    if(!to.empty()){
        b.push_back(uint8_t(slot));b.insert(b.end(),9,0xff);
        for(const auto& field:{to,std::string("new mail"),std::string("body")}){
            b.push_back(0);b.push_back(uint8_t(field.size()+1));b.insert(b.end(),field.begin(),field.end());b.push_back(0);}
        b.insert(b.end(),4,0);
    }
    b.insert(b.end(),{0x20,0,0,0,84});b.insert(b.end(),84,0);b.insert(b.end(),4,0);
    b.insert(b.end(),{0x24,0x26,0x26,0x1b,0,0,0,1,0x29,0,0,0,0});
    b.erase(b.begin()+23,b.begin()+35);B number(phone.begin(),phone.end());number.push_back(0);
    b.insert(b.begin()+23,number.begin(),number.end());b[22]=uint8_t(number.size());
    b[xband::registrationOffset(b,43)]=uint8_t(slot);
    const auto start=xband::registrationOffset(b,81);std::fill_n(b.begin()+start,16,0);std::copy(name.begin(),name.end(),b.begin()+start);
    check(LocalTCPProbe::completeMailProbeRequest(b));return b;
}
std::string nameOf(const B& wire){const auto at=xband::registrationOffset(wire,81);return std::string(wire.begin()+at,std::find(wire.begin()+at,wire.begin()+at+16,0));}
B addressAt(const B& wire,size_t index){
    check(!wire.empty()&&wire[0]==0x1e);size_t p=3;
    for(size_t i=0;;++i){
        p+=14;check(p<wire.size());const auto townLength=wire.at(p++);check(p+townLength<=wire.size());
        B town(wire.begin()+p,wire.begin()+p+townLength);if(i==index)return town;p+=townLength;
        const auto senderLength=wire.at(p++);p+=senderLength+6;
        const auto titleLength=wire.at(p++);p+=titleLength;
        const auto bodyLength=LocalTCPProbe::D::word(wire,p);p+=2+bodyLength+4;
    }
}
void checkMail(const LocalMailJournal::Inbox& inbox,const std::string& key,const B& address){
    const auto found=std::find(inbox.keys.begin(),inbox.keys.end(),key);check(found!=inbox.keys.end());
    check(addressAt(inbox.wire,size_t(found-inbox.keys.begin()))==address);
}
}
int main(int argc,char** argv){try{
    check(argc==2||argc==3);activeRegions=std::make_shared<JapanAreaCodeTable>(argv[1]);
    auto tokyo=mailSenderAddress(*activeRegions,"03-555");check(!tokyo.empty());tokyo.push_back(0);
    if(argc==3){
        // Read-only audit of the existing user trial: never append, mark,
        // change offers, consume cards, or replay service requests.
        const auto path=std::filesystem::absolute(argv[2]);check(std::filesystem::is_directory(path));
        LocalMailJournal live(path);std::map<std::string,B> latest;
        std::vector<std::filesystem::path> files;for(const auto& f:std::filesystem::directory_iterator(path))if(f.path().extension()==L".json")files.push_back(f.path());
        std::sort(files.begin(),files.end());for(const auto& f:files){std::ifstream in(f);J row;in>>row;const auto wire=row.at("request").get<B>();latest[nameOf(wire)]=wire;}
        for(const auto& [name,key]:std::array<std::pair<std::string,std::string>,2>{{{"21","224:0"},{"22","224:1"}}}){
            const auto inbox=live.prepareInbox(latest.at(name));checkMail(inbox,key,tokyo);
            std::cout<<"PASS existing queued mail "<<key<<" -> "<<name<<"; address Tokyo; audit only, no mutation\n";
        }
        return 0;
    }
    const auto root=std::filesystem::temp_directory_path()/("xband-mail-routing-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64()));
    {
        LocalMailJournal journal(root/"phone-change");
        const auto old22=request("03-666","22",0),old21=request("03-666","21",2);
        const auto new22=request("059-666","22",0),new21=request("059-666","21",2);
        journal.append(old22,1);journal.append(old21,1);
        const auto oldBound=journal.append(request("03-555","11",0,"22"),0);
        journal.append(new21,1); // Old22 is historical, not a valid current target.
        const auto pending22=journal.append(request("03-555","11",0,"22"),0);
        journal.append(new22,1);
        auto inbox=journal.prepareInbox(new22);checkMail(inbox,std::to_string(pending22)+":0",tokyo);
        check(std::find(inbox.keys.begin(),inbox.keys.end(),std::to_string(oldBound)+":0")==inbox.keys.end());
        check(journal.prepareInbox(new21).wire.empty());
        const auto pending21=journal.append(request("03-555","11",0,"21"),0);
        checkMail(journal.prepareInbox(new21),std::to_string(pending21)+":0",tokyo);
        LocalMailJournal reopened(root/"phone-change");checkMail(reopened.prepareInbox(new22),std::to_string(pending22)+":0",tokyo);
        checkMail(reopened.prepareInbox(new21),std::to_string(pending21)+":0",tokyo);
        journal.markPrepared(inbox.keys);LocalMailJournal offered(root/"phone-change");check(offered.prepareInbox(new22).wire.empty());
    }
    {
        LocalMailJournal journal(root/"real-duplicates");const auto receiver=request("059-666","22",0);
        journal.append(receiver,1);journal.append(request("03-555","22",0),0);
        const auto pending=journal.append(request("03-555","11",1,"22"),0);
        check(journal.prepareInbox(receiver).wire.empty()); // Two current owners: refuse guessing.
        journal.append(request("03-555","12",0),0);
        checkMail(journal.prepareInbox(receiver),std::to_string(pending)+":0",tokyo);
    }
    std::cout<<"PASS current-terminal routing: phone change, slots21/22, pending mail recovery, no frozen-recipient reassignment, Tokyo address, restart, offer suppression and true duplicate refusal\n";
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
