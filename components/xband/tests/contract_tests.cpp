#include <xband/frame_codec.hpp>
#include <xband/service_endpoint.hpp>
#include <algorithm>
#include <deque>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
using Bytes=std::vector<uint8_t>;
using Decoder=xband::FrameDecoder;
void check(bool v,const char *msg){if(!v)throw std::runtime_error(msg);}
Bytes wire(const Bytes &p){Bytes b(p.size()+4);check(xband::encodeFrame(p,b).status==xband::EncodeStatus::ok,"fixture encode");return b;}
bool equal(std::span<const uint8_t> a,std::span<const uint8_t> b){return std::equal(a.begin(),a.end(),b.begin(),b.end());}
class Echo final:public xband::ServiceEndpoint {
public:
    std::deque<uint8_t> queue;unsigned clock=0;
    bool transmit(uint8_t b,unsigned)override {if(queue.size()==2)return false;queue.push_back(b);return true;}
    void tick(unsigned f)override {clock=f;}
    bool peek(uint8_t &b)const override {if(queue.empty())return false;b=queue.front();return true;}
    void consume()override {if(queue.empty())throw std::logic_error("empty");queue.pop_front();}
    size_t pending()const override {return queue.size();}
    void reset()override {queue.clear();}
};
void run(const std::string &name){
    if(name=="framing"){
        Decoder d;Bytes payload{0x7b,0x22,0x76,0x22,0x3a,0x32,0x7d};auto input=wire(payload),twice=input;
        twice.insert(twice.end(),input.begin(),input.end());auto r=d.feed(twice);
        check(r.status==Decoder::Status::ready&&r.consumed==input.size(),"first frame only");
        check(equal(d.peek(),payload)&&equal(d.peek(),payload),"passive peek");
        check(d.feed(input).consumed==0,"blocked until consumed");
        check(d.consume()&&!d.consume(),"consume once");
        check(d.feed(std::span(twice).subspan(r.consumed)).consumed==input.size(),"coalesced tail");
    }else if(name=="splits"){
        Bytes payload{0,1,3,0x7d,0x7e,0xff};auto input=wire(payload);
        for(size_t cut=0;cut<=input.size();++cut){
            Decoder d;check(d.feed(std::span(input).first(cut)).consumed==cut,"split prefix");
            check(d.feed(std::span(input).subspan(cut)).status==Decoder::Status::ready&&equal(d.peek(),payload),"all split positions");
        }
        Decoder d;for(auto b:input)check(d.feed(std::span(&b,1)).consumed==1,"byte feed");
        check(equal(d.peek(),payload),"binary unchanged");
    }else if(name=="limits"){
        for(auto bad:{Bytes{0,0,0,0},Bytes{0,1,0,1},Bytes{0xff,0xff,0xff,0xff}}){
            Decoder d;auto r=d.feed(bad);check(r.status==Decoder::Status::invalid_length&&r.consumed==4,"bad length");
            check(d.peek().empty()&&!d.consume()&&d.feed(wire({1})).consumed==0,"sticky fault");
        }
        Decoder d;Bytes payload(65536,0xa5);check(d.feed(wire(payload)).status==Decoder::Status::ready&&equal(d.peek(),payload),"maximum payload");
    }else if(name=="reset"){
        auto input=wire({1,2,3});
        for(size_t cut=0;cut<input.size();++cut){
            Decoder d;d.feed(std::span(input).first(cut));d.reset();
            check(d.feed(wire({9})).status==Decoder::Status::ready&&equal(d.peek(),Bytes{9}),"old fragments isolated");
        }
        Decoder d;d.feed(Bytes{0,0,0,0});d.reset();check(d.feed(wire({7})).status==Decoder::Status::ready,"reset fault");
    }else if(name=="encode"){
        Bytes out(7,0xaa),old=out;
        check(xband::encodeFrame(Bytes{1,2,3,4},out).status==xband::EncodeStatus::insufficient_space&&out==old,"short output unchanged");
        check(xband::encodeFrame({},out).status==xband::EncodeStatus::invalid_length&&out==old,"empty encode");
        check(xband::encodeFrame(Bytes(65537),out).status==xband::EncodeStatus::invalid_length,"oversize encode");
        check(xband::encodeFrame(Bytes{65,66,67},out).written==7&&out==Bytes({0,0,0,3,65,66,67}),"literal wire bytes");
    }else if(name=="service_backpressure"){
        Echo e;Bytes input{1,2,3},out;size_t cursor=0;
        auto sink=[&](uint8_t b){out.push_back(b);return true;};
        check(!xband::pumpService(e,input,cursor,42,sink)&&cursor==2,"input retained");
        uint8_t b;while(e.peek(b)){out.push_back(b);e.consume();}
        check(xband::pumpService(e,input,cursor,42,sink)&&out==input&&e.clock==42,"no duplicate on retry");
        e.transmit(4,42);size_t empty=0;
        check(!xband::pumpService(e,{},empty,43,[](uint8_t){return false;})&&e.pending()==1,"output retained");
        check(xband::pumpService(e,{},empty,43,sink)&&out==Bytes({1,2,3,4}),"resume output");
    }else if(name=="service_failure"){
        Echo e;size_t cursor=1;bool threw=false;
        try{xband::pumpService(e,{},cursor,0,[](uint8_t){return true;});}catch(const std::invalid_argument&){threw=true;}
        check(threw,"cursor rejected");e.transmit(8,0);cursor=0;threw=false;
        try{xband::pumpService(e,{},cursor,0,[](uint8_t)->bool{throw std::runtime_error("sink");});}catch(const std::runtime_error&){threw=true;}
        check(threw&&e.pending()==1,"failure retains output");e.reset();check(e.pending()==0,"reset clears");
    }else if(name=="independent"){
        Decoder a,b;Echo first,second;a.feed(wire({1}));first.transmit(2,0);
        check(b.peek().empty()&&second.pending()==0,"independent instances");
    }else throw std::runtime_error("unknown test");
}
int main(int argc,char **argv){
    try{check(argc==2,"test required");run(argv[1]);std::cout<<"PASS "<<argv[1]<<'\n';}
    catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}
}
