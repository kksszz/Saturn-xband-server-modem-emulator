#ifndef XBAND_MEDNAFEN_COMMAND_RUNTIME_H
#define XBAND_MEDNAFEN_COMMAND_RUNTIME_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* C ABI only: no C++20 types, exceptions or ownership cross into Mednafen. */
typedef struct XbandCommandRuntime XbandCommandRuntime;
typedef struct XbandCardRuntime XbandCardRuntime;
typedef void (*XbandCommandLog)(const char* line);
typedef void (*XbandCommandClearReceive)(void);
XbandCommandRuntime* xband_command_create(XbandCommandLog log, XbandCommandClearReceive clear);
void xband_command_destroy(XbandCommandRuntime* runtime);
void xband_command_reset(XbandCommandRuntime* runtime);
void xband_command_feed(XbandCommandRuntime* runtime, uint8_t byte);
/* Returns -1 when empty. Owner copies replies into the UART FIFO. */
int xband_command_pop(XbandCommandRuntime* runtime);
int xband_command_failed(const XbandCommandRuntime* runtime);
/* Configure loopback service only. No sockets opened from mapped UART writes. */
int xband_command_configure_service(XbandCommandRuntime* runtime,uint16_t port,const char* subscriber);
int xband_command_configure_control(XbandCommandRuntime* runtime,uint16_t port);
int xband_command_configure_side(XbandCommandRuntime* runtime,unsigned side);
/* Borrowed card, must outlive command runtime. No external work on bus access. */
void xband_command_configure_card(XbandCommandRuntime* runtime,XbandCardRuntime* card);
void xband_command_clock(XbandCommandRuntime* runtime,uint64_t cycles,unsigned uart_pending);
int xband_command_peer(const XbandCommandRuntime* runtime);
uint8_t xband_command_tx_status(const XbandCommandRuntime* runtime);
uint8_t xband_command_interrupt(const XbandCommandRuntime* runtime,uint8_t ier,uint8_t fcr,unsigned bytes);
void xband_command_uart_activity(XbandCommandRuntime* runtime,int clear_holding);
/* Actual SMPC reset-button NMI plus later mapped (not debug) board-ID read.
   Only enqueue/observe here; host poll performs the confirmed call close. */
void xband_command_reset_button_nmi(XbandCommandRuntime* runtime);
void xband_command_board_identified(XbandCommandRuntime* runtime);
/* Observe original UART controls. A ticket-scoped game -> XOS initialization
   sequence queues compatibility teardown on host poll, never on the bus. */
void xband_command_observe_uart_write(XbandCommandRuntime* runtime,unsigned reg,uint8_t byte);
/* Read-only XOS snapshot, sampled between guest frames. No RAM writes. */
void xband_command_observe_wait(XbandCommandRuntime* runtime,uint32_t clock,uint32_t start,
    uint32_t duration,int active,int monitor,int verified);
void xband_command_poll(XbandCommandRuntime* runtime);
/* Bounded sub-frame pump from the cart event only, not mapped bus callbacks.
   Returns 1 if one pump was performed. Idle/service traffic uses host poll. */
int xband_command_live_poll(XbandCommandRuntime* runtime);
void xband_command_frame(XbandCommandRuntime* runtime);
int xband_command_carrier(const XbandCommandRuntime* runtime);
/* Host adapter entry point, called only by the Mednafen game owner thread. */
void xband_mednafen_host_poll(void);
#ifdef __cplusplus
}
#endif
#endif
