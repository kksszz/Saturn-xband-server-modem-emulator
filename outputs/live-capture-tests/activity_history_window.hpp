#pragma once
#include "activity_history.hpp"
#include "mail_history_view.hpp"
#include <functional>
#include <commctrl.h>
namespace diagnostic {
inline std::wstring activityLabel(const std::string& event){
    const std::pair<const char*,const wchar_t*> labels[]={
        {"credit_binding_review",L"度数精算・対戦照合候補"},
        {"credit_service_pending",L"度数精算・準備"},{"credit_service_sent",L"度数精算・消費要求送信"},
        {"credit_service_confirmed",L"度数精算・確定"},{"credit_service_uncertain",L"度数精算・結果不明"},
        {"credit_service_exhausted",L"度数精算・残高不足の部分消費"},
        {"credit_service_blocked",L"度数精算・処理停止"},
        {"credit_match_deferred",L"対戦精算・保留（サービス継続）"},
        {"credit_match_waiting",L"対戦精算・端末結果の未確認"},{"credit_mail_denied",L"対戦精算後・メール残度数不足"},
        {"credit_notice_sent",L"度数案内・接続終了"},
        {"credit_insufficient",L"残度数不足・案内終了"},
        {"credit_trial_armed",L"度数試験・要求受付"},{"credit_trial_sent",L"度数試験・49送信"},
        {"credit_trial_result",L"度数試験・ゲームDISC(ROM)実績受信"},{"credit_trial_continued",L"度数試験・手動継続"},
        {"credit_trial_insufficient",L"度数試験・不足案内終了"},
        {"credit_trial_uncertain",L"度数試験・結果未確認"},{"credit_trial_interrupted",L"度数試験・接続中断"},
        {"access",L"アクセス受付"},{"standby",L"待機登録"},{"ready",L"着信待機開始"},
        {"matched",L"マッチング成立"},{"dial",L"相手へ発信"},{"connected",L"対戦回線接続"},
        {"hangup",L"対戦回線切断"},{"cancel",L"待機終了・取消"},{"service_end",L"サービス応答終了"},
        {"service_abort",L"サービス接続中断"},{"error",L"接続処理エラー"},
        {"time_denied",L"時間帯制限・接続拒否"},{"time_expired",L"時間帯制限・待機終了"},
        {"terminal_join",L"端末接続"},{"terminal_leave",L"端末切断"},
        {"points_prepared",L"付与値配信準備"},{"points_ledger",L"ポイント台帳記録"},{"points_baseline",L"累積ポイント0開始"},{"points_duplicate",L"結果再報告・加算なし"},{"points_result",L"対戦結果・加算算出"}};
    for(const auto& [key,label]:labels)if(event==key)return label;return rankingWide(event);
}
inline std::array<std::wstring,14> activityCells(const nlohmann::json& row){
    const auto slot=row.value("profile",-1);
    auto event=activityLabel(row.at("event").get<std::string>());
    if(row.value("credit_trial_scope",std::string{})=="reviewed-reset-one-shot")event=L"リセット精算・"+event;
    if(row.value("credit_trial_scope",std::string{})=="automatic-match-settlement")event=L"自動対戦・"+event;
    if(row.contains("result_outcome"))event+=L"（"+rankingWide(row.at("result_outcome").get<std::string>())+L"）";
    auto game=rankingWide(row.value("title",std::string{}));
    if(game.empty()&&row.contains("game")){std::wostringstream s;s<<L"0x"<<std::hex<<row.at("game").get<uint32_t>();game=s.str();}
    auto status=rankingWide(row.value("point_status",std::string{}));
    if(row.contains("credit_trial_session")||row.contains("credit_settlement_state")){
        const auto ledger=row.value("credit_settlement_state",std::string{});
        status=ledger=="confirmed"?L"精算確定・再送対象外":ledger=="exhausted"?L"部分消費確認・不足終了・再送禁止":ledger=="uncertain"?L"結果不明・再送禁止":
            ledger=="issued"?L"送信済み・結果待ち":ledger=="pending"?L"未精算":
            row.value("credit_trial_continuation_verified",false)?L"ゲームDISC(ROM)実績一致・台帳状態未記録":L"消費未確認";
    }
    if(row.contains("credit_binding_candidates"))status=L"未検証・自動消費なし";
    if(row.value("credit_trial_shortfall",false))status=L"部分消費確認・要求量は未精算・再送禁止";
    return {std::to_wstring(row.at("id").get<uint64_t>()),mailHistoryTime(row.value("reported_unix_ms",row.at("unix_ms").get<uint64_t>())),
        row.contains("side")?std::to_wstring(row.at("side").get<unsigned>()+1):L"",
        rankingWide(row.value("name",std::string{})),slot>=0?std::to_wstring(slot+1):L"",
        rankingWide(row.value("phone",std::string{})),game,rankingWide(row.value("request_type",std::string{})),event,
        rankingWide(row.value("target",std::string{})),
        row.contains("configured_points")?std::to_wstring(row.at("configured_points").get<int>()):L"",
        row.contains("points_delta")?std::to_wstring(row.at("points_delta").get<uint32_t>()):L"",
        row.contains("points_total")?std::to_wstring(row.at("points_total").get<uint32_t>()):L"",
        status};
}
inline std::wstring creditTrialDetail(const nlohmann::json& row){
    if(!row.contains("credit_trial_session")&&!row.contains("credit_episode"))return L"";
    auto value=[&](const char* key){return row.contains(key)&&!row.at(key).is_null()?rankingWide(row.at(key).dump()):std::wstring(L"未確認");};
    const auto scope=row.value("credit_trial_scope",std::string{});
    const std::wstring title=scope=="automatic-match-settlement"?L"対戦分と今回接続の自動精算":scope=="automatic-mail-access"?L"メール接続の自動消費":scope=="reviewed-reset-one-shot"?L"確認済みリセットの一回限り精算":
        scope=="manual-one-shot"?L"手動度数試験":L"度数試験（旧記録・種別未記録）";
    return L"\r\n"+title+((scope=="automatic-mail-access"||scope=="automatic-match-settlement")?L"（明示的に有効化したサーバー方針）":L"（通常の自動課金ではありません）")+L"　セッション: "+value("credit_trial_session")+
        L"\r\n要求度数: "+value("requested_credits")+L"　ゲームDISC(ROM)消費実績: "+value("consumed_credits")+
        L"\r\n接続時残度数: "+value("credits_before")+L"　消費後残度数: "+value("remaining_credits")+
        L"\r\n精算エピソード: "+(row.contains("credit_episode")?rankingWide(row.at("credit_episode").get<std::string>()):L"未記録")+
        L"\r\n記録時の台帳状態: "+(row.contains("credit_settlement_state")?rankingWide(row.at("credit_settlement_state").get<std::string>()):L"未記録（ゲームDISC(ROM)実績とは別）");
}
inline std::wstring creditBindingDetail(const nlohmann::json& row){
    if(!row.contains("credit_binding_candidates"))return L"";
    const auto& audit=row.at("credit_binding_candidates");
    std::wstring text=L"\r\n対戦照合: 未検証・自動消費なし　候補数: "+std::to_wstring(audit.at("candidates").size())+
        L"\r\n照合状態: "+rankingWide(audit.at("status").get<std::string>())+L"\r\n候補の接続履歴ID: ";
    bool first=true;for(const auto& candidate:audit.at("candidates")){
        if(!first)text+=L", ";first=false;text+=std::to_wstring(candidate.at("activity_record_id").get<uint64_t>());
    }
    return text+L"\r\n候補1件でも自動確定しません。接続履歴IDは精算エピソードIDではありません。";
}
class ActivityHistoryWindow {
    using J=nlohmann::json;
    HWND window=nullptr,list=nullptr,summary=nullptr,detail=nullptr;
    std::shared_ptr<ActivityHistory> history;J rows=J::array();size_t page=0;bool filling=false;
    bool resultsMode=false;std::function<void()> resultOpener;
    static std::array<std::wstring,14> resultCells(const J& r){
        auto cell=[&](const char* key){if(!r.contains(key)||r.at(key).is_null())return std::wstring(L"不明");return r.at(key).is_string()?rankingWide(r.at(key).get<std::string>()):rankingWide(r.at(key).dump());};
        return {cell("id"),mailHistoryTime(r.at("unix_ms").get<uint64_t>()),cell("side"),cell("name"),cell("reporting_name"),cell("title"),cell("local_result"),cell("remote_result"),cell("result_outcome"),cell("game_error"),cell("match_connection_id"),cell("points_delta"),cell("points_total"),cell("correlation_status")};
    }
    void selection(){
        const int i=int(SendMessageW(list,LVM_GETNEXTITEM,WPARAM(-1),LVNI_SELECTED));
        std::wstring text=L"行を選択すると詳細を表示します。";
        if(i>=0&&size_t(i)<rows.size()){
            if(resultsMode){const auto json=rankingWide(rows[size_t(i)].dump(2));std::wstring lines;
                for(auto c:json){if(c==L'\n')lines+=L'\r';lines+=c;}SetWindowTextW(detail,lines.c_str());return;}
            const auto& row=rows[size_t(i)];const auto cells=activityCells(row);
            text=L"日時 (JST): "+cells[1]+L"　端末: "+cells[2]+L"　ユーザー: "+cells[3]+L"　枠: "+cells[4]+
                L"\r\n電話: "+cells[5]+L"　ゲーム: "+cells[6]+L"\r\n状態: "+cells[8]+L"　指名相手: "+cells[9]+
                L"\r\n種別: "+cells[7]+L"　接続世代: "+std::to_wstring(row.value("generation",uint64_t{}))+
                L"\r\n設定ポイント: "+cells[10]+L"　加算額: "+cells[11]+L"　更新後: "+cells[12]+L"　確認: "+cells[13]+
                (row.contains("configured_win_points")?L"\r\n対戦開始時の設定　勝者: "+std::to_wstring(row.at("configured_win_points").get<int>())+L"　敗者: "+std::to_wstring(row.at("configured_lose_points").get<int>()):L"")+
                L"\r\n結果: "+rankingWide(row.value("result_outcome",std::string{}))+
                (row.contains("reporting_profile")?L"\r\n報告時の選択ユーザー: "+rankingWide(row.value("reporting_name",std::string{}))+L"　枠: "+std::to_wstring(row.at("reporting_profile").get<unsigned>()+1):L"")+
                (row.contains("match_connection_id")?L"\r\n照合先の対戦接続履歴ID: "+std::to_wstring(row.at("match_connection_id").get<uint64_t>()):L"")+
                (row.contains("local_result")?L"　自分: "+std::to_wstring(row.at("local_result").get<uint32_t>())+L"　相手: "+std::to_wstring(row.at("remote_result").get<uint32_t>()):L"")+
                creditTrialDetail(row)+creditBindingDetail(row)+L"\r\n"+rankingWide(row.value("detail",std::string{}));
        }
        SetWindowTextW(detail,text.c_str());
    }
    static LRESULT CALLBACK proc(HWND w,UINT m,WPARAM a,LPARAM b){
        auto s=reinterpret_cast<ActivityHistoryWindow*>(GetWindowLongPtrW(w,GWLP_USERDATA));
        if(m==WM_NCCREATE){s=static_cast<ActivityHistoryWindow*>(reinterpret_cast<CREATESTRUCTW*>(b)->lpCreateParams);SetWindowLongPtrW(w,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(s));}
        if(!s)return DefWindowProcW(w,m,a,b);
        if(m==WM_COMMAND){if(LOWORD(a)==1103&&s->resultOpener){s->resultOpener();return 0;}if(LOWORD(a)==1100&&s->page)--s->page;else if(LOWORD(a)==1101)++s->page;
            if(LOWORD(a)>=1100&&LOWORD(a)<=1102){s->refresh();return 0;}}
        if(m==WM_TIMER){s->refresh();return 0;}
        if(m==WM_NOTIFY&&reinterpret_cast<NMHDR*>(b)->hwndFrom==s->list&&reinterpret_cast<NMHDR*>(b)->code==LVN_ITEMCHANGED){if(!s->filling)s->selection();return 0;}
        if(m==WM_SIZE&&s->list){const int width=std::max(100,int(LOWORD(b))-24),height=int(HIWORD(b));
            MoveWindow(s->list,12,92,width,std::max(100,height-270),TRUE);
            MoveWindow(s->detail,12,std::max(212,height-166),width,150,TRUE);return 0;}
        if(m==WM_CLOSE){DestroyWindow(w);return 0;}
        if(m==WM_DESTROY){s->window=s->list=s->summary=s->detail=nullptr;return 0;}
        return DefWindowProcW(w,m,a,b);
    }
public:
    void setHistory(std::shared_ptr<ActivityHistory> value){history=std::move(value);}
    void setResultHistory(std::shared_ptr<ActivityHistory> value){history=std::move(value);resultsMode=true;}
    void setResultOpener(std::function<void()> value){resultOpener=std::move(value);}
    void refresh(){
        if(!window||!history)return;
        auto result=history->page(page);page=result.at("page").get<size_t>();
        const auto total=result.at("total").get<size_t>();
        const auto label=L"全 "+std::to_wstring(total)+L" 件　ページ "+std::to_wstring(page+1)+L" / "+std::to_wstring(std::max(size_t(1),(total+49)/50))+
            (result.at("error").get<std::string>().empty()?L"":L"　保存エラー: "+rankingWide(result.at("error").get<std::string>()));
        SetWindowTextW(summary,label.c_str());
        EnableWindow(GetDlgItem(window,1100),page!=0);EnableWindow(GetDlgItem(window,1101),total&&(page<(total-1)/50));
        if(rows==result.at("rows"))return;
        uint64_t selectedID=0;const int selected=int(SendMessageW(list,LVM_GETNEXTITEM,WPARAM(-1),LVNI_SELECTED));
        if(selected>=0&&size_t(selected)<rows.size())selectedID=rows[size_t(selected)].at("id").get<uint64_t>();
        filling=true;SendMessageW(list,WM_SETREDRAW,FALSE,0);SendMessageW(list,LVM_DELETEALLITEMS,0,0);rows=result.at("rows");int select=0;
        for(size_t i=0;i<rows.size();++i){auto cells=resultsMode?resultCells(rows[i]):activityCells(rows[i]);LVITEMW item{};item.mask=LVIF_TEXT;item.iItem=int(i);item.pszText=cells[0].data();
            SendMessageW(list,LVM_INSERTITEMW,0,reinterpret_cast<LPARAM>(&item));
            for(unsigned c=1;c<cells.size();++c){item.iSubItem=int(c);item.pszText=cells[c].data();SendMessageW(list,LVM_SETITEMTEXTW,i,reinterpret_cast<LPARAM>(&item));}
            if(rows[i].at("id")==selectedID)select=int(i);
        }
        if(!rows.empty()){LVITEMW item{};item.stateMask=LVIS_SELECTED|LVIS_FOCUSED;item.state=item.stateMask;SendMessageW(list,LVM_SETITEMSTATE,select,reinterpret_cast<LPARAM>(&item));}
        SendMessageW(list,WM_SETREDRAW,TRUE,0);InvalidateRect(list,nullptr,TRUE);filling=false;selection();
    }
    HWND handle()const{return window;}
    HWND table()const{return list;}
    void open(HWND owner,bool show=true){
        if(!history)return;
        if(window){ShowWindow(window,SW_SHOW);SetForegroundWindow(window);return;}
        WNDCLASSW c{};c.lpfnWndProc=proc;c.hInstance=GetModuleHandleW(nullptr);c.lpszClassName=L"XBANDActivityHistory";c.hCursor=LoadCursor(nullptr,IDC_ARROW);c.hbrBackground=GetSysColorBrush(COLOR_WINDOW);RegisterClassW(&c);
        INITCOMMONCONTROLSEX common{sizeof(common),ICC_LISTVIEW_CLASSES};InitCommonControlsEx(&common);
        window=CreateWindowW(c.lpszClassName,resultsMode?L"XBAND | 対戦結果DB（端末報告）":L"XBAND | アクセス・マッチング・ポイント履歴",WS_OVERLAPPEDWINDOW,CW_USEDEFAULT,CW_USEDEFAULT,1300,760,owner,nullptr,c.hInstance,this);
        if(!window)throw std::runtime_error("Activity window creation failed");
        for(unsigned i=0;i<3;++i)CreateWindowW(L"BUTTON",i==0?L"前へ":i==1?L"次へ":L"更新",WS_CHILD|WS_VISIBLE|WS_TABSTOP,12+i*110,12,100,30,window,reinterpret_cast<HMENU>(INT_PTR(1100+i)),c.hInstance,nullptr);
        if(!resultsMode&&resultOpener)CreateWindowW(L"BUTTON",L"対戦結果DB...",WS_CHILD|WS_VISIBLE|WS_TABSTOP,350,12,160,30,window,reinterpret_cast<HMENU>(INT_PTR(1103)),c.hInstance,nullptr);
        summary=CreateWindowW(L"STATIC",L"",WS_CHILD|WS_VISIBLE,530,18,700,24,window,nullptr,c.hInstance,nullptr);
        CreateWindowW(L"STATIC",resultsMode?L"端末の結果報告を永続保存。両側の報告は別行。参加者照合は既存サーバー方針、未報告・未解明項目は不明です。":L"新しい履歴が先頭です（1秒更新）。回線接続・切断の観測であり、ゲームの勝敗や正常終了の判定ではありません。",WS_CHILD|WS_VISIBLE,12,55,1250,28,window,nullptr,c.hInstance,nullptr);
        list=CreateWindowW(WC_LISTVIEWW,L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP|WS_BORDER|LVS_REPORT|LVS_SINGLESEL|LVS_SHOWSELALWAYS,12,92,1250,430,window,nullptr,c.hInstance,nullptr);
        SendMessageW(list,LVM_SETEXTENDEDLISTVIEWSTYLE,0,LVS_EX_FULLROWSELECT|LVS_EX_GRIDLINES|LVS_EX_DOUBLEBUFFER);
        const wchar_t* labels[]={L"ID",L"日時 (JST)",L"端末",L"ユーザー",L"枠",L"電話",L"ゲーム",L"種別",L"イベント",L"指名相手",L"設定ポイント",L"加算額",L"更新後ポイント",L"確認状態"};
        const int widths[]={55,170,50,100,40,120,220,90,180,110,100,80,120,160};
        const wchar_t* resultLabels[]={L"ID",L"受信日時 (JST)",L"端末",L"対戦ユーザー",L"報告ユーザー",L"ゲーム",L"自端末結果",L"相手結果",L"報告上の勝敗",L"エラー値",L"接続履歴ID",L"実加算",L"累積ポイント",L"照合状態"};
        for(unsigned i=0;i<14;++i){LVCOLUMNW col{};col.mask=LVCF_TEXT|LVCF_WIDTH;col.cx=resultsMode?(i==1?170:i==5?200:i==13?280:110):widths[i];col.pszText=const_cast<wchar_t*>(resultsMode?resultLabels[i]:labels[i]);SendMessageW(list,LVM_INSERTCOLUMNW,i,reinterpret_cast<LPARAM>(&col));}
        const int order[]={0,1,3,6,8,10,11,12,13,2,4,5,7,9};
        if(!resultsMode)SendMessageW(list,LVM_SETCOLUMNORDERARRAY,14,reinterpret_cast<LPARAM>(order));
        detail=CreateWindowW(L"EDIT",L"",WS_CHILD|WS_VISIBLE|WS_VSCROLL|ES_MULTILINE|ES_READONLY,12,534,1250,150,window,nullptr,c.hInstance,nullptr);
        for(HWND child=GetWindow(window,GW_CHILD);child;child=GetWindow(child,GW_HWNDNEXT))SendMessageW(child,WM_SETFONT,reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)),TRUE);
        rows=J::array();page=0;refresh();RECT rect{};GetClientRect(window,&rect);SendMessageW(window,WM_SIZE,0,MAKELPARAM(rect.right,rect.bottom));
        SetTimer(window,1,1000,nullptr);if(show)ShowWindow(window,SW_SHOW);
    }
};
}
