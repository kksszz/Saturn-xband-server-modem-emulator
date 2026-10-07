#pragma once
#include <windows.h>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <algorithm>

// Read-only latest-frame viewer. Its own message thread keeps window dragging
// and painting out of the emulation loop. No input is forwarded to the guest.
class ProbePreview {
    std::thread worker;
    std::atomic<HWND> window{nullptr};
    std::atomic<bool> ready{false},closed{false},completed{false};
    std::mutex mutex;
    std::vector<uint32_t> pixels;
    unsigned width=0,height=0;
    const std::wstring caption;
    const bool textMode;
    std::wstring statusText=L"Waiting for diagnostic state...";
    std::chrono::steady_clock::time_point last{};
    static LRESULT CALLBACK procedure(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp) {
        auto* self=reinterpret_cast<ProbePreview*>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));
        if(msg==WM_NCCREATE) {
            self=static_cast<ProbePreview*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            SetWindowLongPtrW(hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));
        }
        if(!self)return DefWindowProcW(hwnd,msg,wp,lp);
        switch(msg) {
        case WM_TIMER:
            SetWindowTextW(hwnd,(self->caption+(self->completed?L" - Finished (close this window)":L" - Running / read-only / 10 Hz")).c_str());
            InvalidateRect(hwnd,nullptr,FALSE);return 0;
        case WM_ERASEBKGND:return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;HDC dc=BeginPaint(hwnd,&ps);RECT r;GetClientRect(hwnd,&r);
            FillRect(dc,&r,static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
            std::lock_guard lock(self->mutex);
            if(self->textMode) {
                SetBkMode(dc,TRANSPARENT);SetTextColor(dc,RGB(220,240,230));
                auto old=SelectObject(dc,GetStockObject(ANSI_FIXED_FONT));
                RECT textRect=r;InflateRect(&textRect,-18,-18);
                DrawTextW(dc,self->statusText.c_str(),-1,&textRect,DT_LEFT|DT_TOP|DT_WORDBREAK|DT_NOPREFIX);
                SelectObject(dc,old);
            }
            if(self->width&&self->height&&!self->pixels.empty()) {
                int w=r.right,h=int(int64_t(w)*self->height/self->width);
                if(h>r.bottom){h=r.bottom;w=int(int64_t(h)*self->width/self->height);}
                BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
                info.bmiHeader.biWidth=self->width;info.bmiHeader.biHeight=-int(self->height);
                info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
                SetStretchBltMode(dc,COLORONCOLOR);
                StretchDIBits(dc,(r.right-w)/2,(r.bottom-h)/2,w,h,0,0,self->width,self->height,
                    self->pixels.data(),&info,DIB_RGB_COLORS,SRCCOPY);
            }
            EndPaint(hwnd,&ps);return 0;
        }
        case WM_DESTROY:self->window=nullptr;self->closed=true;PostQuitMessage(0);return 0;
        }
        return DefWindowProcW(hwnd,msg,wp,lp);
    }
public:
    static uint32_t dibPixel(uint32_t p) {return ((p&255)<<16)|(p&0xff00)|((p>>16)&255);}
    ProbePreview(bool monitor=false,const std::wstring &label=L"",int slot=-1):caption(!label.empty()?label:monitor?L"XBAND Synthetic Peer Monitor (NOT a second game)":L"XBAND Test Preview"),textMode(monitor) {
        worker=std::thread([this,slot]{
            const auto instance=GetModuleHandleW(nullptr);
            WNDCLASSW wc{};wc.lpfnWndProc=procedure;wc.hInstance=instance;
            wc.lpszClassName=L"YmirDiagnosticPreview";wc.hCursor=LoadCursorW(nullptr,MAKEINTRESOURCEW(32512));
            if(!RegisterClassW(&wc)&&GetLastError()!=ERROR_CLASS_ALREADY_EXISTS){ready=true;return;}
            const int previewWidth=slot>=0?(std::min)(800,(GetSystemMetrics(SM_CXSCREEN)-60)/2):800;
            const int previewHeight=slot>=0?(std::min)(600,GetSystemMetrics(SM_CYSCREEN)-140):600;
            auto hwnd=CreateWindowExW(0,wc.lpszClassName,caption.c_str(),WS_OVERLAPPEDWINDOW,
                slot>=0?20+slot*(previewWidth+20):CW_USEDEFAULT,slot>=0?40:CW_USEDEFAULT,
                previewWidth,previewHeight,nullptr,nullptr,instance,this);
            window=hwnd;
            if(hwnd){SetTimer(hwnd,1,100,nullptr);ShowWindow(hwnd,SW_SHOWNOACTIVATE);}
            ready=true;
            if(!hwnd)return;
            MSG msg;while(GetMessageW(&msg,nullptr,0,0)>0){TranslateMessage(&msg);DispatchMessageW(&msg);}
        });
        while(!ready)std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if(!window){worker.join();throw std::runtime_error("Preview window creation failed");}
    }
    ~ProbePreview(){if(auto hwnd=window.load())PostMessageW(hwnd,WM_CLOSE,0,0);if(worker.joinable())worker.join();}
    void publish(const uint32_t* frame,unsigned w,unsigned h,bool force=false) {
        if(closed||!w||!h||w>2048||h>2048)return;
        const auto now=std::chrono::steady_clock::now();
        if(!force&&now-last<std::chrono::milliseconds(100))return;
        std::unique_lock lock(mutex,std::try_to_lock);if(!lock)return;
        last=now;width=w;height=h;pixels.resize(size_t(w)*h);
        for(size_t i=0;i<pixels.size();++i)pixels[i]=dibPixel(frame[i]);
    }
    void publishText(const std::string &text,bool force=false) {
        if(closed||!textMode)return;
        const auto now=std::chrono::steady_clock::now();
        if(!force&&now-last<std::chrono::milliseconds(100))return;
        std::unique_lock lock(mutex,std::try_to_lock);if(!lock)return;
        last=now;statusText.assign(text.begin(),text.end());
    }
    void markFinished(){completed=true;}
    bool isClosed()const noexcept{return closed.load();}
    void finishAndClose(){markFinished();if(auto hwnd=window.load())PostMessageW(hwnd,WM_CLOSE,0,0);if(worker.joinable())worker.join();}
    void finishAndWait(){markFinished();if(worker.joinable())worker.join();}
};
