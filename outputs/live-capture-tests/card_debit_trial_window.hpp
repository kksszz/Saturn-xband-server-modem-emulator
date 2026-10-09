#pragma once
#include "card_debit_trials.hpp"
#include <windows.h>
#include <memory>

namespace diagnostic {
class CardDebitTrialWindow {
    std::shared_ptr<CardDebitTrials> trials;
    HWND window=nullptr,status=nullptr,note=nullptr;
    unsigned side()const{return unsigned(SendMessageW(GetDlgItem(window,1),CB_GETCURSEL,0,0));}
    static std::wstring wide(const std::string& s){return {s.begin(),s.end()};}
    void refresh(){
        if(!window||!trials)return;
        const auto row=trials->snapshot(side());
        auto text=L"Session: "+std::to_wstring(row.session)+L" / "+wide(row.phase)+
            (row.pending?L" / command pending":L"")+L"\n要求度数: "+std::to_wstring(row.amount)+
            L" / 接続時の報告値: "+(row.reported?std::to_wstring(*row.reported):L"未取得")+
            L"\nゲームDISC(ROM)の消費実績返信: "+(row.result?std::to_wstring(*row.result):L"未確認")+
            L"\n消費後のゲームDISC(ROM)報告残度数: "+(row.remaining?std::to_wstring(*row.remaining):L"未確認")+
            L"\n"+wide(row.detail)+L"\n前回: "+wide(row.last);
        SetWindowTextW(status,text.c_str());
        EnableWindow(GetDlgItem(window,4),!row.pending&&(!row.connected||row.phase=="Idle"));
        EnableWindow(GetDlgItem(window,5),!row.pending&&row.connected&&row.phase=="Completed"&&row.result==int64_t(row.amount)&&row.continuationVerified);
    }
    static LRESULT CALLBACK proc(HWND w,UINT m,WPARAM a,LPARAM b){
        auto s=reinterpret_cast<CardDebitTrialWindow*>(GetWindowLongPtrW(w,GWLP_USERDATA));
        if(m==WM_NCCREATE){s=static_cast<CardDebitTrialWindow*>(reinterpret_cast<CREATESTRUCTW*>(b)->lpCreateParams);SetWindowLongPtrW(w,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(s));}
        if(!s)return DefWindowProcW(w,m,a,b);
        if(m==WM_TIMER){s->refresh();return 0;}
        if(m==WM_COMMAND){
            try{
                const auto id=LOWORD(a);
                if(id==4){
                    auto number=[&](int control,unsigned maximum){wchar_t text[32]{};
                        GetWindowTextW(GetDlgItem(w,control),text,32);std::wstring value=text;
                        if(value.empty()||value.find_first_not_of(L"0123456789")!=std::wstring::npos)throw std::runtime_error("Invalid trial value");
                        auto n=std::stoul(value);if(n>maximum)throw std::runtime_error("Invalid trial value");return n;};
                    const auto amount=number(2,32767),seconds=number(3,300);
                    if(!seconds)throw std::runtime_error("Specify a positive timeout");
                    s->trials->reserve(s->side(),uint32_t(amount),uint64_t(seconds)*1000);
                    SetWindowTextW(s->note,L"一回だけ予約しました。仮想カードを挿した対象端末で接続してください。");
                }
                if(id==5){s->trials->continueTrial(s->side());SetWindowTextW(s->note,L"継続を要求しました。消費要求の再送・補充はしません。");}
                s->refresh();
            }catch(const std::exception&){SetWindowTextW(s->note,L"新しい接続でのみ予約できます。度数0〜32767、応答待ち1〜300秒を指定してください。");}
            return 0;
        }
        if(m==WM_DESTROY){KillTimer(w,1);s->window=s->status=s->note=nullptr;return 0;}
        return DefWindowProcW(w,m,a,b);
    }
public:
    void setTrials(std::shared_ptr<CardDebitTrials> value){trials=std::move(value);}
    HWND handle()const{return window;}
    void open(HWND owner,bool show=true){
        if(!trials)return;
        if(window){if(show){ShowWindow(window,SW_SHOW);SetForegroundWindow(window);}return;}
        WNDCLASSW c{};c.lpfnWndProc=proc;c.hInstance=GetModuleHandleW(nullptr);c.lpszClassName=L"XbandCardDebitTrial";
        c.hCursor=LoadCursor(nullptr,IDC_ARROW);c.hbrBackground=GetSysColorBrush(COLOR_BTNFACE);RegisterClassW(&c);
        window=CreateWindowW(c.lpszClassName,L"XBAND | 仮想カード消費試験（一回限り）",WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU,
            CW_USEDEFAULT,CW_USEDEFAULT,720,510,owner,nullptr,c.hInstance,this);
        if(!window)throw std::runtime_error("Cannot create card debit trial window");
        auto control=[&](const wchar_t* cls,const wchar_t* label,DWORD style,int x,int y,int width,int height,int id){
            auto h=CreateWindowW(cls,label,WS_CHILD|WS_VISIBLE|style,x,y,width,height,window,reinterpret_cast<HMENU>(INT_PTR(id)),c.hInstance,nullptr);
            SendMessageW(h,WM_SETFONT,reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)),TRUE);return h;};
        control(L"STATIC",L"実際に仮想カードの度数を減らす診断操作です。通常料金の設定ではありません。\n未送信の新しい接続に一回だけ49を送ります。自動再送・返還・次回の再実行はしません。",0,18,16,670,48,0);
        control(L"STATIC",L"対象端末",0,18,88,100,22,0);
        auto combo=control(L"COMBOBOX",L"",CBS_DROPDOWNLIST|WS_TABSTOP,130,82,180,140,1);
        SendMessageW(combo,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(L"端末1（左）"));
        SendMessageW(combo,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(L"端末2（右）"));SendMessageW(combo,CB_SETCURSEL,0,0);
        control(L"STATIC",L"要求度数",0,18,128,100,22,0);
        control(L"EDIT",L"",WS_BORDER|ES_NUMBER|WS_TABSTOP,130,122,120,28,2);
        control(L"STATIC",L"応答待ち（秒）",0,320,128,140,22,0);
        control(L"EDIT",L"",WS_BORDER|ES_NUMBER|WS_TABSTOP,470,122,120,28,3);
        SendMessageW(GetDlgItem(window,2),EM_SETLIMITTEXT,5,0);SendMessageW(GetDlgItem(window,3),EM_SETLIMITTEXT,3,0);
        control(L"BUTTON",L"次の接続で一回だけ要求",WS_TABSTOP,130,166,210,34,4);
        control(L"BUTTON",L"実績・残度数を確認して継続",WS_TABSTOP,370,166,220,34,5);
        status=control(L"STATIC",L"",0,18,218,670,134,0);
        note=control(L"STATIC",L"応答待ち・継続条件は試験用の安全確認です。当時の課金規則ではありません。\n残度数0への部分消費が確認できた場合は、不足案内で接続を終了します。\nその他の不一致・未確定時は継続・再消費・返還しません。同じ要求を再予約しないでください。",0,18,368,670,74,0);
        SetTimer(window,1,500,nullptr);refresh();if(show)ShowWindow(window,SW_SHOW);
    }
};
}
