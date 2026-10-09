#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commctrl.h>
#pragma comment(lib,"comctl32.lib")
#include <nlohmann/json.hpp>
#include <thread>
#include <mutex>
#include <atomic>
#include <deque>
#include <sstream>
#include <iomanip>
#include "xband_mail_status.hpp"
#include "modem_phone_display.hpp"
#include "game_ranking_settings.hpp"
#include "usage_area_settings.hpp"
#include "standby_wait_settings.hpp"
#include "local_mail_journal.hpp"
#include "mail_history_view.hpp"
#include "activity_history_window.hpp"
#include "game_level_window.hpp"
#include "peer_byte_text.hpp"
#include "server_broadcast_window.hpp"
#include "service_credit_window.hpp"
#ifdef XBAND_DASHBOARD_RENDER_TEST
#include <gdiplus.h>
#include <filesystem>
#include <vector>
#endif

// Display-only snapshots. No guest input, network I/O or protocol decisions.
class XbandDashboard {
    using J=nlohmann::json;
    std::thread worker;std::mutex mutex;J data;
    std::atomic<HWND> hwnd{nullptr};std::atomic<bool> ready{false},closed{false};
    std::array<uint64_t,2> prior{},rate{};uint64_t last=0;
    std::deque<std::wstring> events;std::string previousState;
    std::shared_ptr<diagnostic::GameRankingSettings> rankingSettings;
    std::shared_ptr<diagnostic::LocalMailJournal> mailJournal;
    diagnostic::ActivityHistoryWindow activityWindow;
    diagnostic::ActivityHistoryWindow resultWindow;
    diagnostic::GameLevelWindow levelWindow;
    diagnostic::ServerBroadcastWindow broadcastWindow;
    diagnostic::ServiceCreditWindow creditWindow;
    HWND mailWindow=nullptr,mailText=nullptr,mailList=nullptr,mailSummary=nullptr;
    J mailRows=J::array();bool mailFilling=false,mailRaw=false;
    size_t mailPage=0;
    void mailDetail(){
        const auto selected=int(SendMessageW(mailList,LVM_GETNEXTITEM,WPARAM(-1),LVNI_SELECTED));
        const auto value=selected>=0&&size_t(selected)<mailRows.size()?
            diagnostic::mailHistoryDetail(mailRows[size_t(selected)],mailRaw):L"\u884c\u3092\u9078\u629e\u3059\u308b\u3068\u8a73\u7d30\u3092\u8868\u793a\u3057\u307e\u3059\u3002";
        SetWindowTextW(mailText,value.c_str());
    }
    void fillMail(const J& result){
        mailFilling=true;SendMessageW(mailList,WM_SETREDRAW,FALSE,0);
        SendMessageW(mailList,LVM_DELETEALLITEMS,0,0);mailRows=result.at("rows");
        for(size_t i=0;i<mailRows.size();++i){auto cells=diagnostic::mailHistoryCells(mailRows[i]);
            LVITEMW item{};item.mask=LVIF_TEXT;item.iItem=int(i);item.pszText=cells[0].data();
            SendMessageW(mailList,LVM_INSERTITEMW,0,reinterpret_cast<LPARAM>(&item));
            for(unsigned col=1;col<cells.size();++col){item.iSubItem=int(col);item.pszText=cells[col].data();SendMessageW(mailList,LVM_SETITEMTEXTW,i,reinterpret_cast<LPARAM>(&item));}
        }
        const auto total=result.at("total_observations").get<size_t>();
        const auto size=result.at("page_size").get<size_t>();
        const auto label=L"\u5168 "+std::to_wstring(total)+L" \u4ef6  |  \u30da\u30fc\u30b8 "+std::to_wstring(mailPage+1)+L" / "+std::to_wstring(std::max(size_t(1),(total+size-1)/size));
        SetWindowTextW(mailSummary,label.c_str());
        EnableWindow(GetDlgItem(mailWindow,980),mailPage!=0);
        EnableWindow(GetDlgItem(mailWindow,981),mailPage<(total?((total-1)/size):0));
        if(!mailRows.empty()){LVITEMW selected{};selected.stateMask=LVIS_SELECTED|LVIS_FOCUSED;selected.state=selected.stateMask;SendMessageW(mailList,LVM_SETITEMSTATE,0,reinterpret_cast<LPARAM>(&selected));}
        SendMessageW(mailList,WM_SETREDRAW,TRUE,0);InvalidateRect(mailList,nullptr,TRUE);mailFilling=false;mailDetail();
    }
    void refreshMail(){
        try{
            auto result=mailJournal->page(mailPage);
            const auto total=result.at("total_observations").get<size_t>();
            const auto lastPage=total?(total-1)/result.at("page_size").get<size_t>():0;
            if(mailPage>lastPage){mailPage=lastPage;result=mailJournal->page(mailPage);}
            fillMail(result);
        }catch(const std::exception& e){const auto value=wide(std::string("History read error: ")+e.what());SetWindowTextW(mailText,value.c_str());}
    }
    static LRESULT CALLBACK mailProc(HWND w,UINT m,WPARAM a,LPARAM b){
        auto s=reinterpret_cast<XbandDashboard*>(GetWindowLongPtrW(w,GWLP_USERDATA));
        if(m==WM_NCCREATE){s=static_cast<XbandDashboard*>(reinterpret_cast<CREATESTRUCTW*>(b)->lpCreateParams);SetWindowLongPtrW(w,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(s));}
        if(!s)return DefWindowProcW(w,m,a,b);
        if(m==WM_COMMAND){
            if(LOWORD(a)==980&&s->mailPage)--s->mailPage;
            else if(LOWORD(a)==981)++s->mailPage;
            if(LOWORD(a)>=980&&LOWORD(a)<=982){s->refreshMail();return 0;}
            if(LOWORD(a)==983){s->mailRaw=!s->mailRaw;s->mailDetail();return 0;}
            if(LOWORD(a)==984){s->broadcastWindow.open(w);return 0;}
        }
        if(m==WM_NOTIFY&&reinterpret_cast<NMHDR*>(b)->hwndFrom==s->mailList&&reinterpret_cast<NMHDR*>(b)->code==LVN_ITEMCHANGED){if(!s->mailFilling)s->mailDetail();return 0;}
        if(m==WM_SIZE&&s->mailText){
            const int width=std::max(100,int(LOWORD(b))-24),height=int(HIWORD(b));
            const int table=std::max(100,height-290);
            MoveWindow(s->mailList,12,92,width,table,TRUE);
            MoveWindow(s->mailText,12,104+table,width,std::max(80,height-116-table),TRUE);return 0;
        }
        if(m==WM_CLOSE){DestroyWindow(w);return 0;}
        if(m==WM_DESTROY){s->mailWindow=s->mailText=s->mailList=s->mailSummary=nullptr;return 0;}
        return DefWindowProcW(w,m,a,b);
    }
    void openMail(HWND owner,bool show=true){
        if(mailWindow){ShowWindow(mailWindow,SW_SHOW);SetForegroundWindow(mailWindow);return;}
        WNDCLASSW c{};c.lpfnWndProc=mailProc;c.hInstance=GetModuleHandleW(nullptr);c.lpszClassName=L"XBANDLocalMailHistory";c.hCursor=LoadCursor(nullptr,IDC_ARROW);c.hbrBackground=reinterpret_cast<HBRUSH>(COLOR_WINDOW+1);RegisterClassW(&c);
        INITCOMMONCONTROLSEX common{sizeof(common),ICC_LISTVIEW_CLASSES};InitCommonControlsEx(&common);
        mailWindow=CreateWindowW(c.lpszClassName,L"XBAND | \u30e1\u30fc\u30eb\u5c65\u6b74",WS_OVERLAPPEDWINDOW,CW_USEDEFAULT,CW_USEDEFAULT,1240,760,owner,nullptr,c.hInstance,this);
        if(!mailWindow)return;
        for(unsigned i=0;i<3;++i)CreateWindowW(L"BUTTON",i==0?L"前へ":i==1?L"次へ":L"更新",WS_CHILD|WS_VISIBLE|WS_TABSTOP,12+i*110,12,100,30,mailWindow,reinterpret_cast<HMENU>(INT_PTR(980+i)),c.hInstance,nullptr);
        CreateWindowW(L"BUTTON",L"\u89e3\u6790\u30c7\u30fc\u30bf\u5207\u66ff",WS_CHILD|WS_VISIBLE|WS_TABSTOP,342,12,160,30,mailWindow,reinterpret_cast<HMENU>(INT_PTR(983)),c.hInstance,nullptr);
        CreateWindowW(L"BUTTON",L"センター配信メール...",WS_CHILD|WS_VISIBLE|WS_TABSTOP,520,12,180,30,mailWindow,reinterpret_cast<HMENU>(INT_PTR(984)),c.hInstance,nullptr);
        mailSummary=CreateWindowW(L"STATIC",L"",WS_CHILD|WS_VISIBLE,720,18,450,24,mailWindow,nullptr,c.hInstance,nullptr);
        CreateWindowW(L"STATIC",L"\u300c\u5fdc\u7b54\u3078\u683c\u7d0d\u300d\u306fゲームDISC(ROM)\u306e\u53d7\u4fe1\u5b8c\u4e86\u3092\u610f\u5473\u3057\u307e\u305b\u3093\u3002\u518d\u6295\u7a3f\u306f\u5225\u5c65\u6b74\u3068\u3057\u3066\u8a18\u9332\u3055\u308c\u307e\u3059\u3002",WS_CHILD|WS_VISIBLE,12,55,1180,28,mailWindow,nullptr,c.hInstance,nullptr);
        mailList=CreateWindowW(WC_LISTVIEWW,L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP|WS_BORDER|LVS_REPORT|LVS_SINGLESEL|LVS_SHOWSELALWAYS,12,92,1200,430,mailWindow,nullptr,c.hInstance,nullptr);
        SendMessageW(mailList,LVM_SETEXTENDEDLISTVIEWSTYLE,0,LVS_EX_FULLROWSELECT|LVS_EX_GRIDLINES|LVS_EX_DOUBLEBUFFER);
        const wchar_t* labels[]={L"ID",L"\u53d7\u4ed8\u65e5\u6642 (JST)",L"\u9001\u4fe1\u8005",L"\u9001\u4fe1\u5143\u96fb\u8a71",L"\u67a0",L"\u5b9b\u5148",L"\u4ef6\u540d",L"\u72b6\u614b"};
        const int widths[]={65,165,110,125,45,110,230,240};
        for(unsigned i=0;i<8;++i){LVCOLUMNW col{};col.mask=LVCF_TEXT|LVCF_WIDTH;col.cx=widths[i];col.pszText=const_cast<wchar_t*>(labels[i]);SendMessageW(mailList,LVM_INSERTCOLUMNW,i,reinterpret_cast<LPARAM>(&col));}
        mailText=CreateWindowW(L"EDIT",L"",WS_CHILD|WS_VISIBLE|WS_VSCROLL|ES_MULTILINE|ES_READONLY|ES_AUTOVSCROLL,12,534,1200,175,mailWindow,nullptr,c.hInstance,nullptr);
        for(HWND child=GetWindow(mailWindow,GW_CHILD);child;child=GetWindow(child,GW_HWNDNEXT))SendMessageW(child,WM_SETFONT,reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)),TRUE);
        SendMessageW(mailText,EM_SETLIMITTEXT,1024*1024,0);mailPage=0;mailRaw=false;refreshMail();
        RECT client{};GetClientRect(mailWindow,&client);SendMessageW(mailWindow,WM_SIZE,0,MAKELPARAM(client.right,client.bottom));if(show)ShowWindow(mailWindow,SW_SHOW);
    }
    HWND rankingWindow=nullptr,rankingSelector=nullptr,rankingMessage=nullptr;
    HWND rankingAward=nullptr;
    HWND rankingAwardValue=nullptr;
    HWND rankingLoseValue=nullptr;
    std::array<HWND,5> rankingEdits{};
    std::vector<diagnostic::GameRankingRow> rankingRows;
    std::shared_ptr<diagnostic::UsageAreaSettings> usageSettings;
    std::shared_ptr<diagnostic::StandbyWaitSettings> waitSettings;
    HWND waitWindow=nullptr,waitMessage=nullptr;
    std::array<HWND,3> waitSelectors{};
    void fillWaitSettings(const diagnostic::StandbyWaitValues& values){
        for(unsigned i=0;i<3;++i)SendMessageW(waitSelectors[i],CB_SETCURSEL,values.minutes[i]-1,0);
    }
    void showSavedWaitSettings(bool justSaved=false){
        const auto v=waitSettings->snapshot().minutes;
        const auto text=std::wstring(justSaved?L"保存しました：":L"保存済み：")+L"みじかい "+std::to_wstring(v[0])+L"分 ／ ふつう "+std::to_wstring(v[1])+L"分 ／ ながい "+std::to_wstring(v[2])+L"分\n全番号・ID・タイトルの次の申し込みから適用します。";
        SetWindowTextW(waitMessage,text.c_str());
    }
    static LRESULT CALLBACK waitProc(HWND w,UINT m,WPARAM a,LPARAM b){
        auto s=reinterpret_cast<XbandDashboard*>(GetWindowLongPtrW(w,GWLP_USERDATA));
        if(m==WM_NCCREATE){s=static_cast<XbandDashboard*>(reinterpret_cast<CREATESTRUCTW*>(b)->lpCreateParams);SetWindowLongPtrW(w,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(s));}
        if(!s)return DefWindowProcW(w,m,a,b);
        if(m==WM_COMMAND&&LOWORD(a)==973){try{
            diagnostic::StandbyWaitValues values;
            for(unsigned i=0;i<3;++i){const auto index=SendMessageW(s->waitSelectors[i],CB_GETCURSEL,0,0);
                if(index<0||index>=diagnostic::standbyWaitMaxMinutes)throw std::runtime_error("Select all three wait times");
                values.minutes[i]=unsigned(index)+1;}
            s->waitSettings->save(values);
            s->showSavedWaitSettings(true);
        }catch(const std::exception& e){SetWindowTextW(s->waitMessage,diagnostic::rankingWide(e.what()).c_str());}return 0;}
        if(m==WM_COMMAND&&LOWORD(a)==974){s->fillWaitSettings({});SetWindowTextW(s->waitMessage,L"初期値を表示しました。適用するには保存してください。");return 0;}
        if(m==WM_DESTROY){s->waitWindow=nullptr;return 0;}
        return DefWindowProcW(w,m,a,b);
    }
    void openWait(HWND owner,bool show=true){
        if(!waitSettings)return;
        if(waitWindow){if(show){ShowWindow(waitWindow,SW_SHOW);SetForegroundWindow(waitWindow);}return;}
        WNDCLASSW c{};c.lpfnWndProc=waitProc;c.hInstance=GetModuleHandleW(nullptr);c.lpszClassName=L"XbandGlobalWait";c.hCursor=LoadCursor(nullptr,IDC_ARROW);c.hbrBackground=GetSysColorBrush(COLOR_BTNFACE);RegisterClassW(&c);
        waitWindow=CreateWindowW(c.lpszClassName,L"XBAND | 対戦待ち時間設定（全端末共通）",WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU,CW_USEDEFAULT,CW_USEDEFAULT,570,430,owner,nullptr,c.hInstance,this);
        if(!waitWindow)return;
        auto control=[&](const wchar_t* cls,const wchar_t* label,DWORD style,int x,int y,int width,int height,int id){
            const auto h=CreateWindowW(cls,label,WS_CHILD|WS_VISIBLE|style,x,y,width,height,waitWindow,reinterpret_cast<HMENU>(INT_PTR(id)),c.hInstance,nullptr);
            SendMessageW(h,WM_SETFONT,reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)),TRUE);return h;};
        control(L"STATIC",L"全電話番号・4ユーザー・全ゲーム共通の設定です。\nゲームDISC(ROM)で選んだ「みじかい／ふつう／ながい」に対応する時間を指定します。",0,18,18,520,42,0);
        const wchar_t* labels[]{L"みじかい",L"ふつう",L"ながい"};
        for(unsigned i=0;i<3;++i){const int y=78+int(i)*46;
            control(L"STATIC",labels[i],0,18,y+5,120,24,0);
            waitSelectors[i]=control(L"COMBOBOX",L"",CBS_DROPDOWNLIST|WS_VSCROLL|WS_TABSTOP,144,y,170,210,970+i);
            for(unsigned n=1;n<=diagnostic::standbyWaitMaxMinutes;++n){const auto label=std::to_wstring(n)+L" 分";SendMessageW(waitSelectors[i],CB_ADDSTRING,0,reinterpret_cast<LPARAM>(label.c_str()));}
        }
        control(L"STATIC",L"1〜60分。みじかい ≦ ふつう ≦ ながい（同じ値も設定可能）。\n保存後の新しい申し込みから、通知文と待機期限に同じ値を使います。\nすでに待機中の期限は変更しません。",0,18,216,520,56,0);
        control(L"BUTTON",L"初期値を表示",WS_TABSTOP,144,282,170,32,974);
        control(L"BUTTON",L"保存",WS_TABSTOP|BS_DEFPUSHBUTTON,340,282,170,32,973);
        waitMessage=control(L"STATIC",L"初期値：みじかい1分／ふつう2分／ながい4分。変更後は保存してください。",0,18,330,520,42,0);
        fillWaitSettings(waitSettings->snapshot());
        showSavedWaitSettings();
        if(show)ShowWindow(waitWindow,SW_SHOW);
    }
    HWND usageWindow=nullptr,usagePhone=nullptr,usageSelector=nullptr,usageMessage=nullptr;
    HWND usagePlayMode=nullptr;
    std::array<HWND,2> usagePlayEdits{};
    HWND usageScheduleCheck=nullptr,usageScheduleScope=nullptr;
    std::array<HWND,2> usageDayModes{},usageDayStarts{},usageDayEnds{};
    static std::string usageText(HWND edit){const int n=GetWindowTextLengthW(edit);std::wstring w(size_t(n)+1,0);GetWindowTextW(edit,w.data(),n+1);w.resize(n);return diagnostic::rankingUTF8(w);}
    void enableUsagePlay(){const bool schedule=usageScheduleCheck&&SendMessageW(usageScheduleCheck,BM_GETCHECK,0,0)==BST_CHECKED;
        const auto mode=SendMessageW(usagePlayMode,CB_GETCURSEL,0,0);for(auto edit:usagePlayEdits)EnableWindow(edit,!schedule&&mode==2);
        EnableWindow(usagePlayMode,!schedule);if(usageWindow)EnableWindow(GetDlgItem(usageWindow,959),!schedule);}
    void enableUsageSchedule(){
        const bool enabled=SendMessageW(usageScheduleCheck,BM_GETCHECK,0,0)==BST_CHECKED;
        EnableWindow(usageScheduleScope,enabled);
        for(unsigned i=0;i<2;++i){EnableWindow(usageDayModes[i],enabled);const bool window=enabled&&SendMessageW(usageDayModes[i],CB_GETCURSEL,0,0)==1;
            EnableWindow(usageDayStarts[i],window);EnableWindow(usageDayEnds[i],window);}
        enableUsagePlay();
    }
    void fillUsageSchedule(const diagnostic::UsageSchedule& schedule){
        SendMessageW(usageScheduleCheck,BM_SETCHECK,schedule.enabled?BST_CHECKED:BST_UNCHECKED,0);
        SendMessageW(usageScheduleScope,CB_SETCURSEL,schedule.includeMail?1:0,0);
        for(unsigned i=0;i<2;++i){const auto& r=i?schedule.weekend:schedule.weekdays;
            SendMessageW(usageDayModes[i],CB_SETCURSEL,r.mode,0);
            SetWindowTextW(usageDayStarts[i],diagnostic::rankingWide(diagnostic::usageMinuteText(r.start)).c_str());
            SetWindowTextW(usageDayEnds[i],diagnostic::rankingWide(diagnostic::usageMinuteText(r.end)).c_str());}
        enableUsageSchedule();
    }
    static int usageMinute(HWND edit,bool end){const auto s=usageText(edit);
        if(s.size()!=5||s[2]!=':'||s.find_first_not_of("0123456789:")!=s.npos||s[0]==':'||s[1]==':'||s[3]==':'||s[4]==':')throw std::runtime_error("Enter JST time as HH:MM");
        const int hour=std::stoi(s.substr(0,2)),minute=std::stoi(s.substr(3,2));
        if(hour>24||minute>59||(hour==24&&(!end||minute!=0)))throw std::runtime_error("Time must be 00:00..23:59 (end may be 24:00)");
        return hour*60+minute;
    }
    void fillUsagePlay(const diagnostic::UsagePlayTime& play){
        const auto unlimited=diagnostic::rankingUTF8(L"\u6642\u9593\u5236\u9650\u306a\u3057");
        const int mode=!play.enabled?0:play.lines[0]==unlimited&&play.lines[1].empty()?1:2;
        SendMessageW(usagePlayMode,CB_SETCURSEL,mode,0);
        for(unsigned i=0;i<2;++i)SetWindowTextW(usagePlayEdits[i],diagnostic::rankingWide(play.lines[i]).c_str());
        enableUsagePlay();
    }
    std::string selectedUsagePhone(){const int n=GetWindowTextLengthW(usagePhone);std::wstring w(size_t(n)+1,0);GetWindowTextW(usagePhone,w.data(),n+1);w.resize(n);return diagnostic::rankingUTF8(w);}
    void showSavedUsagePolicy(bool justSaved=false){
        const auto phone=selectedUsagePhone();const auto schedule=usageSettings->schedule(phone);
        std::wstring text=justSaved?L"保存しました。 ":L"保存済み設定： ";
        text+=diagnostic::rankingWide(xband::phoneDigits(phone));
        if(schedule.enabled){
            text+=schedule.includeMail?L"　時間帯制限 有効（対戦・メール）":L"　時間帯制限 有効（対戦のみ）";
            const auto lines=diagnostic::usageScheduleDisplay(schedule).lines;
            text+=L"\n"+diagnostic::rankingWide(lines[0])+L" ／ "+diagnostic::rankingWide(lines[1]);
            text+=L"\n制限は保存直後から適用。接続済みの対戦は中断しません。";
        }else text+=L"　時間帯制限 無効\n表示を設定しても、接続時間の制限は行いません。";
        text+=L"\nゲームDISC(ROM)の表示は次の対戦接続後、使用状況を開き直すと更新されます。";
        SetWindowTextW(usageMessage,text.c_str());
    }
    static LRESULT CALLBACK usageProc(HWND w,UINT m,WPARAM a,LPARAM b){
        auto s=reinterpret_cast<XbandDashboard*>(GetWindowLongPtrW(w,GWLP_USERDATA));
        if(m==WM_NCCREATE){s=static_cast<XbandDashboard*>(reinterpret_cast<CREATESTRUCTW*>(b)->lpCreateParams);SetWindowLongPtrW(w,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(s));}
        if(!s)return DefWindowProcW(w,m,a,b);
        if(m==WM_COMMAND&&LOWORD(a)==952){try{
            const auto selected=SendMessageW(s->usageSelector,CB_GETCURSEL,0,0);
            if(selected<0||selected>3)throw std::runtime_error("Select an area");
            const auto mode=SendMessageW(s->usagePlayMode,CB_GETCURSEL,0,0);
            if(mode<0||mode>2)throw std::runtime_error("Select a play-time display mode");
            diagnostic::UsagePlayTime play;play.enabled=mode!=0;
            for(unsigned i=0;i<2;++i)play.lines[i]=usageText(s->usagePlayEdits[i]);
            if(mode==1)play.lines={diagnostic::rankingUTF8(L"\u6642\u9593\u5236\u9650\u306a\u3057"),""};
            diagnostic::UsageSchedule schedule;schedule.enabled=SendMessageW(s->usageScheduleCheck,BM_GETCHECK,0,0)==BST_CHECKED;
            const auto scope=SendMessageW(s->usageScheduleScope,CB_GETCURSEL,0,0);
            if(scope<0||scope>1)throw std::runtime_error("Select schedule scope");schedule.includeMail=scope==1;
            for(unsigned i=0;i<2;++i){auto& r=i?schedule.weekend:schedule.weekdays;
                r.mode=int(SendMessageW(s->usageDayModes[i],CB_GETCURSEL,0,0));r.start=usageMinute(s->usageDayStarts[i],false);r.end=usageMinute(s->usageDayEnds[i],true);}
            s->usageSettings->save(s->selectedUsagePhone(),int(selected)-1,play,schedule);
            s->showSavedUsagePolicy(true);
        }catch(const std::exception&e){SetWindowTextW(s->usageMessage,diagnostic::rankingWide(e.what()).c_str());}return 0;}
        if(m==WM_COMMAND&&LOWORD(a)==954&&(HIWORD(a)==CBN_SELCHANGE||HIWORD(a)==CBN_EDITCHANGE)){
            std::string phone;
            if(HIWORD(a)==CBN_SELCHANGE){const auto i=SendMessageW(s->usagePhone,CB_GETCURSEL,0,0);const auto n=SendMessageW(s->usagePhone,CB_GETLBTEXTLEN,i,0);if(n<0||n>32)return 0;std::wstring w(size_t(n)+1,0);SendMessageW(s->usagePhone,CB_GETLBTEXT,i,reinterpret_cast<LPARAM>(w.data()));w.resize(n);phone=diagnostic::rankingUTF8(w);}
            else phone=s->selectedUsagePhone();
            SendMessageW(s->usageSelector,CB_SETCURSEL,s->usageSettings->selection(phone)+1,0);
            s->fillUsagePlay(s->usageSettings->playTime(phone));s->fillUsageSchedule(s->usageSettings->schedule(phone));s->showSavedUsagePolicy();return 0;
        }
        if(m==WM_COMMAND&&LOWORD(a)==956&&HIWORD(a)==CBN_SELCHANGE){
            if(SendMessageW(s->usagePlayMode,CB_GETCURSEL,0,0)==1){SetWindowTextW(s->usagePlayEdits[0],L"時間制限なし");SetWindowTextW(s->usagePlayEdits[1],L"");}
            s->enableUsagePlay();return 0;
        }
        if(m==WM_COMMAND&&LOWORD(a)==959){diagnostic::UsagePlayTime play;play.enabled=true;s->fillUsagePlay(play);return 0;}
        if(m==WM_COMMAND&&(LOWORD(a)==960||((LOWORD(a)==962||LOWORD(a)==965)&&HIWORD(a)==CBN_SELCHANGE))){s->enableUsageSchedule();return 0;}
        if(m==WM_DESTROY){s->usageWindow=nullptr;return 0;}
        return DefWindowProcW(w,m,a,b);
    }
    void openUsage(HWND owner,bool show=true){
        if(!usageSettings)return;
        if(usageWindow){ShowWindow(usageWindow,SW_SHOW);SetForegroundWindow(usageWindow);return;}
        WNDCLASSW c{};c.lpfnWndProc=usageProc;c.hInstance=GetModuleHandleW(nullptr);c.lpszClassName=L"XbandUsageArea";c.hCursor=LoadCursor(nullptr,IDC_ARROW);c.hbrBackground=GetSysColorBrush(COLOR_BTNFACE);RegisterClassW(&c);
        usageWindow=CreateWindowW(c.lpszClassName,L"XBAND | 使用状況と利用時間の設定",WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU,CW_USEDEFAULT,CW_USEDEFAULT,640,806,owner,nullptr,c.hInstance,this);
        if(!usageWindow)return;
        auto control=[&](const wchar_t*cls,const wchar_t*title,DWORD style,int x,int y,int width,int height,int id){
            auto h=CreateWindowW(cls,title,WS_CHILD|WS_VISIBLE|style,x,y,width,height,usageWindow,reinterpret_cast<HMENU>(INT_PTR(id)),c.hInstance,nullptr);
            SendMessageW(h,WM_SETFONT,reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)),TRUE);return h;
        };
        control(L"STATIC",L"端末の登録電話番号（設定の検索キー・認証済みIDではありません）",0,18,18,580,24,0);
        usagePhone=control(L"COMBOBOX",L"",CBS_DROPDOWN|WS_VSCROLL|WS_TABSTOP,18,48,560,180,954);
        SendMessageW(usagePhone,CB_LIMITTEXT,32,0);
        for(const auto& [phone,area]:usageSettings->snapshot()){auto w=diagnostic::rankingWide(phone);SendMessageW(usagePhone,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(w.c_str()));}
        control(L"STATIC",L"対戦エリア設定（地域による相手の絞り込みは未実装）",0,18,86,560,24,0);
        usageSelector=control(L"COMBOBOX",L"",CBS_DROPDOWNLIST|WS_TABSTOP,18,112,360,160,951);
        for(const auto* label:{L"上書きしない（保存済みの設定は戻りません）",L"同一局番内に固定（操作不可）",L"全国に固定（操作不可）",L"ゲームDISC(ROM)で任意選択（同一局番内／全国）"})SendMessageW(usageSelector,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(label));
        SendMessageW(usageSelector,CB_SETCURSEL,0,0);
        control(L"STATIC",L"プレー時間の表示（文言だけでは接続を制限しません）",0,18,160,560,24,0);
        usagePlayMode=control(L"COMBOBOX",L"",CBS_DROPDOWNLIST|WS_TABSTOP,18,188,360,160,956);
        for(const auto* label:{L"上書きしない",L"時間制限なし（1行）",L"サーバー指定（2行・任意の文言）"})SendMessageW(usagePlayMode,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(label));
        control(L"BUTTON",L"動画と同じ表示",WS_TABSTOP,398,188,180,30,959);
        const wchar_t* timeLabels[]{L"上段（例：月－金 利用可能）",L"下段（例：土－日 利用可能）"};
        for(unsigned i=0;i<2;++i){const int y=236+int(i)*66;
            control(L"STATIC",timeLabels[i],0,18,y,560,24,0);
            usagePlayEdits[i]=control(L"EDIT",L"",WS_BORDER|ES_AUTOHSCROLL|WS_TABSTOP,18,y+26,560,28,957+i);
            SendMessageW(usagePlayEdits[i],EM_SETLIMITTEXT,30,0);
        }
        usageScheduleCheck=control(L"BUTTON",L"時間帯制限を有効にする",BS_AUTOCHECKBOX|WS_TABSTOP,18,374,180,26,960);
        usageScheduleScope=control(L"COMBOBOX",L"",CBS_DROPDOWNLIST|WS_TABSTOP,208,372,370,100,961);
        for(const auto* label:{L"対戦接続のみ制限（メール利用可能）",L"対戦とメールの接続を制限"})SendMessageW(usageScheduleScope,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(label));
        for(unsigned i=0;i<2;++i){const int y=422+int(i)*58;
            control(L"STATIC",i?L"土・日 (JST)":L"月〜金 (JST)",0,18,y+4,96,24,0);
            usageDayModes[i]=control(L"COMBOBOX",L"",CBS_DROPDOWNLIST|WS_TABSTOP,118,y,150,110,962+i*3);
            for(const auto* label:{L"終日利用可能",L"時間帯を指定",L"利用不可"})SendMessageW(usageDayModes[i],CB_ADDSTRING,0,reinterpret_cast<LPARAM>(label));
            usageDayStarts[i]=control(L"COMBOBOX",L"",CBS_DROPDOWN|WS_VSCROLL|WS_TABSTOP,286,y,112,180,963+i*3);
            usageDayEnds[i]=control(L"COMBOBOX",L"",CBS_DROPDOWN|WS_VSCROLL|WS_TABSTOP,444,y,112,180,964+i*3);
            control(L"STATIC",L"〜",0,412,y+4,24,24,0);
            SendMessageW(usageDayStarts[i],CB_LIMITTEXT,5,0);SendMessageW(usageDayEnds[i],CB_LIMITTEXT,5,0);
            for(int minute=0;minute<=1440;minute+=30){const auto text=diagnostic::rankingWide(diagnostic::usageMinuteText(minute));
                if(minute<1440)SendMessageW(usageDayStarts[i],CB_ADDSTRING,0,reinterpret_cast<LPARAM>(text.c_str()));
                SendMessageW(usageDayEnds[i],CB_ADDSTRING,0,reinterpret_cast<LPARAM>(text.c_str()));}
        }
        control(L"STATIC",L"時刻は日本時間。同じ端末の4ユーザー・全ゲームに共通で適用。\n手入力で1分単位。終了時刻は含まず、開始＞終了は翌日まで有効。\n制限中の表示は曜日別設定から自動生成。接続済みの対戦は中断しません。\n上のチェックを入れ、曜日別の利用条件を設定して保存してください。",0,18,542,580,74,0);
        control(L"BUTTON",L"保存",WS_TABSTOP|BS_DEFPUSHBUTTON,398,630,180,32,952);
        usageMessage=control(L"STATIC",L"番号を選び、設定して保存してください。\n表示の更新と時間帯制限は別の機能です。",0,18,674,580,82,953);
        fillUsagePlay(diagnostic::UsagePlayTime{});
        fillUsageSchedule(diagnostic::UsageSchedule{});
        if(show)ShowWindow(usageWindow,SW_SHOW);
    }
    void fillRanking(bool defaults=false){
        const auto index=SendMessageW(rankingSelector,CB_GETCURSEL,0,0);
        if(index<0||size_t(index)>=rankingRows.size())return;
        auto row=rankingRows[size_t(index)];
        if(defaults){row.winPoints=diagnostic::newGameDefaultWinPoints;row.losePoints=diagnostic::newGameDefaultLosePoints;for(const auto&r:diagnostic::rankingDefaults())if(r.gameID==row.gameID)row=r;}
        SetWindowTextW(rankingEdits[0],diagnostic::rankingWide(row.fields[0]).c_str());
        for(unsigned i=1;i<5;++i){SetWindowTextW(rankingEdits[i],i==2?L"サーバー累積値（初期値0）":L"昇格条件から自動計算");EnableWindow(rankingEdits[i],FALSE);}
        SendMessageW(rankingAward,CB_SETCURSEL,row.winPoints<0?0:1,0);
        SetWindowTextW(rankingAwardValue,std::to_wstring(row.winPoints<0?1:row.winPoints).c_str());
        EnableWindow(rankingAwardValue,row.winPoints>=0);
        SetWindowTextW(rankingLoseValue,std::to_wstring(row.losePoints).c_str());EnableWindow(rankingLoseValue,row.winPoints>=0);
        SetWindowTextW(rankingMessage,L"勝者・敗者それぞれ0～999ポイント。ゲーム別、全番号・4ユーザー共通。\n結果報告後に集計し、次の接続で累積をゲームDISC(ROM)へ配信。引き分けは0。\n対戦前のポイント表示は勝者の値。当時の配点を再現する設定ではありません。\n設定変更は次の対戦から。上書きなしでは両者ともサーバー加算なし。");
    }
    void saveRanking(){try{
        const auto index=SendMessageW(rankingSelector,CB_GETCURSEL,0,0);
        if(index<0||size_t(index)>=rankingRows.size())return;
        auto fields=rankingRows[size_t(index)].fields;
        {int n=GetWindowTextLengthW(rankingEdits[0]);std::wstring w(size_t(n)+1,0);GetWindowTextW(rankingEdits[0],w.data(),n+1);w.resize(n);fields[0]=diagnostic::rankingUTF8(w);}
        const auto award=SendMessageW(rankingAward,CB_GETCURSEL,0,0);
        if(award<0||award>1)throw std::runtime_error("Choose an award policy");
        auto readPoints=[](HWND edit){wchar_t value[32]{};GetWindowTextW(edit,value,32);std::wstring v(value);
            if(v.empty()||v.size()>3||v.find_first_not_of(L"0123456789")!=v.npos)throw std::runtime_error("Winner/loser award must be decimal 0..999");return std::stoi(v);};
        const int points=award==1?readPoints(rankingAwardValue):-1;
        const int losing=readPoints(rankingLoseValue);
        rankingSettings->update(rankingRows[size_t(index)].gameID,fields,points,losing);
        rankingRows=rankingSettings->snapshot();
        SendMessageW(rankingSelector,CB_RESETCONTENT,0,0);
        for(const auto& r:rankingRows){wchar_t value[24];std::swprintf(value,24,L"0x%08X  ",unsigned(r.gameID));auto name=std::wstring(value)+diagnostic::rankingWide(r.fields[0]);SendMessageW(rankingSelector,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(name.c_str()));}
        SendMessageW(rankingSelector,CB_SETCURSEL,index,0);
        SetWindowTextW(rankingMessage,L"保存しました。次の対戦から新しい勝者・敗者の付与値を使用します。\n既存の累積ポイントは変更しません。結果報告後の接続で累積を反映します。");
    }catch(const std::exception&e){SetWindowTextW(rankingMessage,diagnostic::rankingWide(e.what()).c_str());}}
    void addRanking(){try{
        wchar_t idText[32]{},titleText[129]{};
        GetWindowTextW(GetDlgItem(rankingWindow,915),idText,32);
        GetWindowTextW(GetDlgItem(rankingWindow,916),titleText,129);
        std::wstring id(idText);if(id.starts_with(L"0x")||id.starts_with(L"0X"))id.erase(0,2);
        if(id.empty()||id.size()>8||id.find_first_not_of(L"0123456789abcdefABCDEF")!=id.npos)throw std::runtime_error("Enter game ID as 1..8 hexadecimal digits");
        const auto game=uint32_t(std::stoull(id,nullptr,16));
        rankingSettings->add(game,diagnostic::rankingUTF8(titleText));
        rankingRows=rankingSettings->snapshot();SendMessageW(rankingSelector,CB_RESETCONTENT,0,0);
        for(const auto& r:rankingRows){wchar_t value[24];std::swprintf(value,24,L"0x%08X  ",unsigned(r.gameID));auto name=std::wstring(value)+diagnostic::rankingWide(r.fields[0]);SendMessageW(rankingSelector,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(name.c_str()));}
        SendMessageW(rankingSelector,CB_SETCURSEL,rankingRows.size()-1,0);fillRanking();
        // Refresh a previously open rank editor so the new game is selectable.
        if(levelWindow.handle())DestroyWindow(levelWindow.handle());
        SetWindowTextW(rankingMessage,L"ゲームを保存しました。サーバー標準配点（勝者1・敗者0）を適用します。\n初期ランクはLEVEL・200刻み。配点と昇格条件は編集可能です。既存累積は変更しません。");
    }catch(const std::exception&e){SetWindowTextW(rankingMessage,diagnostic::rankingWide(e.what()).c_str());}}
    static LRESULT CALLBACK rankingProc(HWND w,UINT m,WPARAM a,LPARAM b){
        auto s=reinterpret_cast<XbandDashboard*>(GetWindowLongPtrW(w,GWLP_USERDATA));
        if(m==WM_NCCREATE){s=static_cast<XbandDashboard*>(reinterpret_cast<CREATESTRUCTW*>(b)->lpCreateParams);SetWindowLongPtrW(w,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(s));}
        if(!s)return DefWindowProcW(w,m,a,b);
        if(m==WM_COMMAND){
            if(LOWORD(a)==901&&HIWORD(a)==CBN_SELCHANGE)s->fillRanking();
            if(LOWORD(a)==911&&HIWORD(a)==CBN_SELCHANGE){const bool enabled=SendMessageW(s->rankingAward,CB_GETCURSEL,0,0)==1;EnableWindow(s->rankingAwardValue,enabled);EnableWindow(s->rankingLoseValue,enabled);}
            if(LOWORD(a)==907)s->saveRanking();
            if(LOWORD(a)==917)s->addRanking();
            if(LOWORD(a)==908)s->fillRanking(true);
            if(LOWORD(a)==909)DestroyWindow(w);
            if(LOWORD(a)==913)s->levelWindow.open(w,diagnostic::activeGameLevels,s->rankingSettings);
            return 0;
        }
        if(m==WM_DESTROY){s->rankingWindow=nullptr;return 0;}
        return DefWindowProcW(w,m,a,b);
    }
    void openRankings(HWND owner,bool show=true){
        {std::lock_guard lock(mutex);if(!rankingSettings)return;}
        if(rankingWindow){ShowWindow(rankingWindow,SW_SHOW);SetForegroundWindow(rankingWindow);return;}
        rankingRows=rankingSettings->snapshot();
        WNDCLASSW c{};c.lpfnWndProc=rankingProc;c.hInstance=GetModuleHandleW(nullptr);c.lpszClassName=L"XbandGameRankings";c.hCursor=LoadCursor(nullptr,IDC_ARROW);c.hbrBackground=GetSysColorBrush(COLOR_BTNFACE);RegisterClassW(&c);
        rankingWindow=CreateWindowW(c.lpszClassName,L"XBAND | ゲーム・勝者／敗者ポイント設定",WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU,CW_USEDEFAULT,CW_USEDEFAULT,690,720,owner,nullptr,c.hInstance,this);
        if(!rankingWindow)return;
        auto control=[&](const wchar_t*cls,const wchar_t*title,DWORD style,int x,int y,int width,int height,int id){
            HWND h=CreateWindowW(cls,title,WS_CHILD|WS_VISIBLE|style,x,y,width,height,rankingWindow,reinterpret_cast<HMENU>(INT_PTR(id)),c.hInstance,nullptr);
            SendMessageW(h,WM_SETFONT,reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)),TRUE);return h;
        };
        control(L"STATIC",L"ゲーム／固定ID",0,18,22,142,24,0);
        rankingSelector=control(L"COMBOBOX",L"",CBS_DROPDOWNLIST|WS_VSCROLL|WS_TABSTOP,170,18,475,220,901);
        for(const auto &r:rankingRows){wchar_t id[24];std::swprintf(id,24,L"0x%08X  ",unsigned(r.gameID));auto name=std::wstring(id)+diagnostic::rankingWide(r.fields[0]);SendMessageW(rankingSelector,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(name.c_str()));}
        const wchar_t*labels[]={L"ゲーム名",L"現在のランク",L"累積ポイント",L"次のランク",L"昇格までの必要ポイント"};
        for(unsigned i=0;i<5;++i){int y=64+int(i)*42;control(L"STATIC",labels[i],0,18,y+4,145,26,0);rankingEdits[i]=control(L"EDIT",L"",WS_BORDER|ES_AUTOHSCROLL|WS_TABSTOP,170,y,475,29,902+i);SendMessageW(rankingEdits[i],EM_SETLIMITTEXT,128,0);}
        control(L"STATIC",L"ポイント加算方法",0,18,278,145,26,0);
        rankingAward=control(L"COMBOBOX",L"",CBS_DROPDOWNLIST|WS_TABSTOP,170,274,280,150,911);
        for(const auto* label:{L"サーバー加算なし",L"サーバー設定の配点を使用"})
            SendMessageW(rankingAward,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(label));
        control(L"STATIC",L"勝者ポイント",0,18,320,145,26,0);
        rankingAwardValue=control(L"EDIT",L"1",WS_BORDER|ES_NUMBER|WS_TABSTOP,170,316,100,29,912);
        SendMessageW(rankingAwardValue,EM_SETLIMITTEXT,3,0);
        control(L"STATIC",L"敗者ポイント",0,330,320,125,26,0);
        rankingLoseValue=control(L"EDIT",L"0",WS_BORDER|ES_NUMBER|WS_TABSTOP,465,316,100,29,914);
        SendMessageW(rankingLoseValue,EM_SETLIMITTEXT,3,0);
        control(L"BUTTON",L"保存",WS_TABSTOP|BS_DEFPUSHBUTTON,170,376,115,32,907);
        control(L"BUTTON",L"昇格条件...",WS_TABSTOP,18,376,132,32,913);
        control(L"BUTTON",L"名前・配点の初期値",WS_TABSTOP,300,376,160,32,908);
        control(L"BUTTON",L"閉じる",WS_TABSTOP,475,376,115,32,909);
        rankingMessage=control(L"STATIC",L"",0,18,422,630,98,910);
        control(L"STATIC",L"新規ゲームID (HEX)",0,18,534,145,26,0);
        auto idEdit=control(L"EDIT",L"0x",WS_BORDER|ES_AUTOHSCROLL|WS_TABSTOP,170,530,200,29,915);SendMessageW(idEdit,EM_SETLIMITTEXT,10,0);
        control(L"STATIC",L"新規ゲーム名",0,18,576,145,26,0);
        auto titleEdit=control(L"EDIT",L"",WS_BORDER|ES_AUTOHSCROLL|WS_TABSTOP,170,572,475,29,916);SendMessageW(titleEdit,EM_SETLIMITTEXT,128,0);
        control(L"BUTTON",L"ゲームを追加",WS_TABSTOP,475,614,170,32,917);
        control(L"STATIC",L"実要求で確認したIDを指定。重複不可・最大63ゲーム。\n初期はサーバー標準配点（勝者1・敗者0）。編集可能。",0,18,614,445,46,0);
        SendMessageW(rankingSelector,CB_SETCURSEL,0,0);fillRanking();if(show)ShowWindow(rankingWindow,SW_SHOW);
    }
    static std::wstring wide(const std::string&s){return {s.begin(),s.end()};}
    static void rect(HDC d,int x,int y,int w,int h,COLORREF color){RECT r{x,y,x+w,y+h};auto b=CreateSolidBrush(color);FillRect(d,&r,b);DeleteObject(b);}
    static void text(HDC d,int x,int y,int w,const std::wstring&s,int size=18,COLORREF color=RGB(211,223,237)){
        auto f=CreateFontW(-size,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
        auto old=SelectObject(d,f);SetBkMode(d,TRANSPARENT);SetTextColor(d,color);RECT r{x,y,x+w,y+64};DrawTextW(d,s.c_str(),-1,&r,DT_LEFT|DT_WORDBREAK|DT_NOPREFIX);SelectObject(d,old);DeleteObject(f);
    }
    void draw(HDC d){
        rect(d,0,0,1080,800,RGB(13,20,33));
        text(d,30,8,1000,L"XBAND  /  COMMUNICATION MONITOR",28);
        text(d,30,45,1000,L"LOCAL TCP   127.0.0.1   |   Live counters / read-only   |   500 ms updates",16,RGB(132,154,180));
        if(data.empty()){text(d,30,130,950,L"Waiting for server state...");return;}
        const auto progress=data.value("diagnostic_test_progress",std::string{});
        if(!progress.empty())text(d,30,724,1020,wide(progress),14,RGB(255,207,125));
        const auto &p=data.at("pair_control");const unsigned state=p.at("state");const bool failed=p.at("failed");
        const unsigned caller=p.value("caller",2u),callee=caller<2?1-caller:2;
        const auto requested=p.value("requested",std::array<bool,2>{});
        for(int i=0;i<2;++i){
            int x=i?740:30;rect(d,x,120,310,350,RGB(25,38,57));
            const auto &e=data.at("endpoints")[i];const auto &o=e.at("pb3_observation");
            bool connected=e.at("connected");bool joined=p.at("joined")[i];
            text(d,x+18,137,270,L"MODEM "+std::to_wstring(i+1),24);
            text(d,x+18,175,270,connected?L"SERVER ONLINE":L"SERVER OFFLINE",18,connected?RGB(63,219,169):RGB(151,165,185));
            text(d,x+18,205,270,L"Phone: "+wide(diagnostic::modemSubscriberDisplay(o)),16);
            text(d,x+18,250,270,failed?L"Peer session disconnected":joined?(state==2?L"Peer call connected":state==1?(unsigned(i)==caller?L"Calling modem "+std::to_wstring(callee+1):L"Incoming from modem "+std::to_wstring(caller+1)):L"Waiting for call"):L"Peer endpoint offline",17);
            text(d,x+18,299,270,L"Service: UP "+wide(o.at("received").get<std::string>())+L" B / DOWN "+wide(o.at("sent").get<std::string>())+L" B",15);
            text(d,x+18,323,270,wide(xband::monitor::mailSavedLine(o)),13);
            text(d,x+18,347,270,wide(xband::monitor::mailIncomingLine(o)),13);
            text(d,x+18,371,270,wide(xband::monitor::mailHeldLine(o)),13);
            text(d,x+18,395,270,wide(xband::monitor::mailClearLine(o)),13);
            text(d,x+18,420,270,L"Last service call / not delivery proof",12,RGB(132,154,180));
            text(d,x+18,444,270,L"Card: unknown (not reported)",13,RGB(132,154,180));
        }
        rect(d,370,120,340,350,RGB(24,43,61));text(d,391,137,295,L"XBAND SERVER",24);
        text(d,391,185,295,failed?L"SESSION CLOSED":state==2?L"PEER RELAY ACTIVE":state==1?L"INCOMING CALL":caller==2&&(requested[0]||requested[1])?L"WAITING FOR OTHER GAME":L"READY / WAITING",20,failed?RGB(255,133,133):RGB(63,219,169));
        text(d,391,231,295,L"Transport: "+wide(p.at("transport").get<std::string>()),16);
        text(d,391,269,295,wide(diagnostic::modemRouteDisplay(data.at("endpoints"),caller)),16);
        const auto capture=data.value("mail_capture_store",J::object());
        text(d,391,341,295,capture.value("enabled",false)?
            L"Mail: "+std::to_wstring(capture.value("records",size_t{}))+L" retained / "+std::to_wstring(capture.value("bytes",size_t{}))+L" B":
            L"Generation "+std::to_wstring(p.at("generation").get<uint64_t>()),14);
        text(d,391,366,295,wide(xband::monitor::mailStorageLine(capture)),12,RGB(132,154,180));
        if(capture.value("enabled",false))text(d,391,395,295,L"Saved != delivered\nNo delivery / read receipt",13,RGB(255,207,125));
        for(int i=0;i<2;++i){int y=490+i*61;bool active=rate[i]>0&&GetTickCount64()-last<2000;
            rect(d,30,y,1020,51,RGB(25,38,57));rect(d,43,y+18,12,12,active?RGB(63,219,169):RGB(82,97,116));
            auto sent=p.at("sent")[i].get<uint64_t>();auto received=p.at("received")[1-i].get<uint64_t>();
            text(d,70,y+12,960,(i?L"2 -> 1":L"1 -> 2")+std::wstring(active?L"   DATA  ":L"   IDLE  ")+std::to_wstring(rate[i])+L" B/s    accepted "+std::to_wstring(sent)+L" B    delivered "+std::to_wstring(received)+L" B    queued "+std::to_wstring(sent>=received?sent-received:0)+L" B",17);
        }
        text(d,30,616,1020,L"Recent game bytes: HEX + ASCII text (binary = \\xHH; not protocol decoding)",14,RGB(132,154,180));
        for(int i=0;i<2;++i){
            const auto bytes=data.value(i?"hex1":"hex0",std::string("--"));
            const std::wstring direction=i?L"2 -> 1  ":L"1 -> 2  ";
            text(d,30,638+i*43,1020,direction+L"HEX   "+wide(bytes),15);
            text(d,30,657+i*43,1020,direction+L"TEXT  "+diagnostic::peerByteText(bytes),15,RGB(113,229,190));
        }
        text(d,30,746,1020,events.empty()?L"Waiting for state changes":events.back(),14,RGB(132,154,180));
        text(d,30,770,1020,L"Mail counters describe the last reply preparation. Outbox clear is not a guest completion acknowledgement.",12,RGB(132,154,180));
        const auto journal=data.value("local_mail_journal",J::object());
        if(journal.value("enabled",false))text(d,30,789,1020,L"Persistent mail: "+std::to_wstring(journal.value("mails",size_t{0}))+L" submitted / "+std::to_wstring(journal.value("prepared_unconfirmed",size_t{0}))+L" prepared (unconfirmed). No server retry.",11,RGB(132,154,180));
    }
    static void layoutButtons(HWND w){
        RECT r{};GetClientRect(w,&r);const int ids[]={979,978,950,975,900,976};
        const int count=GetDlgItem(w,976)?6:5,step=count==6?170:206,width=count==6?160:194;
        for(int i=0;i<count;++i)if(auto h=GetDlgItem(w,ids[i])){
            const int x=(30+i*step)*r.right/1080,y=76*r.bottom/800;
            const int buttonWidth=width*r.right/1080,buttonHeight=32*r.bottom/800;
            RECT previous{};GetWindowRect(h,&previous);MapWindowPoints(nullptr,w,reinterpret_cast<POINT*>(&previous),2);
            if(previous.left!=x||previous.top!=y||previous.right-previous.left!=buttonWidth||previous.bottom-previous.top!=buttonHeight)
                MoveWindow(h,x,y,buttonWidth,buttonHeight,TRUE);
            const auto font=GetStockObject(DEFAULT_GUI_FONT);
            if(SendMessageW(h,WM_GETFONT,0,0)!=reinterpret_cast<LRESULT>(font))
                SendMessageW(h,WM_SETFONT,reinterpret_cast<WPARAM>(font),TRUE);
        }
    }
    static LRESULT CALLBACK proc(HWND w,UINT m,WPARAM a,LPARAM b){
        auto s=reinterpret_cast<XbandDashboard*>(GetWindowLongPtrW(w,GWLP_USERDATA));
        if(m==WM_NCCREATE){s=static_cast<XbandDashboard*>(reinterpret_cast<CREATESTRUCTW*>(b)->lpCreateParams);SetWindowLongPtrW(w,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(s));}
        if(!s)return DefWindowProcW(w,m,a,b);
        if(m==WM_SIZE){layoutButtons(w);return 0;}
        if(m>=WM_APP+41&&m<=WM_APP+46){
            const int ids[]={900,950,979,978,975,976};const wchar_t* labels[]={L"ゲーム・ポイント設定...",L"使用状況の表示設定...",L"メール履歴...",L"接続・対戦・ポイント履歴...",L"対戦待ち時間設定...",L"消費度数設定..."};
            const auto i=m-(WM_APP+41);
            if(!GetDlgItem(w,ids[i]))CreateWindowW(L"BUTTON",labels[i],WS_CHILD|WS_VISIBLE|WS_TABSTOP,0,0,0,0,w,reinterpret_cast<HMENU>(INT_PTR(ids[i])),GetModuleHandleW(nullptr),nullptr);
            layoutButtons(w);return 0;
        }
        if(m==WM_TIMER){InvalidateRect(w,nullptr,FALSE);return 0;}
        if(m==WM_COMMAND&&LOWORD(a)==979){s->openMail(w);return 0;}
        if(m==WM_COMMAND&&LOWORD(a)==978){s->activityWindow.open(w);return 0;}
        if(m==WM_COMMAND&&LOWORD(a)==900){s->openRankings(w);return 0;}
        if(m==WM_COMMAND&&LOWORD(a)==950){s->openUsage(w);return 0;}
        if(m==WM_COMMAND&&LOWORD(a)==975){s->openWait(w);return 0;}
        if(m==WM_COMMAND&&LOWORD(a)==976){s->creditWindow.open(w);return 0;}
        if(m==WM_ERASEBKGND)return 1;
        if(m==WM_PAINT){PAINTSTRUCT ps;auto dc=BeginPaint(w,&ps);auto mem=CreateCompatibleDC(dc);auto bmp=CreateCompatibleBitmap(dc,1080,800);auto old=SelectObject(mem,bmp);
            {std::lock_guard lock(s->mutex);s->draw(mem);}RECT r;GetClientRect(w,&r);SetStretchBltMode(dc,HALFTONE);StretchBlt(dc,0,0,r.right,r.bottom,mem,0,0,1080,800,SRCCOPY);SelectObject(mem,old);DeleteObject(bmp);DeleteDC(mem);EndPaint(w,&ps);return 0;}
        if(m==WM_DESTROY){s->closed=true;s->hwnd=nullptr;PostQuitMessage(0);return 0;}return DefWindowProcW(w,m,a,b);
    }
public:
    void setServiceCreditSettings(std::shared_ptr<diagnostic::ServiceCreditSettings> value){
        {std::lock_guard lock(mutex);creditWindow.setSettings(std::move(value));}
        if(auto w=hwnd.load())PostMessageW(w,WM_APP+46,0,0);
    }
    void setStandbyWaitSettings(std::shared_ptr<diagnostic::StandbyWaitSettings> value){
        {std::lock_guard lock(mutex);waitSettings=std::move(value);}
        if(auto w=hwnd.load())PostMessageW(w,WM_APP+45,0,0);
    }
    void setActivityHistory(std::shared_ptr<diagnostic::ActivityHistory> value){activityWindow.setHistory(value);if(value)if(auto w=hwnd.load())PostMessageW(w,WM_APP+44,0,0);}
    void setGameResultHistory(std::shared_ptr<diagnostic::ActivityHistory> value){resultWindow.setResultHistory(std::move(value));activityWindow.setResultOpener([this]{resultWindow.open(activityWindow.handle());});}
    void setLocalMailJournal(std::shared_ptr<diagnostic::LocalMailJournal> value){
        {std::lock_guard lock(mutex);mailJournal=std::move(value);}
        broadcastWindow.setJournal(mailJournal);
        if(mailJournal)if(auto w=hwnd.load())PostMessageW(w,WM_APP+43,0,0);
    }
    void setUsageAreaSettings(std::shared_ptr<diagnostic::UsageAreaSettings> value){
        {std::lock_guard lock(mutex);usageSettings=std::move(value);}
        if(auto w=hwnd.load())PostMessageW(w,WM_APP+42,0,0);
    }
    void setRankingSettings(std::shared_ptr<diagnostic::GameRankingSettings> value){
        {std::lock_guard lock(mutex);rankingSettings=std::move(value);}
        if(auto w=hwnd.load())PostMessageW(w,WM_APP+41,0,0);
    }
#ifdef XBAND_DASHBOARD_RENDER_TEST
    struct SnapshotOnly {};
    explicit XbandDashboard(SnapshotOnly){} // No window/thread/network in rendering test.
    static void renderBroadcastEditor(const std::filesystem::path& path){
        if(std::filesystem::exists(path))throw std::runtime_error("Preserve existing preview");
        const auto root=std::filesystem::temp_directory_path()/("xband-center-ui-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64()));
        auto journal=std::make_shared<diagnostic::LocalMailJournal>(root);
        journal->publishBroadcast(L"サーバーの不調について",L"接続を確認しています。\n復旧後にお知らせします。");
        diagnostic::ServerBroadcastWindow editor;editor.setJournal(journal);editor.open(nullptr,false);const auto w=editor.handle();
        if(!w||SendMessageW(GetDlgItem(w,1109),LVM_GETITEMCOUNT,0,0)!=1||IsWindowEnabled(GetDlgItem(w,1102)))throw std::runtime_error("Broadcast controls missing");
        SendMessageW(GetDlgItem(w,1101),CB_SETCURSEL,1,0);SendMessageW(w,WM_COMMAND,MAKEWPARAM(1101,CBN_SELCHANGE),0);
        if(!IsWindowEnabled(GetDlgItem(w,1102))||SendMessageW(GetDlgItem(w,1103),CB_GETCOUNT,0,0)!=5)throw std::runtime_error("Broadcast target choices");
        SetWindowTextW(GetDlgItem(w,1102),L"059-666");SendMessageW(GetDlgItem(w,1103),CB_SETCURSEL,3,0);
        // Click the actual sample command with empty edits: no confirmation or publication.
        SendMessageW(w,WM_COMMAND,MAKEWPARAM(1110,BN_CLICKED),0);
        auto editText=[](HWND h){const auto n=GetWindowTextLengthW(h);std::wstring text(n+1,L'\0');GetWindowTextW(h,text.data(),n+1);text.resize(n);return text;};
        if(editText(GetDlgItem(w,1107))!=diagnostic::broadcastSampleSubject||editText(GetDlgItem(w,1108))!=diagnostic::broadcastSampleText||journal->broadcastHistory().size()!=1||SendMessageW(GetDlgItem(w,1101),CB_GETCURSEL,0,0)!=1||SendMessageW(GetDlgItem(w,1103),CB_GETCURSEL,0,0)!=3)
            throw std::runtime_error("Sample must fill exact text without publishing or changing recipients");
        LVITEMW selected{};selected.stateMask=LVIS_SELECTED|LVIS_FOCUSED;selected.state=selected.stateMask;SendMessageW(GetDlgItem(w,1109),LVM_SETITEMSTATE,0,reinterpret_cast<LPARAM>(&selected));
        RECT r{};GetWindowRect(w,&r);SetWindowPos(w,nullptr,-30000,-30000,0,0,SWP_NOSIZE|SWP_NOACTIVATE|SWP_NOZORDER);ShowWindow(w,SW_SHOWNOACTIVATE);UpdateWindow(w);
        ULONG_PTR token{};Gdiplus::GdiplusStartupInput input;if(Gdiplus::GdiplusStartup(&token,&input,nullptr)!=Gdiplus::Ok)throw std::runtime_error("GDI+ failed");
        {Gdiplus::Bitmap bitmap(r.right-r.left,r.bottom-r.top,PixelFormat32bppRGB);Gdiplus::Graphics graphics(&bitmap);auto dc=graphics.GetHDC();
            if(!PrintWindow(w,dc,0))throw std::runtime_error("Broadcast print failed");GdiFlush();graphics.ReleaseHDC(dc);
            UINT count{},bytes{};Gdiplus::GetImageEncodersSize(&count,&bytes);std::vector<uint8_t> buffer(bytes);auto codecs=reinterpret_cast<Gdiplus::ImageCodecInfo*>(buffer.data());Gdiplus::GetImageEncoders(count,bytes,codecs);
            bool saved=false;for(UINT i=0;i<count;++i)if(std::wcscmp(codecs[i].MimeType,L"image/png")==0){saved=bitmap.Save(path.c_str(),&codecs[i].Clsid,nullptr)==Gdiplus::Ok;break;}if(!saved)throw std::runtime_error("Broadcast PNG failed");}
        DestroyWindow(w);Gdiplus::GdiplusShutdown(token);
    }
    static void renderWaitEditor(const std::filesystem::path& path){
        if(std::filesystem::exists(path))throw std::runtime_error("Preserve existing preview");
        XbandDashboard fixture(SnapshotOnly{});
        const auto file=std::filesystem::temp_directory_path()/("standby-wait-ui-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64()))/"settings.json";
        fixture.waitSettings=std::make_shared<diagnostic::StandbyWaitSettings>(file);fixture.openWait(nullptr,false);
        if(!fixture.waitWindow)throw std::runtime_error("Wait editor creation failed");
        for(unsigned i=0;i<3;++i){if(SendMessageW(fixture.waitSelectors[i],CB_GETCOUNT,0,0)!=60)throw std::runtime_error("Wait choices missing");SendMessageW(fixture.waitSelectors[i],CB_SETCURSEL,2+i*2,0);}
        waitProc(fixture.waitWindow,WM_COMMAND,973,0);
        const diagnostic::StandbyWaitValues expected{{3,5,7}};
        if(diagnostic::StandbyWaitSettings(file).snapshot()!=expected)throw std::runtime_error("Wait UI Save failed");
        SendMessageW(fixture.waitSelectors[0],CB_SETCURSEL,59,0);waitProc(fixture.waitWindow,WM_COMMAND,973,0);
        if(fixture.waitSettings->snapshot()!=expected||diagnostic::StandbyWaitSettings(file).snapshot()!=expected)throw std::runtime_error("Invalid wait order changed saved settings");
        waitProc(fixture.waitWindow,WM_COMMAND,974,0);
        if(fixture.waitSettings->snapshot()!=expected)throw std::runtime_error("Defaults changed settings before Save");
        DestroyWindow(fixture.waitWindow);fixture.openWait(nullptr,false);
        for(unsigned i=0;i<3;++i)if(SendMessageW(fixture.waitSelectors[i],CB_GETCURSEL,0,0)!=expected.minutes[i]-1)throw std::runtime_error("Wait UI reopen did not restore settings");
        RECT client{};GetClientRect(fixture.waitWindow,&client);
        for(auto h:{fixture.waitSelectors[0],fixture.waitSelectors[1],fixture.waitSelectors[2],fixture.waitMessage,GetDlgItem(fixture.waitWindow,973)}){
            RECT r{};GetWindowRect(h,&r);MapWindowPoints(HWND_DESKTOP,fixture.waitWindow,reinterpret_cast<POINT*>(&r),2);
            if(r.left<0||r.top<0||r.right>client.right||r.bottom>client.bottom)throw std::runtime_error("Wait UI control clipped");
        }
        RECT r{};GetWindowRect(fixture.waitWindow,&r);SetWindowPos(fixture.waitWindow,nullptr,-30000,-30000,0,0,SWP_NOSIZE|SWP_NOACTIVATE|SWP_NOZORDER);ShowWindow(fixture.waitWindow,SW_SHOWNOACTIVATE);UpdateWindow(fixture.waitWindow);
        ULONG_PTR token{};Gdiplus::GdiplusStartupInput input;if(Gdiplus::GdiplusStartup(&token,&input,nullptr)!=Gdiplus::Ok)throw std::runtime_error("GDI+ startup failed");
        {Gdiplus::Bitmap bitmap(r.right-r.left,r.bottom-r.top,PixelFormat32bppRGB);Gdiplus::Graphics graphics(&bitmap);const auto dc=graphics.GetHDC();
            if(!PrintWindow(fixture.waitWindow,dc,0))throw std::runtime_error("Wait editor print failed");GdiFlush();graphics.ReleaseHDC(dc);
            UINT count{},bytes{};Gdiplus::GetImageEncodersSize(&count,&bytes);std::vector<uint8_t> buffer(bytes);auto codecs=reinterpret_cast<Gdiplus::ImageCodecInfo*>(buffer.data());Gdiplus::GetImageEncoders(count,bytes,codecs);
            bool saved=false;for(UINT i=0;i<count;++i)if(std::wcscmp(codecs[i].MimeType,L"image/png")==0){saved=bitmap.Save(path.c_str(),&codecs[i].Clsid,nullptr)==Gdiplus::Ok;break;}if(!saved)throw std::runtime_error("Wait PNG save failed");}
        DestroyWindow(fixture.waitWindow);Gdiplus::GdiplusShutdown(token);
    }
    static void renderMailHistory(const std::filesystem::path& journal,const std::filesystem::path& path){
        if(!std::filesystem::is_directory(journal)||std::filesystem::exists(path))throw std::runtime_error("Existing journal and NEW PNG required");
        XbandDashboard fixture(SnapshotOnly{});
        fixture.mailJournal=std::make_shared<diagnostic::LocalMailJournal>(std::filesystem::absolute(journal));
        fixture.openMail(nullptr,false);
        if(!fixture.mailWindow||!fixture.mailList)throw std::runtime_error("Mail history controls missing");
        if(diagnostic::mailHistoryTime(0)!=L"1970-01-01 09:00:00")throw std::runtime_error("JST timestamp mismatch");
        const auto expected=fixture.mailJournal->page(0);
        if(size_t(SendMessageW(fixture.mailList,LVM_GETITEMCOUNT,0,0))!=expected.at("rows").size())throw std::runtime_error("History row count mismatch");
        if(!fixture.mailRows.empty()){
            const auto cells=diagnostic::mailHistoryCells(fixture.mailRows[0]);
            for(int col=0;col<8;++col){wchar_t text[1024]{};LVITEMW item{};item.iSubItem=col;item.pszText=text;item.cchTextMax=1024;SendMessageW(fixture.mailList,LVM_GETITEMTEXTW,0,reinterpret_cast<LPARAM>(&item));if(cells[size_t(col)]!=text)throw std::runtime_error("History column text mismatch");}
            mailProc(fixture.mailWindow,WM_COMMAND,983,0);
            if(!fixture.mailRaw||GetWindowTextLengthW(fixture.mailText)==0)throw std::runtime_error("Raw detail toggle failed");
            mailProc(fixture.mailWindow,WM_COMMAND,983,0);
        }
        fixture.mailPage=100000;fixture.refreshMail();
        const auto total=expected.at("total_observations").get<size_t>();
        if(fixture.mailPage!=(total?(total-1)/20:0))throw std::runtime_error("Last page clamp failed");
        fixture.mailPage=0;fixture.refreshMail();
        auto empty=expected;empty["rows"]=J::array();empty["total_observations"]=0;
        fixture.fillMail(empty);
        if(SendMessageW(fixture.mailList,LVM_GETITEMCOUNT,0,0)!=0||IsWindowEnabled(GetDlgItem(fixture.mailWindow,981)))throw std::runtime_error("Empty history controls mismatch");
        fixture.fillMail(expected);
        RECT r{};GetWindowRect(fixture.mailWindow,&r);
        SetWindowPos(fixture.mailWindow,nullptr,-30000,-30000,0,0,SWP_NOSIZE|SWP_NOACTIVATE|SWP_NOZORDER);
        ShowWindow(fixture.mailWindow,SW_SHOWNOACTIVATE);UpdateWindow(fixture.mailWindow);
        ULONG_PTR token{};Gdiplus::GdiplusStartupInput input;
        if(Gdiplus::GdiplusStartup(&token,&input,nullptr)!=Gdiplus::Ok)throw std::runtime_error("GDI+ startup failed");
        {
            Gdiplus::Bitmap bitmap(r.right-r.left,r.bottom-r.top,PixelFormat32bppRGB);
            Gdiplus::Graphics graphics(&bitmap);auto dc=graphics.GetHDC();
            if(!PrintWindow(fixture.mailWindow,dc,0))throw std::runtime_error("History print failed");
            GdiFlush();graphics.ReleaseHDC(dc);
            UINT count{},bytes{};Gdiplus::GetImageEncodersSize(&count,&bytes);std::vector<uint8_t> buffer(bytes);
            auto codecs=reinterpret_cast<Gdiplus::ImageCodecInfo*>(buffer.data());Gdiplus::GetImageEncoders(count,bytes,codecs);
            bool saved=false;for(UINT i=0;i<count;++i)if(std::wcscmp(codecs[i].MimeType,L"image/png")==0){saved=bitmap.Save(path.c_str(),&codecs[i].Clsid,nullptr)==Gdiplus::Ok;break;}
            if(!saved)throw std::runtime_error("History preview save failed");
        }
        DestroyWindow(fixture.mailWindow);Gdiplus::GdiplusShutdown(token);
    }
    static void renderUsageEditor(const std::filesystem::path &path){
        if(std::filesystem::exists(path))throw std::runtime_error("Preserve existing preview");
        XbandDashboard fixture(SnapshotOnly{});
        const auto file=std::filesystem::temp_directory_path()/("usage-editor-preview-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64()))/"settings.json";
        fixture.usageSettings=std::make_shared<diagnostic::UsageAreaSettings>(file);
        fixture.usageSettings->observe("5550101");fixture.usageSettings->save("5550102",0);
        fixture.openUsage(nullptr,false);
        if(!fixture.usageWindow)throw std::runtime_error("Usage editor creation failed");
        SetWindowTextW(fixture.usagePhone,L"5550101");SendMessageW(fixture.usageSelector,CB_SETCURSEL,2,0);
        usageProc(fixture.usageWindow,WM_COMMAND,959,0);
        usageProc(fixture.usageWindow,WM_COMMAND,952,0);
        diagnostic::UsageAreaSettings restored(file);
        if(restored.selection("5550101")!=1||restored.selection("5550102")!=0)throw std::runtime_error("Usage editor per-phone Save failed");
        auto expectedPlay=diagnostic::UsagePlayTime{};expectedPlay.enabled=true;
        if(restored.playTime("5550101")!=expectedPlay||restored.playTime("5550102").enabled)throw std::runtime_error("Play-time per-phone Save failed");
        SetWindowTextW(fixture.usagePhone,L"5550102");usageProc(fixture.usageWindow,WM_COMMAND,MAKEWPARAM(954,CBN_EDITCHANGE),0);
        if(SendMessageW(fixture.usageSelector,CB_GETCURSEL,0,0)!=1)throw std::runtime_error("Usage editor number selection failed");
        if(SendMessageW(fixture.usagePlayMode,CB_GETCURSEL,0,0)!=0||IsWindowEnabled(fixture.usagePlayEdits[0]))throw std::runtime_error("Play-time selection leaked between phones");
        SendMessageW(fixture.usagePlayMode,CB_SETCURSEL,1,0);usageProc(fixture.usageWindow,WM_COMMAND,MAKEWPARAM(956,CBN_SELCHANGE),0);
        usageProc(fixture.usageWindow,WM_COMMAND,952,0);
        diagnostic::UsageAreaSettings unlimitedRestored(file);
        if(!unlimitedRestored.playTime("5550102").enabled||!unlimitedRestored.playTime("5550102").lines[1].empty()||unlimitedRestored.playTime("5550101")!=expectedPlay)throw std::runtime_error("Unlimited/clear-second-line Save failed");
        SetWindowTextW(fixture.usagePhone,L"5550101");usageProc(fixture.usageWindow,WM_COMMAND,MAKEWPARAM(954,CBN_EDITCHANGE),0);
        if(SendMessageW(fixture.usagePlayMode,CB_GETCURSEL,0,0)!=2||!IsWindowEnabled(fixture.usagePlayEdits[0]))throw std::runtime_error("Custom play-time fields not restored");
        diagnostic::UsageSchedule sampleSchedule;sampleSchedule.enabled=true;sampleSchedule.weekdays={1,540,1020};
        fixture.fillUsageSchedule(sampleSchedule);usageProc(fixture.usageWindow,WM_COMMAND,952,0);
        diagnostic::UsageAreaSettings scheduleRestored(file);
        if(scheduleRestored.schedule("5550101")!=sampleSchedule||scheduleRestored.schedule("5550102").enabled||IsWindowEnabled(fixture.usagePlayMode)||!IsWindowEnabled(fixture.usageDayStarts[0])||IsWindowEnabled(fixture.usageDayStarts[1]))throw std::runtime_error("Native schedule Save/field enable mismatch");
        if(usageText(fixture.usageMessage).find(diagnostic::rankingUTF8(L"時間帯制限 有効（対戦のみ）"))==std::string::npos)throw std::runtime_error("Saved enabled policy not explained");
        SendMessageW(fixture.usageSelector,CB_SETCURSEL,3,0);usageProc(fixture.usageWindow,WM_COMMAND,952,0);
        diagnostic::UsageAreaSettings selectableRestored(file);
        if(selectableRestored.selection("5550101")!=2||selectableRestored.selection("5550102")!=0||selectableRestored.schedule("5550101")!=sampleSchedule)throw std::runtime_error("Native selectable-area Save/preservation failed");
        SetWindowTextW(fixture.usagePhone,L"5550102");usageProc(fixture.usageWindow,WM_COMMAND,MAKEWPARAM(954,CBN_EDITCHANGE),0);
        if(SendMessageW(fixture.usageSelector,CB_GETCURSEL,0,0)!=1)throw std::runtime_error("Selectable area leaked between phones");
        if(usageText(fixture.usageMessage).find(diagnostic::rankingUTF8(L"時間帯制限 無効"))==std::string::npos)throw std::runtime_error("Saved disabled policy not explained");
        SetWindowTextW(fixture.usagePhone,L"5550101");usageProc(fixture.usageWindow,WM_COMMAND,MAKEWPARAM(954,CBN_EDITCHANGE),0);
        if(SendMessageW(fixture.usageSelector,CB_GETCURSEL,0,0)!=3)throw std::runtime_error("Selectable area UI restore failed");
        RECT client{};GetClientRect(fixture.usageWindow,&client);
        for(const auto child:{fixture.usagePlayEdits[0],fixture.usagePlayEdits[1],fixture.usageMessage,GetDlgItem(fixture.usageWindow,952)}){
            RECT bounds{};GetWindowRect(child,&bounds);MapWindowPoints(HWND_DESKTOP,fixture.usageWindow,reinterpret_cast<POINT*>(&bounds),2);
            if(bounds.left<0||bounds.top<0||bounds.right>client.right||bounds.bottom>client.bottom)throw std::runtime_error("Usage editor control clipped");
        }
        RECT r{};GetWindowRect(fixture.usageWindow,&r);
        SetWindowPos(fixture.usageWindow,nullptr,-30000,-30000,0,0,SWP_NOSIZE|SWP_NOACTIVATE|SWP_NOZORDER);
        ShowWindow(fixture.usageWindow,SW_SHOWNOACTIVATE);UpdateWindow(fixture.usageWindow);
        ULONG_PTR token{};Gdiplus::GdiplusStartupInput input;
        if(Gdiplus::GdiplusStartup(&token,&input,nullptr)!=Gdiplus::Ok)throw std::runtime_error("GDI+ startup failed");
        {
            Gdiplus::Bitmap bitmap(r.right-r.left,r.bottom-r.top,PixelFormat32bppRGB);
            Gdiplus::Graphics graphics(&bitmap);auto dc=graphics.GetHDC();
            if(!PrintWindow(fixture.usageWindow,dc,0))throw std::runtime_error("Usage editor print failed");
            GdiFlush();graphics.ReleaseHDC(dc);
            UINT count{},bytes{};Gdiplus::GetImageEncodersSize(&count,&bytes);std::vector<uint8_t> buffer(bytes);
            auto codecs=reinterpret_cast<Gdiplus::ImageCodecInfo*>(buffer.data());Gdiplus::GetImageEncoders(count,bytes,codecs);
            bool saved=false;for(UINT i=0;i<count;++i)if(std::wcscmp(codecs[i].MimeType,L"image/png")==0){saved=bitmap.Save(path.c_str(),&codecs[i].Clsid,nullptr)==Gdiplus::Ok;break;}
            if(!saved)throw std::runtime_error("Usage editor preview save failed");
        }
        DestroyWindow(fixture.usageWindow);Gdiplus::GdiplusShutdown(token);
    }
    static void renderRankingEditor(const std::filesystem::path &path,unsigned selected=0){
        if(std::filesystem::exists(path))throw std::runtime_error("Preserve existing preview");
        XbandDashboard fixture(SnapshotOnly{});
        const auto settingsPath=std::filesystem::temp_directory_path()/("ranking-editor-preview-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64()))/"settings.json";
        fixture.rankingSettings=std::make_shared<diagnostic::GameRankingSettings>(settingsPath);
        fixture.openRankings(nullptr,false);
        if(!fixture.rankingWindow)throw std::runtime_error("Ranking editor creation failed");
        SendMessageW(fixture.rankingSelector,CB_SETCURSEL,selected,0);fixture.fillRanking();
        SendMessageW(fixture.rankingAward,CB_SETCURSEL,1,0);EnableWindow(fixture.rankingAwardValue,TRUE);SetWindowTextW(fixture.rankingAwardValue,L"10");
        EnableWindow(fixture.rankingLoseValue,TRUE);SetWindowTextW(fixture.rankingLoseValue,L"3");
        // Exercise the actual Save handler and verify per-game persistence.
        fixture.saveRanking();
        if(!std::filesystem::exists(settingsPath))throw std::runtime_error("Editor Save handler did not persist settings");
        diagnostic::GameRankingSettings restored(settingsPath);
        if(restored.snapshot().at(selected).winPoints!=10||restored.snapshot().at(selected).losePoints!=3)throw std::runtime_error("Editor winner/loser award persistence");
        SetWindowTextW(fixture.rankingLoseValue,L"");fixture.saveRanking();
        diagnostic::GameRankingSettings afterInvalid(settingsPath);
        if(afterInvalid.snapshot().at(selected).losePoints!=3)throw std::runtime_error("Invalid editor award changed settings");
        fixture.fillRanking();
        if(!IsWindowEnabled(fixture.rankingLoseValue))throw std::runtime_error("Loser control not enabled on selection");
        if(restored.snapshot().at(selected).fields!=fixture.rankingRows.at(selected).fields)throw std::runtime_error("Editor fields changed on save");
        SetWindowTextW(GetDlgItem(fixture.rankingWindow,915),L"0x00010009");
        SetWindowTextW(GetDlgItem(fixture.rankingWindow,916),L"MANUAL TEST");
        rankingProc(fixture.rankingWindow,WM_COMMAND,917,0);
        diagnostic::GameRankingSettings added(settingsPath);
        if(added.snapshot().size()!=diagnostic::rankingDefaults().size()+1||added.snapshot().back().gameID!=0x10009||added.snapshot().back().winPoints!=diagnostic::newGameDefaultWinPoints)throw std::runtime_error("Manual game UI registration failed");
        rankingProc(fixture.rankingWindow,WM_COMMAND,917,0);
        if(diagnostic::GameRankingSettings(settingsPath).snapshot().size()!=diagnostic::rankingDefaults().size()+1)throw std::runtime_error("Duplicate UI registration accepted");
        SendMessageW(fixture.rankingSelector,CB_SETCURSEL,selected,0);fixture.fillRanking();
        RECT r{};GetWindowRect(fixture.rankingWindow,&r);
        SetWindowPos(fixture.rankingWindow,nullptr,-30000,-30000,0,0,SWP_NOSIZE|SWP_NOACTIVATE|SWP_NOZORDER);
        ShowWindow(fixture.rankingWindow,SW_SHOWNOACTIVATE);UpdateWindow(fixture.rankingWindow);
        ULONG_PTR token{};Gdiplus::GdiplusStartupInput input;
        if(Gdiplus::GdiplusStartup(&token,&input,nullptr)!=Gdiplus::Ok)throw std::runtime_error("GDI+ startup failed");
        {
            Gdiplus::Bitmap bitmap(r.right-r.left,r.bottom-r.top,PixelFormat32bppRGB);
            Gdiplus::Graphics graphics(&bitmap);auto dc=graphics.GetHDC();
            if(!PrintWindow(fixture.rankingWindow,dc,0))throw std::runtime_error("Ranking editor print failed");
            GdiFlush();graphics.ReleaseHDC(dc);
            UINT count{},bytes{};Gdiplus::GetImageEncodersSize(&count,&bytes);std::vector<uint8_t> buffer(bytes);
            auto codecs=reinterpret_cast<Gdiplus::ImageCodecInfo*>(buffer.data());Gdiplus::GetImageEncoders(count,bytes,codecs);
            bool saved=false;for(UINT i=0;i<count;++i)if(std::wcscmp(codecs[i].MimeType,L"image/png")==0){saved=bitmap.Save(path.c_str(),&codecs[i].Clsid,nullptr)==Gdiplus::Ok;break;}
            if(!saved)throw std::runtime_error("Ranking editor preview save failed");
        }
        DestroyWindow(fixture.rankingWindow);Gdiplus::GdiplusShutdown(token);
    }
    static void renderSnapshot(const J &snapshot,const std::filesystem::path &path){
        if(std::filesystem::exists(path))throw std::runtime_error("Preserve existing preview");
        ULONG_PTR token{};Gdiplus::GdiplusStartupInput input;
        if(Gdiplus::GdiplusStartup(&token,&input,nullptr)!=Gdiplus::Ok)throw std::runtime_error("GDI+ startup failed");
        struct Stop{ULONG_PTR token;~Stop(){Gdiplus::GdiplusShutdown(token);}} stop{token};
        Gdiplus::Bitmap bitmap(1080,800,PixelFormat32bppRGB);
        {
            Gdiplus::Graphics graphics(&bitmap);const auto dc=graphics.GetHDC();
            XbandDashboard fixture(SnapshotOnly{});fixture.data=snapshot;fixture.draw(dc);
            WNDCLASSW c{};c.lpfnWndProc=DefWindowProcW;c.hInstance=GetModuleHandleW(nullptr);c.lpszClassName=L"XbandButtonLayoutPreview";RegisterClassW(&c);
            HWND w=CreateWindowW(c.lpszClassName,L"",WS_POPUP|WS_CLIPCHILDREN,-30000,-30000,1080,800,nullptr,nullptr,c.hInstance,nullptr);
            if(!w)throw std::runtime_error("Button test window");
            // This test host uses DefWindowProc, so invoke creation with our fixture explicitly.
            SetWindowLongPtrW(w,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(&fixture));
            for(UINT m=WM_APP+41;m<=WM_APP+45;++m)proc(w,m,0,0);
            for(int id:{979,978,950,975,900}){
                auto h=GetDlgItem(w,id);RECT r{};GetWindowRect(h,&r);MapWindowPoints(nullptr,w,reinterpret_cast<POINT*>(&r),2);
                if(!h||r.top!=76||r.bottom!=108||r.right-r.left!=194)throw std::runtime_error("Button alignment");
                int saved=SaveDC(dc);SetViewportOrgEx(dc,r.left,r.top,nullptr);SendMessageW(h,WM_PRINT,reinterpret_cast<WPARAM>(dc),PRF_CLIENT|PRF_NONCLIENT);RestoreDC(dc,saved);
            }
            SetWindowPos(w,nullptr,0,0,810,600,SWP_NOMOVE|SWP_NOZORDER);layoutButtons(w);
            for(int id:{979,978,950,975,900}){RECT r{};GetWindowRect(GetDlgItem(w,id),&r);if(r.bottom-r.top!=24)throw std::runtime_error("Resized button alignment");}
            DestroyWindow(w);GdiFlush();graphics.ReleaseHDC(dc);
        }
        UINT count{},bytes{};Gdiplus::GetImageEncodersSize(&count,&bytes);
        if(!bytes)throw std::runtime_error("PNG encoder unavailable");
        std::vector<uint8_t> buffer(bytes);auto codecs=reinterpret_cast<Gdiplus::ImageCodecInfo*>(buffer.data());
        if(Gdiplus::GetImageEncoders(count,bytes,codecs)!=Gdiplus::Ok)throw std::runtime_error("PNG codecs unavailable");
        for(UINT i=0;i<count;++i)if(std::wcscmp(codecs[i].MimeType,L"image/png")==0){
            if(bitmap.Save(path.c_str(),&codecs[i].Clsid,nullptr)!=Gdiplus::Ok)throw std::runtime_error("Preview save failed");return;
        }
        throw std::runtime_error("PNG encoder not found");
    }
    static void testButtonRepaintIsolation(){
        auto check=[](bool ok){if(!ok)throw std::runtime_error("Dashboard repaint isolation assertion");};
        XbandDashboard fixture(SnapshotOnly{});
        WNDCLASSW c{};c.lpfnWndProc=proc;c.hInstance=GetModuleHandleW(nullptr);c.lpszClassName=L"XbandDashboardRepaintTest";
        RegisterClassW(&c);
        HWND w=CreateWindowW(c.lpszClassName,L"",WS_POPUP|WS_CLIPCHILDREN,-30000,-30000,1080,800,nullptr,nullptr,c.hInstance,&fixture);
        check(w!=nullptr);check((GetWindowLongPtrW(w,GWL_STYLE)&WS_CLIPCHILDREN)!=0);
        for(unsigned i=0;i<6;++i)SendMessageW(w,WM_APP+41+i,0,0);
        unsigned changes=0;
        const auto observer=+[](HWND child,UINT message,WPARAM a,LPARAM b,UINT_PTR,DWORD_PTR reference)->LRESULT{
            if(message==WM_SETFONT||message==WM_WINDOWPOSCHANGED)++*reinterpret_cast<unsigned*>(reference);
            return DefSubclassProc(child,message,a,b);
        };
        for(int id:{979,978,950,975,900,976}){
            auto h=GetDlgItem(w,id);check(h!=nullptr);
            check(SetWindowSubclass(h,observer,1,reinterpret_cast<DWORD_PTR>(&changes))!=FALSE);
            ValidateRect(h,nullptr);
        }
        for(unsigned i=0;i<20;++i){layoutButtons(w);SendMessageW(w,WM_TIMER,1,0);}
        check(changes==0);
        for(int id:{979,978,950,975,900,976})check(!GetUpdateRect(GetDlgItem(w,id),nullptr,FALSE));
        SetWindowPos(w,nullptr,0,0,1200,900,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
        check(changes>0);const auto afterResize=changes;layoutButtons(w);check(changes==afterResize);
        for(int id:{979,978,950,975,900,976})RemoveWindowSubclass(GetDlgItem(w,id),observer,1);
        DestroyWindow(w);
    }
#endif
    XbandDashboard(){worker=std::thread([this]{WNDCLASSW c{};c.lpfnWndProc=proc;c.hInstance=GetModuleHandleW(nullptr);c.lpszClassName=L"XbandDashboard";c.hCursor=LoadCursor(nullptr,IDC_ARROW);RegisterClassW(&c);
        HWND w=CreateWindowW(c.lpszClassName,L"XBANDサーバー | 通信モニター",WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,CW_USEDEFAULT,CW_USEDEFAULT,1100,850,nullptr,nullptr,c.hInstance,this);hwnd=w;if(w){ShowWindow(w,SW_SHOWNOACTIVATE);SetTimer(w,1,250,nullptr);}ready=true;if(!w)return;MSG msg;while(GetMessage(&msg,nullptr,0,0)>0){TranslateMessage(&msg);DispatchMessage(&msg);}});while(!ready)Sleep(1);if(!hwnd){worker.join();throw std::runtime_error("Dashboard creation failed");}}
    ~XbandDashboard(){if(auto w=hwnd.load())PostMessage(w,WM_CLOSE,0,0);if(worker.joinable())worker.join();}
    bool isClosed()const{return closed;}
    void publish(const J&v){std::lock_guard lock(mutex);const auto now=GetTickCount64();const auto&p=v.at("pair_control");
        for(int i=0;i<2;++i){auto n=p.at("sent")[i].get<uint64_t>();rate[i]=last&&now>last&&n>=prior[i]?(n-prior[i])*1000/(now-last):0;prior[i]=n;}
        last=now;auto state=p.at("state").dump()+"/"+p.at("failed").dump()+"/"+p.at("joined").dump();
        if(state!=previousState){SYSTEMTIME t;GetLocalTime(&t);std::wostringstream o;o<<std::setfill(L'0')<<std::setw(2)<<t.wHour<<L":"<<std::setw(2)<<t.wMinute<<L":"<<std::setw(2)<<t.wSecond<<L"  State changed: "<<wide(state);events.push_back(o.str());if(events.size()>20)events.pop_front();previousState=state;}data=v;
    }
};
