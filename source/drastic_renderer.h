#ifndef DRASTIC_NX_RENDERER_H
#define DRASTIC_NX_RENDERER_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "drastic_config.h"
#include "overlay.h"

typedef void (*DrasticCoreRenderFrame)(void *env, void *clazz,
                                       int texture_top, int texture_bottom,
                                       unsigned char swap);

bool drastic_renderer_init(const DrasticRuntimeConfig *config);
void drastic_renderer_present(const DrasticRuntimeConfig *config,
                              DrasticCoreRenderFrame core_render,
                              void *env, void *clazz,
                              const DrasticOverlayFrame *overlay,
                              bool consume_core_frame);
/* Acquire the next VI image before waiting for the core. This overlaps FIFO
 * pacing with emulation instead of serializing two display intervals. */
bool drastic_renderer_acquire_next_frame(void);
void drastic_renderer_suspend(void);
void drastic_renderer_resume(void);
void drastic_renderer_shutdown(void);
unsigned drastic_renderer_frame_count(void);
unsigned drastic_renderer_capture_count(void);
unsigned drastic_renderer_changed_capture_count(void);
bool drastic_renderer_lsfg_available(void);
bool drastic_renderer_lsfg_enabled(void);
bool drastic_renderer_lsfg_request_enabled(bool enabled);
bool drastic_renderer_lsfg_dll_available(void);
/* Stores the most recently captured native top/bottom DS frames as one
 * 256x384 PNG. This intentionally captures the emulator image, not the menu
 * overlay or the optional frame-generation output. */
bool drastic_renderer_write_screenshot(const char *path);
bool drastic_renderer_set_custom_shader(const char *relative_path,
                                        char *error, size_t error_size);
const char *drastic_renderer_last_error(void);

#endif
