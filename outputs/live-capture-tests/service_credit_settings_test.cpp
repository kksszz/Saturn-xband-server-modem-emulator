#include "service_credit_window.hpp"
#include <iostream>
#include <gdiplus.h>
#include <vector>
void render(HWND window,const std::filesystem::path& path){
    Gdiplus::GdiplusStartupInput input;ULONG_PTR token{};
    if(Gdiplus::GdiplusStartup(&token,&input,nullptr)!=Gdiplus::Ok)throw std::runtime_error("GDI startup");
    RECT r{};GetClientRect(window,&r);const int width=r.right,height=r.bottom;
    auto dc=CreateCompatibleDC(nullptr);BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth=width;info.bmiHeader.biHeight=-height;info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;
    void* pixels=nullptr;auto dib=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&pixels,nullptr,0);auto old=SelectObject(dc,dib);
    SetWindowPos(window,nullptr,-30000,-30000,0,0,SWP_NOSIZE|SWP_NOACTIVATE|SWP_NOZORDER);
    ShowWindow(window,SW_SHOWNOACTIVATE);UpdateWindow(window);
    if(!PrintWindow(window,dc,PW_CLIENTONLY))throw std::runtime_error("Credit editor print failed");
    GdiFlush();
    bool saved=false;
    {Gdiplus::Bitmap bitmap(width,height,width*4,PixelFormat32bppRGB,static_cast<BYTE*>(pixels));
     UINT count{},size{};Gdiplus::GetImageEncodersSize(&count,&size);std::vector<uint8_t> data(size);
     auto codecs=reinterpret_cast<Gdiplus::ImageCodecInfo*>(data.data());Gdiplus::GetImageEncoders(count,size,codecs);
     std::filesystem::create_directories(path.parent_path());
     for(UINT i=0;i<count;++i)if(std::wcscmp(codecs[i].MimeType,L"image/png")==0){saved=bitmap.Save(path.c_str(),&codecs[i].Clsid,nullptr)==Gdiplus::Ok;break;}}
    SelectObject(dc,old);DeleteObject(dib);DeleteDC(dc);Gdiplus::GdiplusShutdown(token);
    if(!saved)throw std::runtime_error("Credit UI PNG failed");
}
int main(int argc,char**argv){try{
    const auto path=std::filesystem::temp_directory_path()/
        ("xband-credit-ui-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64()))/"settings.json";
    auto settings=std::make_shared<diagnostic::ServiceCreditSettings>(path);
    if(settings->snapshot().configured||std::filesystem::exists(path))throw std::runtime_error("Initial settings silently configured rates");
    diagnostic::ServiceCreditWindow window;window.setSettings(settings);window.open(nullptr,false);
    const auto w=window.handle();
    const auto label=[&](int id){wchar_t text[1024]{};GetWindowTextW(GetDlgItem(w,id),text,1024);return std::wstring(text);};
    if(label(9).find(L"個別精算")==std::wstring::npos||
       label(9).find(L"相手の接続は待ちません")==std::wstring::npos||
       label(9).find(L"全タイトル共通処理")==std::wstring::npos||
       label(10).find(L"この端末の結果が未確認")==std::wstring::npos)
        throw std::runtime_error("Independent settlement or title-neutral explanation missing");
    wchar_t defaultMail[16]{},defaultMatch[16]{};
    GetWindowTextW(GetDlgItem(w,1),defaultMail,16);GetWindowTextW(GetDlgItem(w,2),defaultMatch,16);
    if(std::wstring(defaultMail)!=L"1"||std::wstring(defaultMatch)!=L"3"||settings->snapshot().mailEnabled||settings->snapshot().matchEnabled||
       SendMessageW(GetDlgItem(w,6),BM_GETCHECK,0,0)!=BST_UNCHECKED)
        throw std::runtime_error("Default amounts or opt-in state incorrect");
    SetWindowTextW(GetDlgItem(w,1),L"");
    SendMessageW(w,WM_COMMAND,3,0);
    if(settings->snapshot().configured)throw std::runtime_error("Empty UI input accepted");
    SetWindowTextW(GetDlgItem(w,1),L"1");SetWindowTextW(GetDlgItem(w,2),L"3");SendMessageW(w,WM_COMMAND,3,0);
    if(diagnostic::ServiceCreditSettings(path).snapshot()!=diagnostic::ServiceCreditValues{true,1,3})throw std::runtime_error("UI save/reopen failed");
    if(label(10).find(L"相手の接続は待ちません")==std::wstring::npos||
       label(10).find(L"各端末の新しい結果付き接続")==std::wstring::npos)
        throw std::runtime_error("Save message still requires peer connection");
    SetWindowTextW(GetDlgItem(w,1),L"32768");SendMessageW(w,WM_COMMAND,3,0);
    if(diagnostic::ServiceCreditSettings(path).snapshot()!=diagnostic::ServiceCreditValues{true,1,3})throw std::runtime_error("Invalid UI request changed file");
    settings->save(0,32767);
    if(diagnostic::ServiceCreditSettings(path).snapshot()!=diagnostic::ServiceCreditValues{true,0,32767})throw std::runtime_error("Boundary settings rejected");
    settings->save(1,3,true);
    if(!diagnostic::ServiceCreditSettings(path).snapshot().mailEnabled)throw std::runtime_error("Explicit enable not persisted");
    const auto legacy=path.parent_path()/"legacy.json";
    {std::ofstream out(legacy);out<<R"({"schema":1,"scope":"server-global","activation":"draft-not-active","requested_units":{"mail":7,"match":9}})";}
    const auto old=diagnostic::ServiceCreditSettings(legacy).snapshot();
    if(old.mail!=7||old.match!=9||old.mailEnabled||old.matchEnabled)throw std::runtime_error("Legacy settings silently activated");
    const auto mailOnly=path.parent_path()/"mail-only.json";
    {std::ofstream out(mailOnly);out<<R"({"schema":2,"scope":"server-global","activation":"explicit-mail-policy","mail_enabled":true,"requested_units":{"mail":2,"match":7,"reset":1}})";}
    const auto migrated=diagnostic::ServiceCreditSettings(mailOnly).snapshot();
    if(!migrated.mailEnabled||migrated.matchEnabled||migrated.mail!=2||migrated.match!=7)throw std::runtime_error("Schema2 match opt-in regression");
    SendMessageW(GetDlgItem(w,5),BM_SETCHECK,BST_CHECKED,0);
    SendMessageW(GetDlgItem(w,6),BM_SETCHECK,BST_CHECKED,0);
    SetWindowTextW(GetDlgItem(w,1),L"1");SetWindowTextW(GetDlgItem(w,2),L"3");SendMessageW(w,WM_COMMAND,3,0);
    const auto active=diagnostic::ServiceCreditSettings(path).snapshot();
    if(!active.mailEnabled||!active.matchEnabled||active.resetScope!=1||active.includeMail!=1)throw std::runtime_error("Explicit match/UI policy not persisted");
    for(const char* field:{"reset_scope","include_mail"})for(const auto value:{nlohmann::json(0),nlohmann::json(2),nlohmann::json(1.5)}){
        std::ifstream in(path);auto changed=nlohmann::json::parse(in);changed[field]=value;
        const auto invalid=path.parent_path()/"invalid-policy.json";{std::ofstream out(invalid);out<<changed;}
        bool refused=false;try{diagnostic::ServiceCreditSettings badPolicy(invalid);}catch(...){refused=true;}
        if(!refused)throw std::runtime_error("Unsupported policy accepted");
    }
    if(GetDlgItem(w,4))throw std::runtime_error("Diagnostic card debit button must not appear in normal UI");
    SendMessageW(w,WM_COMMAND,4,0); // Former diagnostic command has no normal UI route.
    bool diagnosticOpened=false;
    EnumThreadWindows(GetCurrentThreadId(),[](HWND candidate,LPARAM state)->BOOL{
        wchar_t className[64]{};GetClassNameW(candidate,className,64);
        if(std::wcscmp(className,L"XbandCardDebitTrial")==0)*reinterpret_cast<bool*>(state)=true;
        return TRUE;
    },reinterpret_cast<LPARAM>(&diagnosticOpened));
    if(diagnosticOpened)throw std::runtime_error("Former diagnostic command opened a trial window");
    for(int id:{1,2,3,5,6,7,8,9,10}){RECT r{};GetWindowRect(GetDlgItem(w,id),&r);if(r.right<=r.left||r.bottom<=r.top)throw std::runtime_error("Missing UI geometry");}
    if(argc==2){
        SetWindowTextW(GetDlgItem(w,1),L"1");SetWindowTextW(GetDlgItem(w,2),L"3");SendMessageW(w,WM_COMMAND,3,0);
        render(w,std::filesystem::absolute(argv[1]));
    }
    DestroyWindow(w);
    // Unsupported policy must not silently enable or change amounts.
    const auto bad=path.parent_path()/"bad.json";
    {std::ofstream out(bad);out<<R"({"schema":1,"scope":"server-global","activation":"active","requested_units":{"mail":1,"match":3}})";}
    bool rejected=false;try{diagnostic::ServiceCreditSettings invalid(bad);}catch(const std::exception&){rejected=true;}
    if(!rejected)throw std::runtime_error("Unsupported active charging policy accepted");
    std::cout<<"PASS credit UI defaults1/3, reset1, independent settlement explanations, opt-in persistence, legacy inactive migration, validation; fixture only\n";
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
