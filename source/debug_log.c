#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "debug_log.h"

#define DEBUG_DIR "sdmc:/GBAStation/debug"
#define DEBUG_LOG_PATH DEBUG_DIR "/drastic.log"
#define CRASH_LOG_PATH DEBUG_DIR "/drastic_crash.log"

static FILE *g_log;

static void ensure_debug_directory(void) {
  (void)mkdir("sdmc:/GBAStation", 0777);
  if (mkdir(DEBUG_DIR, 0777) != 0 && errno != EEXIST)
    return;
}

void debug_log_init(int argc, char *argv[]) {
  ensure_debug_directory();
  g_log = fopen(DEBUG_LOG_PATH, "wb");
  if (!g_log) return;
  fprintf(g_log, "GBAStation Drastic Vulkan host start\n");
  fprintf(g_log, "argc=%d\n", argc);
  for (int index = 0; index < argc; index++)
    fprintf(g_log, "argv[%d]=%s\n", index,
            argv && argv[index] ? argv[index] : "(null)");
  fflush(g_log);
}

void debug_logf(const char *format, ...) {
  if (!g_log) return;
  va_list arguments;
  va_start(arguments, format);
  vfprintf(g_log, format, arguments);
  va_end(arguments);
  fputc('\n', g_log);
  fflush(g_log);
}

void debug_log_exception(uint32_t error_desc, uint64_t pc, uint64_t lr,
                         uint64_t sp, uint64_t far, uint32_t esr) {
  ensure_debug_directory();
  FILE *file = fopen(CRASH_LOG_PATH, "wb");
  if (!file) return;
  fprintf(file, "GBAStation Drastic fatal CPU exception\n");
  fprintf(file, "error_desc=0x%08x\n", error_desc);
  fprintf(file, "pc=0x%016llx\n", (unsigned long long)pc);
  fprintf(file, "lr=0x%016llx\n", (unsigned long long)lr);
  fprintf(file, "sp=0x%016llx\n", (unsigned long long)sp);
  fprintf(file, "far=0x%016llx\n", (unsigned long long)far);
  fprintf(file, "esr=0x%08x\n", esr);
  fflush(file);
  fclose(file);
  debug_logf("fatal CPU exception: pc=0x%016llx lr=0x%016llx far=0x%016llx esr=0x%08x",
             (unsigned long long)pc, (unsigned long long)lr,
             (unsigned long long)far, esr);
}
