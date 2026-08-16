#ifndef DRASTIC_NX_DEBUG_LOG_H
#define DRASTIC_NX_DEBUG_LOG_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void debug_log_init(int argc, char *argv[]);
void debug_logf(const char *format, ...);
void debug_log_exception(uint32_t error_desc, uint64_t pc, uint64_t lr,
                         uint64_t sp, uint64_t far, uint32_t esr);

#ifdef __cplusplus
}
#endif

#endif
