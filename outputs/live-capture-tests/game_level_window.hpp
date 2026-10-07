#pragma once
#include "game_level_settings.hpp"
#include <commctrl.h>
namespace diagnostic {
class GameLevelWindow {
    HWND window=nullptr,gameBox=nullptr,rankBox=nullptr,valueBox=nullptr,stepBox=nullptr,list=nullptr,message=nullptr;
    HWND unitBox=nullptr,directionBox=nullptr;
    std::shared_ptr<GameLevelSettings> settings;std::shared_ptr<GameRankingSettings> names;
    std::vector<GameRankingRow> games;
    uint32_t game()const{auto i=SendMessageW(gameBox,CB_GETCURSEL,0,0);if(i<0||size_t(i)>=games.size())throw std::runtime_error("Select game");return games[size_t(i)].gameID;}
    static uint32_t number(HWND h){wchar_t buffer[32]{};GetWindowTextW(h,buffer,32);std::wstring s(buffer);
        if(s.empty()||s.size()>9||s.find_first_not_of(L"0123456789")!=s.npos)throw std::runtime_error("Enter decimal points 0..999999999");return uint32_t(std::stoul(s));}
    void fill(){
        const auto t=settings->thresholds(game());SendMessageW(list,LVM_DELETEALLITEMS,0,0);
        const auto selected=SendMessageW(rankBox,CB_GETCURSEL,0,0);SendMessageW(rankBox,CB_RESETCONTENT,0,0);
        for(unsigned i=0;i<30;++i){std::wstring level=rankingWide(settings->label(game(),i+1)),start=std::to_wstring(t[i]);
            SendMessageW(rankBox,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(level.c_str()));
            std::wstring range=i==29?start+L"以上":start+L" ～ "+std::to_wstring(t[i+1]-1);
            LVITEMW item{};item.mask=LVIF_TEXT;item.iItem=int(i);item.pszText=level.data();SendMessageW(list,LVM_INSERTITEMW,0,reinterpret_cast<LPARAM>(&item));
            item.iSubItem=1;item.pszText=start.data();SendMessageW(list,LVM_SETITEMTEXTW,i,reinterpret_cast<LPARAM>(&item));
            item.iSubItem=2;item.pszText=range.data();SendMessageW(list,LVM_SETITEMTEXTW,i,reinterpret_cast<LPARAM>(&item));
        }
        SendMessageW(rankBox,CB_SETCURSEL,selected<0?1:selected,0);
        const auto a=settings->appearance(game());SendMessageW(unitBox,CB_SETCURSEL,a.unit=="LEVEL"?0:a.unit==rankingUTF8(L"段")?1:2,0);
        SendMessageW(directionBox,CB_SETCURSEL,a.descending?1:0,0);
        selectRank();
    }
    void selectRank(){const auto i=SendMessageW(rankBox,CB_GETCURSEL,0,0);if(i<0||i>=30)return;
        SetWindowTextW(valueBox,std::to_wstring(settings->thresholds(game())[size_t(i)]).c_str());EnableWindow(valueBox,i!=0);}
    void save(bool uniform){try{
        auto t=settings->thresholds(game());if(uniform)t=GameLevelSettings::uniform(number(stepBox));
        else {const auto i=SendMessageW(rankBox,CB_GETCURSEL,0,0);if(i<0||i>=30)throw std::runtime_error("Select rank");t[size_t(i)]=number(valueBox);}
        settings->save(game(),t);fill();SetWindowTextW(message,L"保存しました。次のサーバー接続で累積ポイントから再計算します。");
    }catch(const std::exception& e){SetWindowTextW(message,rankingWide(e.what()).c_str());}}
    static LRESULT CALLBACK proc(HWND w,UINT m,WPARAM a,LPARAM b){
        auto s=reinterpret_cast<GameLevelWindow*>(GetWindowLongPtrW(w,GWLP_USERDATA));
        if(m==WM_NCCREATE){s=static_cast<GameLevelWindow*>(reinterpret_cast<CREATESTRUCTW*>(b)->lpCreateParams);SetWindowLongPtrW(w,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(s));}
        if(!s)return DefWindowProcW(w,m,a,b);
        if(m==WM_COMMAND){
            if(LOWORD(a)==1001&&HIWORD(a)==CBN_SELCHANGE)s->fill();
            if(LOWORD(a)==1002&&HIWORD(a)==CBN_SELCHANGE)s->selectRank();
            if(LOWORD(a)==1004)s->save(false);if(LOWORD(a)==1006)s->save(true);
            if(LOWORD(a)==1010)try{
                const auto u=SendMessageW(s->unitBox,CB_GETCURSEL,0,0),d=SendMessageW(s->directionBox,CB_GETCURSEL,0,0);
                if(u<0||u>2||d<0||d>1)throw std::runtime_error("Select unit/direction");
                s->settings->saveAppearance(s->game(),{u==0?"LEVEL":rankingUTF8(u==1?L"段":L"級"),d==1});s->fill();SetWindowTextW(s->message,L"ゲーム別の単位と数字の順序を保存しました。");
            }catch(const std::exception& e){SetWindowTextW(s->message,rankingWide(e.what()).c_str());}
            return 0;
        }
        if(m==WM_CLOSE){DestroyWindow(w);return 0;}
        if(m==WM_DESTROY){s->window=nullptr;return 0;}
        return DefWindowProcW(w,m,a,b);
    }
public:
    HWND handle()const{return window;}
    void open(HWND owner,std::shared_ptr<GameLevelSettings> levels,std::shared_ptr<GameRankingSettings> titles,bool show=true){
        if(!levels||!titles)return;if(window){ShowWindow(window,SW_SHOW);SetForegroundWindow(window);return;}
        settings=std::move(levels);names=std::move(titles);games=names->snapshot();
        WNDCLASSW c{};c.lpfnWndProc=proc;c.hInstance=GetModuleHandleW(nullptr);c.lpszClassName=L"XBANDGameLevels";c.hCursor=LoadCursor(nullptr,IDC_ARROW);c.hbrBackground=GetSysColorBrush(COLOR_BTNFACE);RegisterClassW(&c);
        INITCOMMONCONTROLSEX common{sizeof(common),ICC_LISTVIEW_CLASSES};InitCommonControlsEx(&common);
        window=CreateWindowW(c.lpszClassName,L"XBAND | 昇格条件（30ランク）",WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU,CW_USEDEFAULT,CW_USEDEFAULT,730,780,owner,nullptr,c.hInstance,this);
        if(!window)throw std::runtime_error("Level editor creation failed");
        auto add=[&](const wchar_t* cls,const wchar_t* text,DWORD style,int x,int y,int width,int height,int id){
            auto h=CreateWindowW(cls,text,WS_CHILD|WS_VISIBLE|style,x,y,width,height,window,reinterpret_cast<HMENU>(INT_PTR(id)),c.hInstance,nullptr);
            SendMessageW(h,WM_SETFONT,reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)),TRUE);return h;};
        add(L"STATIC",L"ゲーム",0,18,22,85,24,0);
        gameBox=add(L"COMBOBOX",L"",CBS_DROPDOWNLIST|WS_VSCROLL|WS_TABSTOP,110,18,570,220,1001);
        for(const auto& r:games){const auto text=rankingWide(r.fields[0]);SendMessageW(gameBox,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(text.c_str()));}
        add(L"STATIC",L"ランク",0,18,68,85,24,0);rankBox=add(L"COMBOBOX",L"",CBS_DROPDOWNLIST|WS_VSCROLL|WS_TABSTOP,110,64,145,260,1002);
        for(unsigned i=1;i<=30;++i){auto text=L"LEVEL "+std::to_wstring(i);SendMessageW(rankBox,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(text.c_str()));}
        add(L"STATIC",L"到達ポイント",0,272,68,110,24,0);
        valueBox=add(L"COMBOBOX",L"",CBS_DROPDOWN|WS_VSCROLL|WS_TABSTOP,386,64,155,230,1003);SendMessageW(valueBox,CB_LIMITTEXT,9,0);
        for(unsigned i=0;i<=30;++i){auto text=std::to_wstring(i*200);SendMessageW(valueBox,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(text.c_str()));}
        add(L"BUTTON",L"このランクを保存",WS_TABSTOP,550,64,130,30,1004);
        add(L"STATIC",L"全30ランクの間隔",0,18,115,145,24,0);
        stepBox=add(L"COMBOBOX",L"200",CBS_DROPDOWN|WS_VSCROLL|WS_TABSTOP,170,110,150,200,1005);SendMessageW(stepBox,CB_LIMITTEXT,9,0);
        for(const auto* text:{L"50",L"100",L"200",L"500",L"1000",L"2000"})SendMessageW(stepBox,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(text));
        SetWindowTextW(stepBox,L"200");add(L"BUTTON",L"選択ゲームに一括適用",WS_TABSTOP,340,110,205,30,1006);
        add(L"STATIC",L"表示単位",0,18,159,85,24,0);unitBox=add(L"COMBOBOX",L"",CBS_DROPDOWNLIST|WS_TABSTOP,110,155,145,150,1008);
        for(const auto* text:{L"LEVEL",L"段",L"級"})SendMessageW(unitBox,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(text));
        directionBox=add(L"COMBOBOX",L"",CBS_DROPDOWNLIST|WS_TABSTOP,272,155,265,150,1009);
        for(const auto* text:{L"昇順（1 → 30）",L"降順（30 → 1）"})SendMessageW(directionBox,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(text));
        add(L"BUTTON",L"表示形式を保存",WS_TABSTOP,550,155,130,30,1010);
        add(L"STATIC",L"初期値は200刻み（第1ランク=0、第30ランク=5800）。当時の公式昇格表ではありません。\n数値は直接入力も可能です。最初は0固定、到達ポイントは低い順に設定します。",0,18,200,680,46,0);
        list=add(WC_LISTVIEWW,L"",WS_BORDER|LVS_REPORT|LVS_SINGLESEL,18,250,662,400,1007);
        SendMessageW(list,LVM_SETEXTENDEDLISTVIEWSTYLE,0,LVS_EX_FULLROWSELECT|LVS_EX_GRIDLINES|LVS_EX_DOUBLEBUFFER);
        const wchar_t* labels[]={L"ランク",L"到達ポイント",L"累積ポイント範囲"};const int widths[]={120,160,350};
        for(unsigned i=0;i<3;++i){LVCOLUMNW col{};col.mask=LVCF_TEXT|LVCF_WIDTH;col.cx=widths[i];col.pszText=const_cast<wchar_t*>(labels[i]);SendMessageW(list,LVM_INSERTCOLUMNW,i,reinterpret_cast<LPARAM>(&col));}
        message=add(L"STATIC",L"変更したいゲームとランクを選択してください。最大ランクでは次の昇格はありません。",0,18,670,662,60,0);
        SendMessageW(gameBox,CB_SETCURSEL,0,0);SendMessageW(rankBox,CB_SETCURSEL,1,0);fill();if(show)ShowWindow(window,SW_SHOW);
    }
    void testControls(){
        if(!window||SendMessageW(rankBox,CB_GETCOUNT,0,0)!=30||SendMessageW(list,LVM_GETITEMCOUNT,0,0)!=30)throw std::runtime_error("Level dropdown/table missing ranks");
        SetWindowTextW(stepBox,L"300");save(true);if(settings->thresholds(game())[29]!=8700)throw std::runtime_error("Level uniform UI save failed");
        SendMessageW(rankBox,CB_SETCURSEL,1,0);selectRank();SetWindowTextW(valueBox,L"350");save(false);if(settings->thresholds(game())[1]!=350)throw std::runtime_error("Level individual UI save failed");
        SetWindowTextW(valueBox,L"600");save(false);if(settings->thresholds(game())[1]!=350)throw std::runtime_error("Invalid level UI save accepted");
        SendMessageW(unitBox,CB_SETCURSEL,2,0);SendMessageW(directionBox,CB_SETCURSEL,1,0);proc(window,WM_COMMAND,1010,0);
        if(settings->label(game(),1)!=rankingUTF8(L"30級"))throw std::runtime_error("Level format UI save failed");
        DestroyWindow(window);
    }
};
}
