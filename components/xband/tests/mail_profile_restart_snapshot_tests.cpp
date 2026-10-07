#include <xband/mail_profile_restart_snapshot.hpp>
#include <fstream>
#include <iostream>

int main(int argc,char **argv){try{
    auto check=[](bool ok){if(!ok)throw std::runtime_error("Profile restart snapshot assertion failed");};
    if(argc==4&&std::string_view(argv[1])=="--verify-original-long-held"){
        auto read=[](const char *path,size_t limit){
            std::ifstream file(path,std::ios::binary|std::ios::ate);
            if(!file)throw std::runtime_error("Cannot open original-ROM snapshot");
            const auto size=file.tellg();
            if(size<0||size>static_cast<std::streamoff>(limit))throw std::runtime_error("Original-ROM snapshot size");
            std::vector<uint8_t> bytes(static_cast<size_t>(size));
            file.seekg(0);file.read(reinterpret_cast<char*>(bytes.data()),size);
            if(file.gcount()!=size)throw std::runtime_error("Original-ROM snapshot short read");
            return bytes;
        };
        const auto custodyBytes=read(argv[2],256*1024);
        const auto stateBytes=read(argv[3],128*1024);
        const auto custody=xband::decodeMailCaptureSnapshot(custodyBytes);
        const auto &records=custody.captures();
        check(records.size()==2&&records[0].localId==1&&records[1].localId==2);
        std::vector<uint8_t> expectedBody{0,0,0,115,0,0,'9','8'};
        expectedBody.insert(expectedBody.end(),112,'7');
        expectedBody.insert(expectedBody.end(),{0,0xad});
        check(expectedBody.size()==122&&records[1].fields[2]==expectedBody);
        check(stateBytes.size()>=10&&stateBytes[8]>0&&size_t(stateBytes[8])+9<stateBytes.size());
        const std::string token(stateBytes.begin()+9,stateBytes.begin()+9+stateBytes[8]);
        xband::MailProfileNames names;
        xband::MailProfileOffers offers;
        xband::decodeMailProfileRestartSnapshot(stateBytes,token,custody,names,offers);
        const auto entries=offers.snapshotEntries();
        check(names.size()==2&&entries.size()==2);
        for(size_t i=0;i<entries.size();++i)
            check(entries[i].id==i+1&&entries[i].observedHeld&&entries[i].target.endpoint=="pb3-1"&&entries[i].target.player==0);
        check(xband::encodeMailProfileRestartSnapshot(token,custody,names,offers)==stateBytes);
        bool rejected=false;
        try{xband::MailProfileNames n;xband::MailProfileOffers o;
            xband::decodeMailProfileRestartSnapshot(stateBytes,token+"x",custody,n,o);
        }catch(const std::invalid_argument&){rejected=true;}
        check(rejected);
        auto altered=custody;auto extra=records[0];check(altered.retain(extra)==3);
        rejected=false;
        try{xband::MailProfileNames n;xband::MailProfileOffers o;
            xband::decodeMailProfileRestartSnapshot(stateBytes,token,altered,n,o);
        }catch(const std::invalid_argument&){rejected=true;}
        check(rejected);
        std::cout<<"PASS original long-mail profile snapshot: exact 122-byte body, IDs1/2, names2/offers2/observed-held2, exact re-encode, wrong token or custody rejected\n";
        return 0;
    }
    if(argc!=1)throw std::invalid_argument("Expected no arguments or --verify-original-long-held custody state");
    xband::MailCaptureStore custody;
    xband::MailCapture mail{};
    mail.sourceEndpoint="pb3-0";mail.sourcePhone="123";mail.sourceCodename={'S'};
    mail.fields={std::vector<uint8_t>{'R',0},std::vector<uint8_t>{'T',0},std::vector<uint8_t>{0,7}};
    check(custody.retain(mail)==1);
    xband::MailProfileNames names;
    names.observe("pb3-1","fresh-COW-server-run",0,std::vector<uint8_t>{'R'});
    xband::MailProfileOffers offers;
    const xband::MailProfileNames::Target target{"pb3-1","fresh-COW-server-run",0};
    const std::vector<uint64_t> id{1};
    check(offers.offerBatch(target,id));
    check(offers.rememberedHeldFor(target,id)==id);
    const auto saved=xband::encodeMailProfileRestartSnapshot("trial-token",custody,names,offers);
    xband::MailProfileNames restoredNames;
    xband::MailProfileOffers restoredOffers;
    xband::decodeMailProfileRestartSnapshot(saved,"trial-token",custody,restoredNames,restoredOffers);
    check(restoredNames.resolve("pb3-1","fresh-COW-server-run",0)==std::optional<xband::MailProfileNames::Name>({'R'}));
    check(restoredOffers.rememberedHeldFor(target,std::vector<uint64_t>{})==id);
    check(restoredOffers.heldFor({"pb3-0","fresh-COW-server-run",0},id).empty());
    auto rejected=[&](const auto &bytes,const std::string &token,const auto &store){
        xband::MailProfileNames n; xband::MailProfileOffers o;
        try{xband::decodeMailProfileRestartSnapshot(bytes,token,store,n,o);}catch(const std::invalid_argument&){return n.size()==0&&o.size()==0;}
        return false;
    };
    check(rejected(saved,"other-token",custody));
    auto damaged=saved;damaged[12]^=1;check(rejected(damaged,"trial-token",custody));
    damaged=saved;damaged.pop_back();check(rejected(damaged,"trial-token",custody));
    xband::MailCaptureStore other;check(rejected(saved,"trial-token",other));
    std::cout<<"PASS profile restart snapshot: names/offers/held restored, wrong token/custody/corruption fail closed\n";
    return 0;
}catch(const std::exception &e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
