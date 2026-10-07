#pragma once
#include "diagnostic_incoming_record.hpp"
#include "diagnostic_mail_date.hpp"
#include <windows.h>
#include <string>
namespace diagnostic {
// Saturn native uncompressed text object: flags:u16, size:u16,
// checksum:u16, NUL-terminated EUC-JP, one allocator padding byte.
// Centre text is bounded separately from captured player-mail bodies.
inline constexpr size_t broadcastBodyMaxBytes=240;
inline constexpr wchar_t broadcastSampleSubject[]=L"サーバーの不調について";
inline constexpr wchar_t broadcastSampleText[]=L"11／26(木)の午後から夜間にかけてサーバー不調のため接続が不安定になっていました。修復は完了しましたが、ご迷惑をおかけしたことをお詫び申し上げます。";
inline RecordBytes broadcastEUC(const std::wstring& text,size_t maxBytes){
    if(text.empty()||text.find(L'\0')!=std::wstring::npos)throw std::invalid_argument("Empty text or embedded NUL");
    std::wstring normalized;
    for(size_t i=0;i<text.size();++i){auto c=text[i];if(c==L'\r'){if(i+1<text.size()&&text[i+1]==L'\n')++i;c=L'\n';}
        if(c<L' '&&c!=L'\n')throw std::invalid_argument("Unsupported control character");normalized+=c;}
    // Same standard-JIS conversion as ranking settings, with LF accepted.
    // Windows EUC-JP encode is not available on all hosts; CP932 encode is.
    BOOL substituted=FALSE;
    auto n=WideCharToMultiByte(932,WC_NO_BEST_FIT_CHARS,normalized.data(),int(normalized.size()),nullptr,0,nullptr,&substituted);
    if(n<=0||substituted)throw std::invalid_argument("Text is not standard JIS (emoji cannot be sent)");
    std::string sjis(n,'\0');
    if(!WideCharToMultiByte(932,WC_NO_BEST_FIT_CHARS,normalized.data(),int(normalized.size()),sjis.data(),n,nullptr,&substituted)||substituted)
        throw std::invalid_argument("Unsupported character");
    RecordBytes bytes;
    for(size_t i=0;i<sjis.size();++i){unsigned a=uint8_t(sjis[i]);
        if(a<0x80){if((a<0x20&&a!=10)||a==0x7f)throw std::invalid_argument("Unsupported control character");bytes.push_back(uint8_t(a));}
        else if(a>=0xa1&&a<=0xdf){bytes.push_back(0x8e);bytes.push_back(uint8_t(a));}
        else{if(++i>=sjis.size())throw std::invalid_argument("Incomplete Japanese character");unsigned b=uint8_t(sjis[i]);
            unsigned row=(a<=0x9f?a-0x81:a-0xc1)*2+0x21,col;
            if(b>=0x9f){++row;col=b-0x7e;}else col=b-(b>0x7f?0x20:0x1f);
            if(row<0x21||row>0x7e||col<0x21||col>0x7e)throw std::invalid_argument("Nonstandard JIS character");
            bytes.push_back(uint8_t(row|0x80));bytes.push_back(uint8_t(col|0x80));}}
    if(bytes.size()>maxBytes)throw std::invalid_argument("EUC-JP byte limit exceeded");
    return bytes;
}
inline RecordBytes broadcastBody(const RecordBytes& text){
    if(text.empty()||text.size()>broadcastBodyMaxBytes||std::find(text.begin(),text.end(),0)!=text.end())throw std::invalid_argument("Invalid broadcast body");
    const auto n=text.size()+1;RecordBytes out{0,0,uint8_t(n>>8),uint8_t(n),0,0};
    out.insert(out.end(),text.begin(),text.end());out.insert(out.end(),{0,0});return out;
}
inline IncomingRecord broadcastRecord(const RecordBytes& subject,const RecordBytes& body,uint32_t date,uint16_t icon){
    if(subject.empty()||subject.size()>32||std::find(subject.begin(),subject.end(),0)!=subject.end())throw std::invalid_argument("Invalid broadcast subject");
    IncomingRecord r{};r.opaqueHeader.fill(0xff);r.opaqueA=0xff;r.opaqueC=icon;
    // Native receive identification: field1 -> record+10 (town),
    // field2 -> record+32 (codename). Do not use outbound recipient order.
    r.opaqueE=date;r.field2={'X','B','A','N','D',0};
    r.field1={0xc5,0xec,0xb5,0xfe,0}; // 東京 (EUC-JP), as in the supplied real-machine photo.
    r.field3=subject;r.field3.push_back(0);r.field4=broadcastBody(body);
    r.allocationBasis=uint16_t(0x80+r.field4.size()-4);r.opaqueD=uint16_t(0x80+r.field4.size());return r;
}
}
