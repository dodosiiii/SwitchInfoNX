#ifndef MTP_WRAPPER_H
#define MTP_WRAPPER_H

#include <switch.h>

#ifdef __cplusplus
extern "C" {
#endif

Result mtp_server_start(void);
void mtp_server_stop(void);
bool mtp_server_is_running(void);

// MTP transfer progress (updated by server thread, read by UI)
extern volatile int mtp_xfer_active;
extern volatile u64 mtp_xfer_done;
extern volatile u64 mtp_xfer_total;
extern volatile char mtp_xfer_filename[256];

#ifdef __cplusplus
}
#endif

#endif
