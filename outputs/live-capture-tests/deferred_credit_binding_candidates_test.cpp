#include "deferred_credit_binding_candidates.hpp"
#include <fstream>
#include <filesystem>
#include <iostream>
int main(int argc,char** argv){try{
    using J=nlohmann::json;using B=LocalTCPProbe::Bytes;
    auto check=[](bool ok){if(!ok)throw std::runtime_error("Binding candidates assertion");};
    check(argc==3||argc==4);std::ifstream m(argv[1]),r(argv[2]);const auto match=J::parse(m),report=J::parse(r);
    const auto request=report.at("request").get<B>();const auto time=report.at("accepted_unix_ms").get<uint64_t>();
    unsigned cases=0;
    for(unsigned mode=0;mode<11;++mode){
        J history=J::array({match});B wire=request;unsigned side=0;
        if(mode==1)history=J::array();
        if(mode==2){auto extra=match;extra["id"]=999999;history.push_back(extra);}
        if(mode==3)history[0]["unix_ms"]=time+1;
        if(mode==4)history[0]["participants"][0]["phone"]="other";
        if(mode==5)history[0]["participants"][0]["profile"]=1;
        if(mode==6)history[0]["game"]=65540;
        if(mode==7)history[0]["participants"][1]["side"]=0;
        if(mode==8)history.push_back(match);
        if(mode==9)wire.pop_back();
        if(mode==10)side=2;
        const auto observed=diagnostic::deferredCreditBindingCandidates(wire,side,time,history);
        check(observed.at("verified")==false&&observed.at("debit_enabled")==false);
        check(observed.at("candidates").size()==(mode==0?1:mode==2?2:0));
        if(mode==0)check(observed.at("status")=="single-candidate-needs-review");
        if(mode==2)check(observed.at("status")=="ambiguous-needs-review");
        if(mode==8)check(observed.at("status")=="duplicate-history-id");
        ++cases;
    }
    if(argc==4){
        J history=J::array();
        for(const auto& file:std::filesystem::directory_iterator(argv[3]))if(file.path().extension()==".json"){
            std::ifstream input(file.path());history.push_back(J::parse(input));
        }
        const auto observed=diagnostic::deferredCreditBindingCandidates(request,report.at("side").get<unsigned>(),time,history);
        std::cout<<"ACTUAL_HISTORY_REVIEW status="<<observed.at("status")<<" candidates="<<observed.at("candidates").size()<<" verified=false debit_enabled=false\n";
    }
    std::cout<<"PASS "<<cases<<" binding candidate cases; single or ambiguous never verified, no debit\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
