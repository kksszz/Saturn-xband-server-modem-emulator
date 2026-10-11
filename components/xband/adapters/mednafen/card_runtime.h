#ifndef XBAND_MEDNAFEN_CARD_RUNTIME_H
#define XBAND_MEDNAFEN_CARD_RUNTIME_H
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct XbandCardRuntime XbandCardRuntime;
/* Existing images are preserved. Only a missing image creates a synthetic
   100-unit test card. Absolute path required; exclusive storage lock. */
XbandCardRuntime* xband_card_create(const wchar_t* path);
void xband_card_destroy(XbandCardRuntime* card);
void xband_card_reset(XbandCardRuntime* card);
uint8_t xband_card_read(XbandCardRuntime* card,uint8_t fallback);
void xband_card_write(XbandCardRuntime* card,uint8_t value);
int xband_card_flush(XbandCardRuntime* card);
int xband_card_units(const XbandCardRuntime* card);
/* Local management only, on the emulator owner thread outside mapped I/O.
   JSON uses the shared server UI protocol; never directly debits a balance. */
const char* xband_card_report(XbandCardRuntime* card);
int xband_card_command(XbandCardRuntime* card,const char* json);
void xband_card_frame(XbandCardRuntime* card);
#ifdef __cplusplus
}
#endif
#endif
