// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#ifdef XBAND_MEDNAFEN_LINK_CABLE
int xband_cable_start(int modem_cart);
void xband_cable_stop(void);
int xband_cable_enabled(void);
int xband_cable_connected(void);
void xband_cable_poll(void);
void xband_cable_reset(void);
void xband_cable_set_cancel(int (*cancel)(void));
uint64_t xband_cable_budget(uint64_t cycle,unsigned clock_tag);
void xband_cable_send(int master,uint8_t value,uint64_t cycle);
int xband_cable_receive(int master,uint64_t cycle);
#else
static inline int xband_cable_start(int modem_cart){(void)modem_cart;return 1;}
static inline void xband_cable_stop(void){}
static inline int xband_cable_enabled(void){return 0;}
static inline int xband_cable_connected(void){return 0;}
static inline void xband_cable_poll(void){}
static inline void xband_cable_reset(void){}
static inline void xband_cable_set_cancel(int (*cancel)(void)){(void)cancel;}
static inline uint64_t xband_cable_budget(uint64_t cycle,unsigned clock_tag){(void)cycle;(void)clock_tag;return ~(uint64_t)0;}
static inline void xband_cable_send(int master,uint8_t value,uint64_t cycle){(void)master;(void)value;(void)cycle;}
static inline int xband_cable_receive(int master,uint64_t cycle){(void)master;(void)cycle;return -1;}
#endif
#ifdef __cplusplus
}
#endif
