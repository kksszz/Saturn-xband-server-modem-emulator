#pragma once
#include "local_mail_journal.hpp"
#include "game_ranking_settings.hpp"
#include "mail_history_view.hpp"
namespace diagnostic {
class ServerBroadcastWindow {
    std::shared_ptr<LocalMailJournal> journal;
    HWND window=nullptr,scope=nullptr,phone=nullptr,slot=nullptr,subject=nullptr,body=nullptr,status=nullptr,list=nullptr,detail=nullptr;
    nlohmann::json rows=nlohmann::json::array();
    static std::wstring value(HWND w){const auto n=GetWindowTextLengthW(w);std::wstring text(n+1,L'\0');GetWindowTextW(w,text.data(),n+1);text.resize(n);return text;}
    void showDetail(){auto i=int(SendMessageW(list,LVM_GETNEXTITEM,WPARAM(-1),LVNI_SELECTED));
        if(i<0||size_t(i)>=rows.size()){SetWindowTextW(detail,L"");return;}
        auto text=rankingWide(rows[i].at("body").get<std::string>());std::wstring lines;
        for(auto c:text){if(c==L'\n')lines+=L'\r';lines+=c;}SetWindowTextW(detail,lines.c_str());}
    void refresh(){try{rows=journal->broadcastHistory();SendMessageW(list,LVM_DELETEALLITEMS,0,0);
        for(size_t i=0;i<rows.size();++i){const auto& r=rows[i];auto to=rankingWide(r.at("target_phone").get<std::string>());
            if(to.empty())to=L"全ユーザー（今後の接続も含む）";
            else to+=r.at("target_profile").get<int>()<0?L" / 全4枠":L" / 枠"+std::to_wstring(r.at("target_profile").get<int>()+1);
            std::array<std::wstring,5> cells{std::to_wstring(r.at("id").get<uint64_t>()),mailHistoryTime(r.at("accepted_unix_ms").get<uint64_t>()),to,
                rankingWide(r.at("subject").get<std::string>()),std::to_wstring(r.at("prepared_accounts").get<size_t>())};
            LVITEMW item{};item.mask=LVIF_TEXT;item.iItem=int(i);item.pszText=cells[0].data();SendMessageW(list,LVM_INSERTITEMW,0,reinterpret_cast<LPARAM>(&item));
            for(int c=1;c<5;++c){item.iSubItem=c;item.pszText=cells[c].data();SendMessageW(list,LVM_SETITEMTEXTW,i,reinterpret_cast<LPARAM>(&item));}}
        showDetail();}catch(const std::exception& e){SetWindowTextW(status,rankingWide(e.what()).c_str());}}
    static LRESULT CALLBACK proc(HWND w,UINT m,WPARAM a,LPARAM b){
        auto s=reinterpret_cast<ServerBroadcastWindow*>(GetWindowLongPtrW(w,GWLP_USERDATA));
        if(m==WM_NCCREATE){s=static_cast<ServerBroadcastWindow*>(reinterpret_cast<CREATESTRUCTW*>(b)->lpCreateParams);SetWindowLongPtrW(w,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(s));}
        if(!s)return DefWindowProcW(w,m,a,b);
        if(m==WM_COMMAND&&LOWORD(a)==1101&&HIWORD(a)==CBN_SELCHANGE){const bool specific=SendMessageW(s->scope,CB_GETCURSEL,0,0)==1;EnableWindow(s->phone,specific);EnableWindow(s->slot,specific);return 0;}
        if(m==WM_COMMAND&&LOWORD(a)==1105){try{
            const auto title=value(s->subject),text=value(s->body);(void)broadcastEUC(title,32);(void)broadcastEUC(text,broadcastBodyMaxBytes);
            const bool specific=SendMessageW(s->scope,CB_GETCURSEL,0,0)==1;
            auto number=specific?rankingUTF8(value(s->phone)):std::string{};
            if(specific&&xband::phoneDigits(number).empty())throw std::invalid_argument("Recipient phone required");
            const int profile=specific?int(SendMessageW(s->slot,CB_GETCURSEL,0,0))-1:-1;
            if(MessageBoxW(w,L"配信キューへ登録します。各対象ユーザーの次回接続時に送信します。登録してよろしいですか？",L"XBAND 配信登録",MB_YESNO|MB_ICONQUESTION)!=IDYES)return 0;
            const auto id=s->journal->publishBroadcast(title,text,number,profile);
            SetWindowTextW(s->status,(L"配信ID "+std::to_wstring(id)+L" を保存しました。各ユーザーの次回接続時に配送します。").c_str());
            SetWindowTextW(s->subject,L"");SetWindowTextW(s->body,L"");s->refresh();
        }catch(const std::exception& e){SetWindowTextW(s->status,(L"登録できません: "+rankingWide(e.what())).c_str());}return 0;}
        if(m==WM_COMMAND&&LOWORD(a)==1106){s->refresh();return 0;}
        if(m==WM_COMMAND&&LOWORD(a)==1110){
            if((!value(s->subject).empty()||!value(s->body).empty())&&MessageBoxW(w,L"入力中の件名と本文をサンプルメールに置き換えますか？",L"サンプルメール作成",MB_YESNO|MB_ICONQUESTION)!=IDYES)return 0;
            SetWindowTextW(s->subject,broadcastSampleSubject);SetWindowTextW(s->body,broadcastSampleText);
            SetWindowTextW(s->status,L"画像の文面を入力しました（未登録・未配信）。内容と配信先を確認して「配信キューへ登録」を押してください。");return 0;}
        if(m==WM_NOTIFY&&reinterpret_cast<NMHDR*>(b)->hwndFrom==s->list&&reinterpret_cast<NMHDR*>(b)->code==LVN_ITEMCHANGED){s->showDetail();return 0;}
        if(m==WM_TIMER){s->refresh();return 0;}
        if(m==WM_CLOSE){DestroyWindow(w);return 0;}
        if(m==WM_DESTROY){s->window=nullptr;return 0;}return DefWindowProcW(w,m,a,b);
    }
public:
    void setJournal(std::shared_ptr<LocalMailJournal> j){journal=std::move(j);}
    HWND handle()const{return window;}
    void open(HWND owner,bool visible=true){
        if(!journal)return;if(window){ShowWindow(window,SW_SHOW);SetForegroundWindow(window);return;}
        WNDCLASSW c{};c.lpfnWndProc=proc;c.hInstance=GetModuleHandleW(nullptr);c.lpszClassName=L"XBANDServerBroadcast";c.hCursor=LoadCursor(nullptr,IDC_ARROW);c.hbrBackground=reinterpret_cast<HBRUSH>(COLOR_BTNFACE+1);RegisterClassW(&c);
        window=CreateWindowW(c.lpszClassName,L"XBAND | センター配信メール",WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX,CW_USEDEFAULT,CW_USEDEFAULT,960,750,owner,nullptr,c.hInstance,this);
        auto control=[&](const wchar_t* cls,const wchar_t* text,DWORD style,int x,int y,int width,int height,int id=0){auto h=CreateWindowW(cls,text,WS_CHILD|WS_VISIBLE|style,x,y,width,height,window,reinterpret_cast<HMENU>(INT_PTR(id)),c.hInstance,nullptr);SendMessageW(h,WM_SETFONT,reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)),TRUE);return h;};
        control(L"STATIC",L"差出人: XBAND / 住所: 東京 / ROM内のXBANDアイコンを使用。接続中に即時プッシュする機能ではありません。",0,18,16,900,24);
        control(L"STATIC",L"配信先",0,18,56,80,24);scope=control(L"COMBOBOX",L"",CBS_DROPDOWNLIST|WS_TABSTOP,110,52,260,150,1101);
        for(auto text:{L"全ユーザー",L"電話番号・ユーザー枠を指定"})SendMessageW(scope,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(text));SendMessageW(scope,CB_SETCURSEL,0,0);
        phone=control(L"EDIT",L"",WS_BORDER|WS_TABSTOP|ES_AUTOHSCROLL,390,52,230,26,1102);SendMessageW(phone,EM_SETLIMITTEXT,24,0);
        slot=control(L"COMBOBOX",L"",CBS_DROPDOWNLIST|WS_TABSTOP,640,52,260,150,1103);
        for(auto text:{L"その端末の全4枠",L"ユーザー枠1",L"ユーザー枠2",L"ユーザー枠3",L"ユーザー枠4"})SendMessageW(slot,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(text));SendMessageW(slot,CB_SETCURSEL,0,0);EnableWindow(phone,FALSE);EnableWindow(slot,FALSE);
        control(L"STATIC",L"件名",0,18,96,80,24);subject=control(L"EDIT",L"",WS_BORDER|WS_TABSTOP|ES_AUTOHSCROLL,110,92,790,26,1107);SendMessageW(subject,EM_SETLIMITTEXT,32,0);
        control(L"STATIC",L"本文",0,18,136,80,24);body=control(L"EDIT",L"",WS_BORDER|WS_TABSTOP|ES_MULTILINE|ES_AUTOVSCROLL|WS_VSCROLL|ES_WANTRETURN,110,132,790,120,1108);SendMessageW(body,EM_SETLIMITTEXT,240,0);
        control(L"STATIC",L"件名32 / 本文240バイト以内（EUC-JP、日本語は通常2バイト）。絵文字不可。受信箱満杯ならサーバーに残します。",0,18,266,900,24);
        control(L"BUTTON",L"配信キューへ登録",WS_TABSTOP,110,302,230,32,1105);control(L"BUTTON",L"履歴を更新",WS_TABSTOP,358,302,150,32,1106);
        control(L"BUTTON",L"サンプルメール作成",WS_TABSTOP,526,302,230,32,1110);
        status=control(L"STATIC",L"登録は永続保存されます。応答への格納後は自動再送しません。ROMの受信・既読完了は未確認です。",0,18,350,900,38);
        INITCOMMONCONTROLSEX common{sizeof(common),ICC_LISTVIEW_CLASSES};InitCommonControlsEx(&common);
        list=control(WC_LISTVIEWW,L"",WS_BORDER|WS_TABSTOP|LVS_REPORT|LVS_SINGLESEL|LVS_SHOWSELALWAYS,18,400,900,180,1109);
        SendMessageW(list,LVM_SETEXTENDEDLISTVIEWSTYLE,0,LVS_EX_FULLROWSELECT|LVS_EX_GRIDLINES|LVS_EX_DOUBLEBUFFER);
        const wchar_t* labels[]={L"配信ID",L"登録日時 (JST)",L"配信先",L"件名",L"応答格納ユーザー数"};const int widths[]={65,155,220,270,165};
        for(int i=0;i<5;++i){LVCOLUMNW col{};col.mask=LVCF_TEXT|LVCF_WIDTH;col.cx=widths[i];col.pszText=const_cast<wchar_t*>(labels[i]);SendMessageW(list,LVM_INSERTCOLUMNW,i,reinterpret_cast<LPARAM>(&col));}
        detail=control(L"EDIT",L"履歴を選択すると本文を表示します。",WS_BORDER|ES_MULTILINE|ES_READONLY|WS_VSCROLL,18,594,900,95);
        refresh();if(visible)ShowWindow(window,SW_SHOW);
    }
};
}
