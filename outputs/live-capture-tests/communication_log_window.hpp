#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include "communication_log.hpp"
#pragma comment(lib,"comdlg32.lib")
#pragma comment(lib,"shell32.lib")

namespace diagnostic {
class CommunicationLogWindow {
    HWND window=nullptr,body=nullptr,status=nullptr,category=nullptr,side=nullptr,direction=nullptr,search=nullptr,automatic=nullptr;
    std::shared_ptr<CommunicationLog> log;
    size_t page=0;std::wstring displayed;
    static std::wstring wide(const std::string& s){if(s.empty())return {};const int n=MultiByteToWideChar(CP_UTF8,0,s.data(),int(s.size()),nullptr,0);std::wstring v(n,0);MultiByteToWideChar(CP_UTF8,0,s.data(),int(s.size()),v.data(),n);return v;}
    static std::string utf8(const std::wstring& s){if(s.empty())return {};const int n=WideCharToMultiByte(CP_UTF8,0,s.data(),int(s.size()),nullptr,0,nullptr,nullptr);std::string v(n,0);WideCharToMultiByte(CP_UTF8,0,s.data(),int(s.size()),v.data(),n,nullptr,nullptr);return v;}
    HWND control(const wchar_t* cls,const wchar_t* title,DWORD style,int id,int x,int y,int width,int height){
        auto h=CreateWindowW(cls,title,WS_CHILD|WS_VISIBLE|WS_TABSTOP|style,x,y,width,height,window,reinterpret_cast<HMENU>(INT_PTR(id)),GetModuleHandleW(nullptr),nullptr);
        SendMessageW(h,WM_SETFONT,reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)),TRUE);return h;
    }
    void refresh(bool tail=false){
        if(!log)return;
        const auto snap=log->snapshot();if(tail&&!snap.pages.empty())page=snap.pages.size()-1;
        if(page>=snap.pages.size())page=snap.pages.empty()?0:snap.pages.size()-1;
        wchar_t query[256]{};GetWindowTextW(search,query,256);const auto needle=utf8(query);
        const int c=int(SendMessageW(category,CB_GETCURSEL,0,0)),s=int(SendMessageW(side,CB_GETCURSEL,0,0)),d=int(SendMessageW(direction,CB_GETCURSEL,0,0));
        std::ostringstream out;
        if(!snap.pages.empty())for(const auto& row:CommunicationLog::readPage(snap.pages[page])){
            const auto channel=row.value("channel",std::string{});
            if(c>0&&channel!=(c==1?"Service":c==2?"Peer":"Control"))continue;
            if(s>0&&row.value("side",0)!=unsigned(s-1))continue;
            if(d>0&&row.value("direction",std::string{})!=(d==1?"TX":"RX"))continue;
            const auto line="["+std::to_string(row.value("unix_ms",uint64_t{}))+"] "+channel+" Modem "+std::to_string(row.value("side",0)+1)+" "+row.value("direction",std::string{})+" session="+std::to_string(row.value("session",uint64_t{}))+" frame="+std::to_string(row.value("guest_frame",0u))+"\r\nHEX  "+row.value("hex",std::string{})+"\r\nTEXT "+row.value("text",std::string{})+"\r\n\r\n";
            if(!needle.empty()&&line.find(needle)==std::string::npos)continue;
            out<<line;
        }
        const auto text=wide(out.str());
        if(text!=displayed){displayed=text;SetWindowTextW(body,displayed.c_str());if(tail){SendMessageW(body,EM_SETSEL,displayed.size(),displayed.size());SendMessageW(body,EM_SCROLLCARET,0,0);}}
        const auto label=L"Page "+std::to_wstring(snap.pages.empty()?0:page+1)+L"/"+std::to_wstring(snap.pages.size())+L" | Records: "+std::to_wstring(snap.records)+L" | "+(snap.error.empty()?L"Recording":wide(snap.error))+L"\r\n"+snap.directory.wstring();
        SetWindowTextW(status,label.c_str());
        EnableWindow(GetDlgItem(window,11),page>0);EnableWindow(GetDlgItem(window,12),page+1<snap.pages.size());
    }
    void save(){
        wchar_t path[32768]=L"communication-log-page.txt";
        OPENFILENAMEW dialog{};dialog.lStructSize=sizeof(dialog);dialog.hwndOwner=window;dialog.lpstrFile=path;dialog.nMaxFile=32768;dialog.lpstrFilter=L"Text files\0*.txt\0All files\0*.*\0";dialog.lpstrDefExt=L"txt";dialog.Flags=OFN_OVERWRITEPROMPT|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR;
        if(!GetSaveFileNameW(&dialog))return;
        try{
            const auto target=std::filesystem::weakly_canonical(path);
            auto extension=target.extension().wstring();for(auto& c:extension)c=towlower(c);
            if(extension==L".jsonl")throw std::runtime_error("Export as .txt, not a capture .jsonl file.");
            const auto source=std::filesystem::weakly_canonical(log->snapshot().directory);
            const auto relative=target.lexically_relative(source);
            if(!relative.empty()&&*relative.begin()!=L"..")throw std::runtime_error("Export outside the active log directory.");
            std::ofstream out(target,std::ios::binary|std::ios::trunc);out<<utf8(displayed);out.close();if(!out)throw std::runtime_error("Cannot save page.");
        }catch(const std::exception& e){MessageBoxW(window,wide(e.what()).c_str(),L"Log export",MB_ICONERROR);}
    }
    static LRESULT CALLBACK proc(HWND w,UINT m,WPARAM a,LPARAM b){
        auto self=reinterpret_cast<CommunicationLogWindow*>(GetWindowLongPtrW(w,GWLP_USERDATA));
        if(m==WM_NCCREATE){self=static_cast<CommunicationLogWindow*>(reinterpret_cast<CREATESTRUCTW*>(b)->lpCreateParams);self->window=w;SetWindowLongPtrW(w,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));}
        if(!self)return DefWindowProcW(w,m,a,b);
        if(m==WM_GETMINMAXINFO){auto info=reinterpret_cast<MINMAXINFO*>(b);info->ptMinTrackSize={1100,400};return 0;}
        if(m==WM_CREATE){
            self->category=self->control(L"COMBOBOX",L"",CBS_DROPDOWNLIST,1,12,12,125,200);
            for(auto text:{L"All channels",L"Service",L"Peer",L"Control"})SendMessageW(self->category,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(text));
            self->side=self->control(L"COMBOBOX",L"",CBS_DROPDOWNLIST,2,147,12,115,200);
            for(auto text:{L"Both modems",L"Modem 1",L"Modem 2"})SendMessageW(self->side,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(text));
            self->direction=self->control(L"COMBOBOX",L"",CBS_DROPDOWNLIST,3,272,12,100,200);
            for(auto text:{L"TX + RX",L"TX",L"RX"})SendMessageW(self->direction,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(text));
            for(auto h:{self->category,self->side,self->direction})SendMessageW(h,CB_SETCURSEL,0,0);
            self->control(L"STATIC",L"Find on page:",0,0,382,16,85,22);
            self->search=self->control(L"EDIT",L"",WS_BORDER|ES_AUTOHSCROLL,4,472,12,230,24);SendMessageW(self->search,EM_SETLIMITTEXT,255,0);
            self->automatic=self->control(L"BUTTON",L"Auto-scroll",BS_AUTOCHECKBOX,5,714,12,110,24);SendMessageW(self->automatic,BM_SETCHECK,BST_CHECKED,0);
            self->control(L"BUTTON",L"Previous",0,11,12,46,80,25);self->control(L"BUTTON",L"Next",0,12,100,46,70,25);
            self->control(L"BUTTON",L"Latest",0,13,178,46,70,25);self->control(L"BUTTON",L"Save page...",0,14,258,46,100,25);self->control(L"BUTTON",L"Open log folder",0,15,368,46,120,25);
            self->control(L"STATIC",L"TX = modem to server; RX = server to modem. ASCII escapes; not protocol decoding.",0,0,500,50,540,23);
            self->status=self->control(L"STATIC",L"",0,6,12,82,1040,46);
            self->body=self->control(L"EDIT",L"",WS_BORDER|WS_VSCROLL|WS_HSCROLL|ES_MULTILINE|ES_READONLY|ES_AUTOVSCROLL|ES_AUTOHSCROLL,7,12,137,1040,495);
            SendMessageW(self->body,EM_SETLIMITTEXT,4*1024*1024,0);SendMessageW(self->body,WM_SETFONT,reinterpret_cast<WPARAM>(GetStockObject(ANSI_FIXED_FONT)),TRUE);
            self->refresh(true);SetTimer(w,1,500,nullptr);return 0;
        }
        if(m==WM_TIMER){if(SendMessageW(self->automatic,BM_GETCHECK,0,0)==BST_CHECKED)self->refresh(true);return 0;}
        if(m==WM_SIZE&&self->body){RECT r{};GetClientRect(w,&r);MoveWindow(self->body,12,137,std::max(100L,r.right-24),std::max(100L,r.bottom-149),TRUE);return 0;}
        if(m==WM_COMMAND){
            const auto id=LOWORD(a),notice=HIWORD(a);
            if((id<=3&&notice==CBN_SELCHANGE)||(id==4&&notice==EN_CHANGE)){self->refresh(false);return 0;}
            if(id==11||id==12){SendMessageW(self->automatic,BM_SETCHECK,BST_UNCHECKED,0);if(id==11&&self->page)--self->page;if(id==12)++self->page;self->refresh(false);return 0;}
            if(id==13){self->refresh(true);return 0;}if(id==14){self->save();return 0;}
            if(id==15){ShellExecuteW(w,L"open",self->log->snapshot().directory.c_str(),nullptr,nullptr,SW_SHOWNORMAL);return 0;}
        }
        if(m==WM_CLOSE){DestroyWindow(w);return 0;}
        if(m==WM_DESTROY){KillTimer(w,1);self->window=nullptr;self->body=nullptr;self->displayed.clear();return 0;}
        return DefWindowProcW(w,m,a,b);
    }
public:
    ~CommunicationLogWindow(){if(window)DestroyWindow(window);}
    HWND handle()const{return window;}
    void open(HWND owner,std::shared_ptr<CommunicationLog> journal,bool visible=true){
        if(window){ShowWindow(window,SW_SHOW);SetForegroundWindow(window);return;}
        if(!journal){MessageBoxW(owner,L"Communication logging is unavailable. Check the server startup log.",L"Communication logs",MB_ICONWARNING);return;}
        log=std::move(journal);page=0;
        WNDCLASSW cls{};cls.lpfnWndProc=proc;cls.hInstance=GetModuleHandleW(nullptr);cls.lpszClassName=L"XBANDCommunicationLogWindow";cls.hCursor=LoadCursor(nullptr,IDC_ARROW);cls.hbrBackground=reinterpret_cast<HBRUSH>(COLOR_WINDOW+1);RegisterClassW(&cls);
        CreateWindowW(cls.lpszClassName,L"Communication logs - private data; do not share without review",WS_OVERLAPPEDWINDOW,CW_USEDEFAULT,CW_USEDEFAULT,1100,700,owner,nullptr,cls.hInstance,this);
        if(window&&visible)ShowWindow(window,SW_SHOWNORMAL);
    }
};
}
