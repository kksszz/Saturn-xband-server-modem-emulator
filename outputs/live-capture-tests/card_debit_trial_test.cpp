#define PB3_SERVICE_SERVER_TEST 1
#include "pb3_service_server.cpp"
#include "card_debit_trial_window.hpp"
#include "activity_history_window.hpp"
#include <objidl.h>
#include <gdiplus.h>
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
    auto check=[](bool v,const char* message){if(!v)throw std::runtime_error(message);};
    {
        const auto path=std::filesystem::temp_directory_path()/
            ("xband-credit-history-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64()));
        auto history=std::make_shared<diagnostic::ActivityHistory>(path,true);
        auto trials=std::make_shared<diagnostic::CardDebitTrials>();
        PB3Service audit(0,std::make_shared<PB3Observation>(),std::make_shared<xband::LocalMatchRoles>(),false,false);
        audit.setActivityHistory(history);audit.setCardDebitTrials(trials);
        trials->reserve(0,1,10000);audit.testTCP().state=LocalTCPProbe::State::SynReceived;
        audit.processCardTrialCommand();audit.publishCardTrial();audit.publishCardTrial();
        audit.testTCP().cardDebit.state=media_card::ServiceDebitExchange::State::Waiting;
        audit.publishCardTrial();audit.publishCardTrial();
        audit.testTCP().cardDebit.state=media_card::ServiceDebitExchange::State::Completed;
        audit.testTCP().cardDebit.result=1;
        audit.testTCP().cardDebit.initialCard=media_card::CardReport{94,std::array<uint8_t,13>{1,2,3,4,5,6,7,0,0,0,1,7,63}};
        audit.testTCP().cardDebit.updatedCard=media_card::CardReport{93,std::array<uint8_t,13>{1,2,3,4,5,6,7,0,0,0,1,7,31}};
        audit.publishCardTrial();audit.publishCardTrial();
        trials->continueTrial(0);audit.processCardTrialCommand();audit.publishCardTrial();
        // FIN clears this flag: it must not produce a second result row.
        audit.testTCP().cardDebitReleased=false;audit.publishCardTrial();audit.reset();
        check(history->page(0).at("total")==4,"Duplicate trial stage persisted");
        const auto rows=history->page(0).at("rows");
        check(rows[0]["event"]=="credit_trial_continued"&&rows[0]["remaining_credits"]==93&&
            rows[0]["consumed_credits"]==1&&!rows[0].contains("points_delta"),"Credit history conflated or lost result");
        check(diagnostic::creditTrialDetail(rows[0]).find(L"93")!=std::wstring::npos&&
            diagnostic::creditTrialDetail(rows[0]).find(L"未確認")!=std::wstring::npos,
            "Credit detail hid balance or invented unknown pre-debit balance");
        diagnostic::ActivityHistory reopened(path);
        check(reopened.page(0).at("rows")==rows,"Credit history restart lost observations");
        trials->reserve(0,3,10000);audit.testTCP().state=LocalTCPProbe::State::SynReceived;
        audit.processCardTrialCommand();audit.publishCardTrial();
        audit.testTCP().cardDebit.state=media_card::ServiceDebitExchange::State::Waiting;audit.publishCardTrial();audit.reset();
        check(history->page(0).at("rows")[0]["event"]=="credit_trial_interrupted"&&
            !history->page(0).at("rows")[0].contains("consumed_credits"),"Interruption invented consumption result");
        check(audit.testTCP().cardDebit.state==media_card::ServiceDebitExchange::State::Disabled,"Audit replayed interrupted trial");
        trials->reserve(0,1,10000);audit.testTCP().state=LocalTCPProbe::State::SynReceived;
        audit.processCardTrialCommand();audit.publishCardTrial();
        audit.testTCP().cardDebit.state=media_card::ServiceDebitExchange::State::Uncertain;
        audit.publishCardTrial();audit.publishCardTrial();
        const auto count=history->page(0).at("total");audit.reset();
        check(history->page(0).at("total")==count&&history->page(0).at("rows")[0]["event"]=="credit_trial_uncertain"&&
            !history->page(0).at("rows")[0].contains("remaining_credits"),"Timeout invented balance or duplicated history");
        trials->reserve(0,3,10000);audit.testTCP().state=LocalTCPProbe::State::SynReceived;
        audit.processCardTrialCommand();audit.publishCardTrial();
        auto& inconsistent=audit.testTCP().cardDebit;
        inconsistent.initialCard=media_card::CardReport{100,std::array<uint8_t,13>{1,2,3,4,5,6,7,0,0,0,1,15,15}};
        inconsistent.updatedCard=inconsistent.initialCard;inconsistent.result=3;
        inconsistent.state=media_card::ServiceDebitExchange::State::Completed;
        audit.publishCardTrial();audit.publishCardTrial();
        const auto conflict=history->page(0).at("rows")[0];
        check(conflict["event"]=="credit_trial_result"&&conflict["consumed_credits"]==3&&
            conflict["remaining_credits"]==100&&conflict["credit_trial_continuation_verified"]==false&&
            conflict["credit_trial_continuation_problem"].get<std::string>().find("Balance difference")!=std::string::npos,
            "Contradictory observations lost or marked as verified in history");
        bool blocked=false;try{trials->continueTrial(0);}catch(const std::exception&){blocked=true;}
        check(blocked&&!audit.testTCP().cardDebitReleased,"Contradictory history trial continued");audit.reset();
    }
    auto bus=std::make_shared<diagnostic::CardDebitTrials>();
    bus->reserve(1,3,10000);
    check(bus->snapshot(1).pending&&!bus->snapshot(0).pending,"Trial endpoint isolation");
    PB3Service service(1,std::make_shared<PB3Observation>(),std::make_shared<xband::LocalMatchRoles>(),false,false);
    service.setCardDebitTrials(bus);service.processCardTrialCommand();service.publishCardTrial();
    check(bus->snapshot(1).pending&&bus->snapshot(1).amount==3&&
        service.testTCP().cardDebit.state==media_card::ServiceDebitExchange::State::Disabled,
        "Idle epoch consumed reservation or hid requested amount");
    service.reset();service.tick(1);service.reset();service.tick(2);
    check(bus->snapshot(1).pending,"Pre-dial resets discarded unsent reservation");
    service.testTCP().state=LocalTCPProbe::State::SynReceived;
    service.processCardTrialCommand();service.publishCardTrial();
    check(service.testTCP().cardDebit.state==media_card::ServiceDebitExchange::State::Armed&&
        bus->snapshot(1).phase=="Armed"&&!bus->snapshot(1).pending,"Reserved trial did not arm owner");
    bool rejected=false;try{bus->reserve(1,3,10000);}catch(const std::exception&){rejected=true;}
    check(rejected,"Repeated arm allowed");
    service.testTCP().cardDebit.state=media_card::ServiceDebitExchange::State::Waiting;service.publishCardTrial();
    service.reset();
    check(service.testTCP().cardDebit.state==media_card::ServiceDebitExchange::State::Disabled&&
        !bus->snapshot(1).pending&&bus->snapshot(1).last.find("Waiting")!=std::string::npos,
        "Reset repeated debit or erased unconfirmed summary");
    service.tick(1);
    check(service.testTCP().cardDebit.state==media_card::ServiceDebitExchange::State::Disabled,"Reset trial retried");
    diagnostic::CardDebitTrialWindow window;window.setTrials(bus);window.open(nullptr,false);
    const auto w=window.handle();
    SendMessageW(GetDlgItem(w,1),CB_SETCURSEL,1,0);SendMessageW(w,WM_TIMER,1,0);
    SetWindowTextW(GetDlgItem(w,2),L"3");SetWindowTextW(GetDlgItem(w,3),L"10");
    SendMessageW(w,WM_COMMAND,4,0);check(bus->snapshot(1).pending,"UI did not queue explicit arm");
    service.testTCP().state=LocalTCPProbe::State::SynReceived;
    service.processCardTrialCommand();service.publishCardTrial();
    service.testTCP().cardDebit.state=media_card::ServiceDebitExchange::State::Completed;
    service.testTCP().cardDebit.result=2;service.publishCardTrial();SendMessageW(w,WM_TIMER,1,0);
    check(!IsWindowEnabled(GetDlgItem(w,5)),"Partial debit enabled continuation");
    service.testTCP().cardDebit.result=3;service.publishCardTrial();SendMessageW(w,WM_TIMER,1,0);
    check(!IsWindowEnabled(GetDlgItem(w,5)),"Unknown balance enabled continuation");
    service.testTCP().cardDebit.initialCard=media_card::CardReport{3,std::array<uint8_t,13>{1,2,3,4,5,6,7,0,0,0,0,0,7}};
    service.testTCP().cardDebit.updatedCard=service.testTCP().cardDebit.initialCard;
    service.publishCardTrial();SendMessageW(w,WM_TIMER,1,0);
    check(!IsWindowEnabled(GetDlgItem(w,5))&&bus->snapshot(1).result==3&&bus->snapshot(1).remaining==3,
        "Contradictory balance enabled continuation or lost observations");
    rejected=false;try{bus->continueTrial(1);}catch(const std::exception&){rejected=true;}
    check(rejected&&!bus->snapshot(1).pending,"Mailbox released inconsistent debit");
    service.testTCP().cardDebit.updatedCard=media_card::CardReport{0,std::array<uint8_t,13>{1,2,3,4,5,6,7,0,0,0,0,0,0}};
    (*service.testTCP().cardDebit.updatedCard->raw)[0]=9;
    service.publishCardTrial();SendMessageW(w,WM_TIMER,1,0);
    check(!IsWindowEnabled(GetDlgItem(w,5)),"Changed identity enabled continuation");
    (*service.testTCP().cardDebit.updatedCard->raw)[0]=1;
    service.publishCardTrial();SendMessageW(w,WM_TIMER,1,0);
    check(bus->snapshot(1).remaining==0,"Zero remaining balance was hidden");
    check(IsWindowEnabled(GetDlgItem(w,5)),"Matching result did not enable continuation");
    SendMessageW(w,WM_COMMAND,5,0);
    check(!service.testTCP().cardDebitReleased&&bus->snapshot(1).pending,"UI directly mutated service");
    service.processCardTrialCommand();service.publishCardTrial();
    check(service.testTCP().cardDebitReleased&&bus->snapshot(1).phase=="Continued","Owner did not continue explicitly");
    // A stale continuation cannot apply to a replacement session.
    auto stale=std::make_shared<diagnostic::CardDebitTrials>();auto epoch=stale->begin(0);
    stale->publish(0,epoch,"Completed",3,3,100,{},97,true);stale->continueTrial(0);stale->finish(0,epoch);
    auto next=stale->begin(0);check(!stale->take(0,next),"Stale continuation crossed reset");
    for(int id:{1,2,3,4,5}){RECT r{};GetWindowRect(GetDlgItem(w,id),&r);check(r.right>r.left&&r.bottom>r.top,"Missing trial UI geometry");}
    SendMessageW(w,WM_TIMER,1,0);if(argc==2)render(w,std::filesystem::absolute(argv[1]));DestroyWindow(w);
    std::cout<<"PASS trial UI/owner mailbox: endpoint isolation, one-shot reserve, queued continuation, partial/unknown/inconsistent balance and identity blocked, reset no retry, stale session guard\n";
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
