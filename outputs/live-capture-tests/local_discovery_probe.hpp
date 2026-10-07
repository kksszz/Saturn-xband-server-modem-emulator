#pragma once
#include <vector>
#include <cstdint>
#include <algorithm>
// Synthetic, local-only endpoint-list experiment. NOT an authenticated XBAND service.
struct LocalDiscoveryProbe {
    using Bytes=std::vector<uint8_t>;
    static uint16_t checksum(const Bytes &b) {
        uint32_t sum=0;
        for(size_t i=0;i<b.size();i+=2) sum+=(uint32_t(b[i])<<8)|(i+1<b.size()?b[i+1]:0);
        while(sum>>16)sum=(sum&65535)+(sum>>16);
        return uint16_t(~sum);
    }
    static unsigned word(const Bytes &b,size_t i){return (unsigned(b[i])<<8)|b[i+1];}
    static void put(Bytes &b,size_t i,unsigned v){b[i]=uint8_t(v>>8);b[i+1]=uint8_t(v);}
    static Bytes pseudo(const Bytes &ip) {
        Bytes p(ip.begin()+12,ip.begin()+20);p.push_back(0);p.push_back(17);
        p.push_back(ip[24]);p.push_back(ip[25]);p.insert(p.end(),ip.begin()+20,ip.end());return p;
    }
    static Bytes reply(const Bytes &ip) {
        // Narrowly accept only the observed unfragmented IPv4/UDP request shape.
        if(ip.size()!=33||ip[0]!=0x45||word(ip,2)!=33||word(ip,6)!=0||ip[9]!=17)return {};
        if(checksum(Bytes(ip.begin(),ip.begin()+20))!=0||word(ip,24)!=13)return {};
        if(word(ip,26)!=0 && checksum(pseudo(ip))!=0)return {};
        if(!std::equal(ip.begin()+12,ip.begin()+16,Bytes{10,0,0,2}.begin()))return {};
        if(ip[16]!=10||ip[17]!=0||ip[18]!=0||ip[19]<101||ip[19]>108)return {};
        if(word(ip,20)==0||word(ip,22)!=2004)return {};
        if(Bytes(ip.begin()+28,ip.end())!=Bytes{0x1f,0x74,0x6a,0x30,0x34})return {};
        // Leading byte is ignored by the observed caller; zero is an experimental choice.
        // Body: count=1, reserved=0, port=2005, literal virtual address=10.0.0.1.
        Bytes out{0x45,0,0,37,0,1,0,0,64,17,0,0};
        out.insert(out.end(),ip.begin()+16,ip.begin()+20);
        out.insert(out.end(),ip.begin()+12,ip.begin()+16);
        out.insert(out.end(),{7,0xd4,4,0,0,17,0,0,0,1,0,7,0xd5,10,0,0,1});
        put(out,22,word(ip,20)); // Reply to the guest's actual socket, including mail's 1025.
        auto udp=checksum(pseudo(out));put(out,26,udp?udp:65535);
        put(out,10,checksum(Bytes(out.begin(),out.begin()+20)));
        return out;
    }
};
