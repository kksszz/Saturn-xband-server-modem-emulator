#pragma once
#include <windows.h>
#include <objbase.h>
#include <filesystem>
#include <optional>
#include <array>
#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>
#pragma comment(lib,"ole32.lib")

namespace diagnostic {
inline std::array<uint8_t,13> syntheticCard(unsigned units){
    if(units>32767)throw std::invalid_argument("Card units outside0..32767");
    GUID identity{};if(FAILED(CoCreateGuid(&identity)))throw std::runtime_error("Card identity generation failed");
    std::array<uint8_t,13> bytes{};
    std::copy_n(reinterpret_cast<const uint8_t*>(&identity),8,bytes.begin());
    for(unsigned i=0;i<5;++i){const auto weight=1u<<(3*(4-i));
        const auto digit=units/weight;bytes[8+i]=uint8_t((1u<<digit)-1);units%=weight;}
    return bytes;
}
// Create-only, never recharge/replace an existing card.
inline void createSyntheticCard(const std::filesystem::path& path,unsigned units){
    const auto bytes=syntheticCard(units);
    HANDLE file=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE)throw std::runtime_error("New card path exists or cannot be created");
    DWORD written=0;const bool ok=WriteFile(file,bytes.data(),DWORD(bytes.size()),&written,nullptr)&&
        written==bytes.size()&&FlushFileBuffers(file);CloseHandle(file);
    if(!ok)throw std::runtime_error("Card write failed; incomplete file retained");
}
class MediaCardExchangeWindow {
    HWND window=nullptr;std::optional<std::filesystem::path> result;
    void create(){
        wchar_t value[16]{};GetWindowTextW(GetDlgItem(window,101),value,16);
        unsigned units=0;
        {
            const std::wstring digits(value);
            if(digits.empty()||digits.size()>5||digits.find_first_not_of(L"0123456789")!=std::wstring::npos||
                (units=unsigned(std::stoul(digits)))>32767){
                MessageBoxW(window,L"度数は0～32767の整数で入力してください。",L"入力範囲",MB_OK|MB_ICONWARNING);return;}
        }
        try{
            wchar_t localData[32768]{};
            const auto length=GetEnvironmentVariableW(L"LOCALAPPDATA",localData,32768);
            if(!length||length>=32768)throw std::runtime_error("Local application data directory unavailable");
            const auto directory=std::filesystem::path(localData)/L"XBAND"/L"media-cards";
            std::filesystem::create_directories(directory);
            GUID id{};wchar_t name[40]{};
            if(FAILED(CoCreateGuid(&id))||!StringFromGUID2(id,name,40))throw std::runtime_error("Card filename generation failed");
            const auto path=directory/(std::wstring(name)+L".bin");
            createSyntheticCard(path,units);
            result=path;DestroyWindow(window);
        }catch(const std::exception&){MessageBoxW(window,L"カードを自動保存できませんでした。保存先の空き容量・アクセス権を確認してください。元のカードは保持します。",L"カードの差し替え",MB_OK|MB_ICONERROR);}
    }
    static LRESULT CALLBACK proc(HWND w,UINT msg,WPARAM wp,LPARAM lp){
        auto self=reinterpret_cast<MediaCardExchangeWindow*>(GetWindowLongPtrW(w,GWLP_USERDATA));
        if(msg==WM_NCCREATE){self=static_cast<MediaCardExchangeWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            self->window=w;SetWindowLongPtrW(w,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));}
        if(!self)return DefWindowProcW(w,msg,wp,lp);
        if(msg==WM_CREATE){
            auto control=[&](const wchar_t* type,const wchar_t* text,DWORD style,int x,int y,int width,int height,int id){
                auto h=CreateWindowW(type,text,WS_CHILD|WS_VISIBLE|style,x,y,width,height,w,reinterpret_cast<HMENU>(INT_PTR(id)),GetModuleHandleW(nullptr),nullptr);
                SendMessageW(h,WM_SETFONT,reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)),TRUE);return h;};
            control(L"STATIC",L"元のカードは保持します。挿入中の交換はOFF→ONを自動で行います。",0,16,14,470,36,0);
            control(L"STATIC",L"新規カードの度数",0,16,56,150,22,0);
            control(L"EDIT",L"100",WS_BORDER|WS_TABSTOP|ES_NUMBER,170,52,110,26,101);
            SendMessageW(GetDlgItem(w,101),EM_SETLIMITTEXT,5,0);
            control(L"BUTTON",L"50",WS_TABSTOP,292,52,80,26,102);
            control(L"BUTTON",L"100",WS_TABSTOP,384,52,80,26,103);
            control(L"STATIC",L"入力可能範囲：0～32767度数（0は試験用）",0,16,88,460,24,107);
            control(L"STATIC",L"新しいカードは自動保存し、指定度数で差し替えます。",0,16,116,460,24,0);
            control(L"BUTTON",L"OK",WS_TABSTOP|BS_DEFPUSHBUTTON,224,170,120,28,105);
            control(L"BUTTON",L"キャンセル",WS_TABSTOP,356,170,120,28,106);return 0;
        }
        if(msg==WM_COMMAND&&HIWORD(wp)==BN_CLICKED){switch(LOWORD(wp)){
            case 102:SetWindowTextW(GetDlgItem(w,101),L"50");return 0;
            case 103:SetWindowTextW(GetDlgItem(w,101),L"100");return 0;
            case 105:self->create();return 0;
            case 106:DestroyWindow(w);return 0;}}
        if(msg==WM_DESTROY){self->window=nullptr;return 0;}
        return DefWindowProcW(w,msg,wp,lp);
    }
public:
    static void testControls(){
        MediaCardExchangeWindow fixture;WNDCLASSW cls{};cls.lpfnWndProc=proc;cls.hInstance=GetModuleHandleW(nullptr);
        cls.lpszClassName=L"XbandCardExchangeTest";RegisterClassW(&cls);
        const auto w=CreateWindowW(cls.lpszClassName,L"",WS_POPUP,-30000,-30000,510,290,nullptr,nullptr,cls.hInstance,&fixture);
        if(!w)throw std::runtime_error("Card exchange test window failed");
        auto check=[&](bool ok){if(!ok){DestroyWindow(w);throw std::runtime_error("Card exchange UI assertion");}};
        wchar_t text[100]{};
        check(!GetDlgItem(w,104)); // Existing-file selection is intentionally absent.
        GetWindowTextW(GetDlgItem(w,105),text,100);check(std::wstring(text)==L"OK");
        SendMessageW(w,WM_COMMAND,MAKEWPARAM(102,BN_CLICKED),0);GetWindowTextW(GetDlgItem(w,101),text,100);check(std::wstring(text)==L"50");
        SendMessageW(w,WM_COMMAND,MAKEWPARAM(103,BN_CLICKED),0);GetWindowTextW(GetDlgItem(w,101),text,100);check(std::wstring(text)==L"100");
        GetWindowTextW(GetDlgItem(w,107),text,100);check(std::wstring(text).find(L"0～32767")!=std::wstring::npos);
        check(SendMessageW(GetDlgItem(w,101),EM_GETLIMITTEXT,0,0)==5);DestroyWindow(w);
    }
    std::optional<std::filesystem::path> open(HWND owner,unsigned side){
        result.reset();WNDCLASSW cls{};cls.lpfnWndProc=proc;cls.hInstance=GetModuleHandleW(nullptr);
        cls.lpszClassName=L"XbandMediaCardExchange";cls.hCursor=LoadCursorW(nullptr,MAKEINTRESOURCEW(32512));
        cls.hbrBackground=reinterpret_cast<HBRUSH>(COLOR_BTNFACE+1);RegisterClassW(&cls);
        RECT rect{};GetWindowRect(owner,&rect);const auto title=L"モデム"+std::to_wstring(side+1)+L"：カードの差し替え";
        HWND dialog=CreateWindowExW(WS_EX_DLGMODALFRAME,cls.lpszClassName,title.c_str(),WS_CAPTION|WS_SYSMENU,
            rect.left+40,rect.top+70,510,246,owner,nullptr,cls.hInstance,this);
        if(!dialog)return {};EnableWindow(owner,FALSE);ShowWindow(dialog,SW_SHOW);MSG message{};
        while(window){const auto status=GetMessageW(&message,nullptr,0,0);if(status<=0){if(status==0)PostQuitMessage(int(message.wParam));break;}
            if(message.message==WM_KEYDOWN&&message.wParam==VK_ESCAPE){DestroyWindow(dialog);continue;}
            if(!IsDialogMessageW(dialog,&message)){TranslateMessage(&message);DispatchMessageW(&message);}}
        if(window)DestroyWindow(window);EnableWindow(owner,TRUE);SetForegroundWindow(owner);return result;
    }
};
}
