// SPDX-License-Identifier: GPL-3.0-only
// Included after sh7095.inc inside MDFN_IEN_SS. Explicit diagnostic launch only;
// runs before gameplay and then terminates loading, using a fresh test profile.
static void CableCoreSelfTest()
{
 auto check=[](bool ok,const char* detail){if(!ok)throw MDFN_Error(0,"LINK_CORE_SELFTEST_FAIL: %s",detail);};
 check(xband_cable_enabled(),"enable link cable for this diagnostic");
 SH7095& c=CPU[0];
 c.SCI_Reset();
 check(c.OnChipRegRead_INLINE<uint8>(0xFFFFFE04)==0x84,"reset SSR routing");
 c.OnChipRegWrite<uint8>(0xFFFFFE01,0);
 c.OnChipRegWrite<uint8>(0xFFFFFE02,0x30);
 c.OnChipRegWrite<uint8>(0xFFFFFE03,0xA5);
 c.OnChipRegRead_INLINE<uint8>(0xFFFFFE04);
 c.OnChipRegWrite<uint8>(0xFFFFFE04,0x04);
 check(c.CableSCI.txBusy && c.CableSCI.txValue==0xA5 && !(c.CableSCI.SSR&4),"TDR/SSR starts shift");
 c.timestamp+=321;SH7095_mem_timestamp=c.timestamp;
 c.CableSCIPoll();
 check(!c.CableSCI.txBusy && (c.CableSCI.SSR&0x84)==0x84,"timed TX completion");
 c.CableSCI.receive(0x5A,SS_XbandCycles());
 check(c.OnChipRegRead_INLINE<uint8>(0xFFFFFE05)==0x5A && (c.CableSCI.SSR&0x40),"RDR read retains flag");
 c.IPRB=0x7000;c.IRL=0;c.IPRA=0;c.VCRA=0x5152;c.VCRB=0x5354;
 c.OnChipRegWrite<uint8>(0xFFFFFE02,0x70);
 uint8 vector=0;
 check(c.GetPendingInt(&vector)==7 && vector==0x52,"native RXI vector");
 c.CableSCI.receive(0x33,SS_XbandCycles());
 check(c.GetPendingInt(&vector)==7 && vector==0x51,"native ERI vector");
 c.OnChipRegRead_INLINE<uint8>(0xFFFFFE04);
 c.OnChipRegWrite<uint8>(0xFFFFFE04,0xC4);
 check(!(c.CableSCI.SSR&0x20),"observed error clear");
 c.DMAOR=1;c.DMACH[0].CHCR=1;c.DMACH[0].DRCR=1;
 c.DMACH[0].SAR=0xFFFFFE05;c.DMACH[0].DAR=0xFFFFFE03;c.DMACH[0].TCR=1;
 check(c.DMA_RunCond(0),"native RXI DMA request");
 c.DMA_DoTransfer(0);
 check(!(c.CableSCI.SSR&0x40) && c.CableSCI.txBusy && c.CableSCI.txValue==0x5A && c.DMACH[0].TCR==0,"native byte DMA RDR/TDR");
 check(!c.DMA_RunCond(0),"DMA completion blocks next transfer");
 c.DMACH[0].CHCR=1;c.DMACH[0].DRCR=2;
 check(c.DMA_RunCond(0),"native TXI DMA request");
 c.OnChipRegWrite<uint8>(0xFFFFFE02,0);
 check(!c.DMA_RunCond(0),"TE disabled DMA request");
 c.OnChipRegWrite<uint8>(0xFFFFFE02,0x80);
 check(c.GetPendingInt(&vector)==7 && vector==0x53,"native TXI vector");
 c.OnChipRegWrite<uint8>(0xFFFFFE02,4);
 check(c.GetPendingInt(&vector)==7 && vector==0x54,"native TEI vector");
 c.SCI_Reset();
 check(c.CableSCI.SSR==0x84 && !c.CableSCI.txBusy,"SCI reset");
 // Load failure is intentional: do not execute the loaded DISC or save mutated
 // diagnostic CPU state. The marker, not the process exit code, signals success.
 throw MDFN_Error(0,"LINK_CORE_SELFTEST_PASS: registers/timing/native IRQ/native byte DMA/reset");
}
