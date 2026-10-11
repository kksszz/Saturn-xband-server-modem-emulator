// Mednafen XBAND host adapter. Included inside MDFN_IEN_SS in cart.cpp.
// Host bus/IRQ/clock binding. Network runtime uses a separate C ABI; no game-memory writes.
static XbandCardRuntime* XbandCard=nullptr;
static unsigned XbandCardReads=0;
static bool XbandCardSaveFailed=false;
static XbandCommandRuntime* XbandCommands=nullptr;
static std::unique_ptr<xband::ModemRegisterBank> XbandBank;
static std::unique_ptr<xband::mednafen_probe::RequestedBusBridge> XbandBus;
static bool XbandDirty=false;
static unsigned XbandProbeReads=0;
static unsigned XbandUartTrace=0;
static void XbandUpdateIRQ()
{
 const uint8_t code=xband_command_interrupt(XbandCommands,XbandBank->ier,XbandBank->fcr,unsigned(XbandBank->rx.size()));
 SCU_SetInt(SCU_INT_EXT0+12,code==4||code==12);
}
static void XbandCommandObserved(const char* line)
{
 MDFN_printf("[XBAND AT] %s\n",line);
}
static void XbandDrainCommands()
{
 while(XbandBank->rx.size()<256)
 {
  const int b=xband_command_pop(XbandCommands);
  if(b<0) break;
  XbandBank->rx.push_back(uint8_t(b));
 }
}
static void XbandClock()
{
 xband_command_clock(XbandCommands,SS_XbandCycles(),unsigned(XbandBank->rx.size()));
 XbandDrainCommands();XbandUpdateIRQ();
}
static sscpu_timestamp_t XbandUpdate(sscpu_timestamp_t timestamp)
{
 XbandClock();
 return xband_command_peer(XbandCommands)?timestamp+256:SS_EVENT_DISABLED_TS;
}
extern "C" void xband_mednafen_host_poll(void)
{
 if(!XbandCommands||!XbandBank)return;
 XbandClock();xband_command_poll(XbandCommands);XbandClock();
 if(xband_command_peer(XbandCommands)&&events[SS_EVENT_CART].event_time==SS_EVENT_DISABLED_TS)
  SS_SetEventNT(&events[SS_EVENT_CART],SH7095_mem_timestamp+256);
}

static bool XbandOwns(uint32 address,unsigned width,bool write)
{
 return XbandBus && (write ? XbandBus->ownsWrite(address,width) : XbandBus->ownsRead(address,width));
}
static void XbandAccess(uint32 address,unsigned width,bool write,uint32* db)
{
 assert(XbandOwns(address,width,write));
 if(write)
 {
  const auto programmed=XbandBank->programBytes;
  XbandBus->write(address,width,*db);
  if(programmed && XbandBank->programBytes!=programmed) XbandDirty=true;
 }
 else
 {
  XbandBus->read(address,width,*db);
  if(address==0x05885029 && width==1){
   xband_command_board_identified(XbandCommands);
   if(XbandProbeReads++==0)
    MDFN_printf("[XBAND probe] ROM read modem identification: 0x11 (XBAND adapter).\n");
  }
 }
}
static void XbandReset(bool powering_up)
{
 xband_card_reset(XbandCard);
 XbandCardReads=0;
 XbandBank->resetUART();
 xband_command_reset(XbandCommands);
 XbandUpdateIRQ();
 XbandBank->flashUnlock=XbandBank->programBytes=0;
 XbandBank->flashIdentify=false;
 XbandProbeReads=0;
 XbandUartTrace=0;
}
static void XbandKill()
{
 if(XbandCard&&!xband_card_flush(XbandCard)) MDFN_printf("[XBAND card] SAVE FAILED; previous image preserved.\n");
 xband_card_destroy(XbandCard);XbandCard=nullptr;
 SCU_SetInt(SCU_INT_EXT0+12,false);
 XbandBus.reset(); XbandBank.reset();
 xband_command_destroy(XbandCommands); XbandCommands=nullptr;
}
static bool XbandGetClearDirty()
{
 const bool dirty=XbandDirty; XbandDirty=false; return dirty;
}
static void XbandGetNV(const char** ext,void** ptr,bool* nv16,uint64* size)
{
 *ext="xbf"; *ptr=XbandBank->flashData.data(); *nv16=false; *size=XbandBank->flashData.size();
}
static void XbandState(StateMem* sm,const unsigned load,const bool data_only)
{
 auto& b=*XbandBank;
 SFORMAT regs[]={
  SFPTR8N(b.flashData.data(),b.flashData.size(),SFORMAT::FORM::NVMEM,"Flash"),
  SFVAR(b.dll),SFVAR(b.dlm),SFVAR(b.ier),SFVAR(b.lcr),SFVAR(b.mcr),SFVAR(b.scr),SFVAR(b.fcr),
  SFVAR(b.programBytes),SFVAR(b.flashUnlock),SFVAR(b.flashIdentify),SFEND
 };
 MDFNSS_StateAction(sm,load,data_only,regs,"CART_XBAND_PROBE");
 if(load) { b.rx.clear(); xband_command_reset(XbandCommands); xband_card_reset(XbandCard); XbandUpdateIRQ(); XbandDirty=true; XbandProbeReads=0; XbandUartTrace=0; }
}
static void XbandFrameCompleted()
{
 xband_card_frame(XbandCard);
 // Filesystem work outside mapped bus callbacks. Card bytes are persistent
 // external media and deliberately do not rewind with emulator save states.
 if(XbandCard&&!XbandCardSaveFailed&&!xband_card_flush(XbandCard))
 {
  XbandCardSaveFailed=true;
  MDFN_printf("[XBAND card] SAVE FAILED; card ejected, previous image preserved.\n");
 }
 XbandClock();
 const bool verified=SS_XbandPeekRAM(0x0602b164,2)==0x2f86&&
  SS_XbandPeekRAM(0x060943ac,2)==0x2fe6&&SS_XbandPeekRAM(0x06094378,4)==0x060ba5bc;
 xband_command_observe_wait(XbandCommands,SS_XbandPeekRAM(0x060b59e4,4),
  SS_XbandPeekRAM(0x060ba5b8,4),SS_XbandPeekRAM(0x060ba5bc,4),
  SS_XbandPeekRAM(0x060ba5b4,2)==1,SS_XbandPeekRAM(0x060b732c,1)!=0,verified);
 xband_command_frame(XbandCommands);XbandDrainCommands();XbandUpdateIRQ();
}
static void XbandInit(CartInfo* c)
{
 XbandBank.reset(new xband::ModemRegisterBank);
 XbandCommands=xband_command_create(XbandCommandObserved,[]{XbandBank->rx.clear();});
 if(!XbandCommands) throw MDFN_Error(0,"Could not initialize XBAND AT runtime");
 const char* servicePort=std::getenv("XBAND_MEDNAFEN_SERVICE_PORT");
 const char* subscriber=std::getenv("XBAND_MEDNAFEN_SUBSCRIBER");
 const char* sideText=std::getenv("XBAND_MEDNAFEN_SIDE");
 const unsigned side=sideText&&std::string(sideText)=="1"?1:0;
 if(!xband_command_configure_side(XbandCommands,side))throw MDFN_Error(0,"Invalid XBAND side");
 if(servicePort&&*servicePort){
  char* end=nullptr;const long port=std::strtol(servicePort,&end,10);
  if(!end||*end||port<1024||port>65530||!xband_command_configure_service(XbandCommands,uint16_t(port),subscriber))
   throw MDFN_Error(0,"Invalid isolated XBAND service configuration");
  if(!xband_command_configure_control(XbandCommands,uint16_t(port+4)))
   throw MDFN_Error(0,"Invalid isolated XBAND control configuration");
 }
 XbandBus.reset(new xband::mednafen_probe::RequestedBusBridge(*XbandBank));
 XbandBus->mode.identify=XbandBus->mode.uart=XbandBus->mode.flash=XbandBus->mode.storage=true;
 const wchar_t* cardPath=_wgetenv(L"XBAND_MEDNAFEN_CARD");
 XbandCard=cardPath&&*cardPath?xband_card_create(cardPath):nullptr;
 XbandCardReads=0;XbandCardSaveFailed=false;
 if(cardPath&&*cardPath&&!XbandCard) throw MDFN_Error(0,"Could not load private XBAND trial card; file preserved");
 xband_command_configure_card(XbandCommands,XbandCard);
 XbandBus->cardRead=[](uint8_t fallback){
  if(XbandCardReads++==0) MDFN_printf("[XBAND card] Guest read card pins; units=%d\n",xband_card_units(XbandCard));
  return xband_card_read(XbandCard,fallback);
 };
 XbandBus->cardWrite=[](uint8_t value){xband_card_write(XbandCard,value);};
 XbandBus->uartRead=[](uint32_t a) {
  XbandClock();
  const uint8_t interrupt=xband_command_interrupt(XbandCommands,XbandBank->ier,XbandBank->fcr,unsigned(XbandBank->rx.size()));
  const uint8_t value=XbandBank->readRegister(a,true,interrupt,xband_command_carrier(XbandCommands)!=0,xband_command_tx_status(XbandCommands),[](uint8_t b){
   xband_command_uart_activity(XbandCommands,0);
   if(XbandUartTrace++<512) MDFN_printf("[XBAND RX] %02x\n",unsigned(b));
  });
  XbandUpdateIRQ();
  return value;
 };
 XbandBus->uartWrite=[](uint32_t a,uint8_t v) {
  XbandClock();
  XbandBank->writeRegister(a,v,true,[](uint8_t b){xband_command_feed(XbandCommands,b);XbandDrainCommands();},
   []{xband_command_uart_activity(XbandCommands,0);},[]{xband_command_uart_activity(XbandCommands,1);});
  xband_command_observe_uart_write(XbandCommands,(a>>2)&7,v);
  XbandClock();
  if(((a>>2)&7)!=0 && XbandUartTrace++<512)
   MDFN_printf("[XBAND UART] reg=%u value=%02x IER=%02x RX=%u\n",unsigned((a>>2)&7),unsigned(v),unsigned(XbandBank->ier),unsigned(XbandBank->rx.size()));
 };
 XbandDirty=false; XbandProbeReads=0; XbandUartTrace=0;
 c->OwnsRequested=XbandOwns; c->RequestedAccess=XbandAccess;
 c->FrameCompleted=XbandFrameCompleted;
 c->ResetButtonNMI=[]{xband_command_reset_button_nmi(XbandCommands);};
 c->EventHandler=XbandUpdate;
 c->Reset=XbandReset; c->Kill=XbandKill;
 c->GetNVInfo=XbandGetNV; c->GetClearNVDirty=XbandGetClearDirty;
 c->StateAction=XbandState;
 MDFN_printf("[XBAND probe] AT/card enabled; units=%d; isolated service=%s; side=%u; experimental async peer transport.\n",xband_card_units(XbandCard),servicePort&&*servicePort?servicePort:"disabled",side);
}
