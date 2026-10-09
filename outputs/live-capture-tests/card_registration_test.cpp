#include "local_tcp_probe.hpp"
#include <iostream>
int main(){try{
    auto check=[](bool yes,const char* message){if(!yes)throw std::runtime_error(message);};
        {
            using T=LocalTCPProbe;using B=T::Bytes;
            unsigned cases=0;
            for(unsigned cardLength:{0u,13u})for(uint8_t selector:{uint8_t(2),uint8_t(3),uint8_t(4)}){
                // Synthetic extension of the established resource/result framing.
                // The raw card is deliberately opaque and contains misleading tags.
                B request(184+cardLength,0);
                const B prefix{0x1f,0x74,0x6a,0x30,0x34,0x0b};
                std::copy(prefix.begin(),prefix.end(),request.begin());
                request[22]=12;
                const std::string phone="33366666665";
                std::copy(phone.begin(),phone.end(),request.begin()+23);
                request[135]=0x1e;request[138]=100;T::putLong(request,139,cardLength);
                if(cardLength){const B raw{0x0e,2,0x1d,0x20,0x15,0xff,0,0,0,0,1,15,15};
                    std::copy(raw.begin(),raw.end(),request.begin()+143);}
                request[143+cardLength]=0x0c;T::putLong(request,144+cardLength,0x10003);
                request[159+cardLength]=0x0e;request[160+cardLength]=selector==2?3:selector;
                request[161+cardLength]=0x1c;request[162+cardLength]=16;
                request[175+cardLength]=2;request[176+cardLength]=1;
                request.insert(request.end(),{0x15,0,2,0,1,0,0,1,0x14});
                request.insert(request.end(),276,0);
                request.insert(request.end(),{2,1,0,0,1,0x44});request.insert(request.end(),324,0);
                request.insert(request.end(),{0x16,0,0,0x1d,0,0,0x20,0,0,0,84});
                B result(84,0);T::putLong(result,0,0x10003);
                request.insert(request.end(),result.begin(),result.end());request.insert(request.end(),4,0);
                request.insert(request.end(),{0x24,0x26,0x26,0x1b,0,0,0,1,0x29,0,0,0,0});
                if(selector==2){const B name{3,'2','2',0};
                    request[160+cardLength]=2;request.insert(request.begin()+161+cardLength,name.begin(),name.end());}
                check(T::completeMailProbeRequest(request),"Opaque card postmatch framing rejected");
                check(T::serviceRequestCode(request)==selector,"Card shifted selector misread");
                check(diagnostic::receivedGameID(request)==0x10003,"Card shifted game ID misread");
                B expectedResult{0,0,0,84};expectedResult.insert(expectedResult.end(),result.begin(),result.begin()+80);
                check(T::observedGameResult(request)==expectedResult,"Card shifted result changed");
                if(selector!=4){check(T::standbyWaitPreference(request)==2,"Card shifted wait preference");
                    check(T::standbyAreaPreference(request)==1,"Card shifted area preference");}
                for(size_t cut=0;cut<request.size();++cut)
                    check(!T::completeMailProbeRequest(B(request.begin(),request.begin()+cut)),"Partial card postmatch accepted");
                for(uint32_t length:{1u,12u,14u,0xffffffffu}){
                    auto bad=request;T::putLong(bad,139,length);
                    check(!T::completeMailProbeRequest(bad),"Unsupported card length accepted");}
                auto bad=request;bad[136]=1;
                check(!T::completeMailProbeRequest(bad),"Unsupported card tag accepted");
                bad=request;bad.push_back(0);
                check(!T::completeMailProbeRequest(bad),"Card request trailing byte accepted");
                ++cases;
            }
            std::cout<<"PASS card postmatch framing: "<<cases<<" synthetic mail/match/named cases, opaque13/absent, selector/game/result/preferences, all truncations and malformed lengths; no charging\n";
        }
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
