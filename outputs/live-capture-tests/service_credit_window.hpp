#pragma once
#include "service_credit_settings.hpp"
#include "card_debit_trial_window.hpp"
#include <string>

namespace diagnostic {
class ServiceCreditWindow {
    std::shared_ptr<ServiceCreditSettings> settings;
    HWND window=nullptr,message=nullptr;
    CardDebitTrialWindow trialWindow;
    static LRESULT CALLBACK proc(HWND w,UINT m,WPARAM a,LPARAM b){
        auto s=reinterpret_cast<ServiceCreditWindow*>(GetWindowLongPtrW(w,GWLP_USERDATA));
        if(m==WM_NCCREATE){s=static_cast<ServiceCreditWindow*>(reinterpret_cast<CREATESTRUCTW*>(b)->lpCreateParams);SetWindowLongPtrW(w,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(s));}
        if(!s)return DefWindowProcW(w,m,a,b);
        if(m==WM_COMMAND&&LOWORD(a)==4){s->trialWindow.open(w);return 0;}
        if(m==WM_COMMAND&&LOWORD(a)==3){
            try{
                auto value=[&](int id){wchar_t text[32]{};GetWindowTextW(GetDlgItem(w,id),text,32);
                    const std::wstring v=text;
                    if(v.empty()||v.find_first_not_of(L"0123456789")!=std::wstring::npos)throw std::runtime_error("Invalid units");
                    const auto n=std::stoul(v);if(n>32767)throw std::runtime_error("Invalid units");return unsigned(n);};
                const auto mail=value(1),match=value(2);
                const bool enabled=SendMessageW(GetDlgItem(w,5),BM_GETCHECK,0,0)==BST_CHECKED;
                const bool matches=SendMessageW(GetDlgItem(w,6),BM_GETCHECK,0,0)==BST_CHECKED;
                s->settings->save(mail,match,enabled,matches);
                SetWindowTextW(s->message,L"保存しました。新しい接続・対戦に適用します。\n各端末の新しい結果付き接続で一度だけ精算。相手の接続は待ちません。\n結果不明の再送・返還はしません。旧履歴からの自動精算はありません。");
            }catch(const std::exception&){SetWindowTextW(s->message,L"度数は0〜32767。保存に失敗した場合は旧設定を維持します。");}
            return 0;
        }
        if(m==WM_DESTROY){s->window=s->message=nullptr;return 0;}
        return DefWindowProcW(w,m,a,b);
    }
public:
    void setTrials(std::shared_ptr<CardDebitTrials> value){trialWindow.setTrials(std::move(value));}
    void setSettings(std::shared_ptr<ServiceCreditSettings> s){settings=std::move(s);}
    HWND handle()const{return window;}
    void open(HWND owner,bool show=true){
        if(!settings)return;
        if(window){if(show){ShowWindow(window,SW_SHOW);SetForegroundWindow(window);}return;}
        WNDCLASSW c{};c.lpfnWndProc=proc;c.hInstance=GetModuleHandleW(nullptr);c.lpszClassName=L"XbandServiceCreditDraft";
        c.hCursor=LoadCursor(nullptr,IDC_ARROW);c.hbrBackground=GetSysColorBrush(COLOR_BTNFACE);RegisterClassW(&c);
        window=CreateWindowW(c.lpszClassName,L"XBAND | サービス消費度数",WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU,
            CW_USEDEFAULT,CW_USEDEFAULT,680,610,owner,nullptr,c.hInstance,this);
        if(!window)throw std::runtime_error("Cannot create service credit settings window");
        auto control=[&](const wchar_t* cls,const wchar_t* label,DWORD style,int x,int y,int width,int height,int id){
            auto h=CreateWindowW(cls,label,WS_CHILD|WS_VISIBLE|style,x,y,width,height,window,reinterpret_cast<HMENU>(INT_PTR(id)),c.hInstance,nullptr);
            SendMessageW(h,WM_SETFONT,reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)),TRUE);return h;};
        control(L"STATIC",L"全電話番号・4ユーザー・全ゲーム共通。XBANDポイントとは別です。\nメールは接続ごとに指定度数をROMへ要求し、実績一致後に自動継続します。",0,18,18,590,44,0);
        control(L"STATIC",L"メールアクセス時の消費要求",0,18,88,280,24,0);
        control(L"STATIC",L"通常対戦の度数（次の接続で精算）",0,18,130,300,24,0);
        const auto v=settings->snapshot();
        for(int i=0;i<2;++i){const auto text=std::to_wstring(i?v.match:v.mail);
            auto edit=control(L"EDIT",text.c_str(),WS_BORDER|ES_NUMBER|WS_TABSTOP,320,82+i*42,140,28,i+1);
            SendMessageW(edit,EM_SETLIMITTEXT,5,0);}
        control(L"STATIC",L"途中リセット：対戦分1度数で固定（通常対戦の度数と置き換え）。\n0〜32767はローカル上限です。当時のサーバー料金上限ではありません。",0,18,176,590,44,0);
        auto enabled=control(L"BUTTON",L"メール接続の自動消費を有効にする",BS_AUTOCHECKBOX|WS_TABSTOP,18,226,480,28,5);
        SendMessageW(enabled,BM_SETCHECK,v.mailEnabled?BST_CHECKED:BST_UNCHECKED,0);
        auto matches=control(L"BUTTON",L"新しい対戦の自動精算を有効にする",BS_AUTOCHECKBOX|WS_TABSTOP,18,258,500,28,6);
        SendMessageW(matches,BM_SETCHECK,v.matchEnabled?BST_CHECKED:BST_UNCHECKED,0);
        control(L"STATIC",L"リセット時の対象",0,18,310,270,24,0);
        control(L"STATIC",L"対戦精算時のメール料金",0,18,352,270,24,0);
        control(L"STATIC",L"両側とも各1度数（今回指定の共通運用）",0,280,310,350,24,7);
        control(L"STATIC",L"有効なメール料金も加算（標準：4／2）",0,280,352,350,24,8);
        control(L"STATIC",L"全タイトル共通処理。各端末の新しい結果で個別精算（リセット：-608／-609）。\n未分類のエラー・強制再起動は自動精算しません。相手の接続は待ちません。",0,18,398,630,44,9);
        control(L"BUTTON",L"設定を保存",WS_TABSTOP,350,452,180,32,3);
        control(L"BUTTON",L"仮想カード消費試験...",WS_TABSTOP,18,452,260,32,4);
        message=control(L"STATIC",L"自動消費の有効化は明示操作のみ。相手側の責任は判定しません。\nこの端末の結果が未確認なら、消費・メール送受信・次の対戦を止めます。\n未挿入・不足・消費結果不明では再送・補充しません。",0,18,500,630,60,10);
        if(show)ShowWindow(window,SW_SHOW);
    }
};
}
