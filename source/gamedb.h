#ifndef DRASTIC_NX_GAMEDB_H
#define DRASTIC_NX_GAMEDB_H

#include <stddef.h>

#include "drastic_config.h"

/* Resolves the launcher-owned per-game persistence root.  The host reads this
 * at launch so savePath remains authoritative even if launch.cfg is absent. */
int gamedb_get_save_path(const char *rom_path, char *out, size_t out_size);

/* Writes only the matching GameData_NDS.json object and preserves every
 * launcher-owned field. All routines return non-zero on a successful write. */
int gamedb_session_started(const char *rom_path, int *play_count, int *play_time);
int gamedb_session_finished(const char *rom_path, int play_count, int play_time,
                            const char *screenshot_path);
int gamedb_save_display_current(const char *rom_path,
                                const DrasticRuntimeConfig *config);
int gamedb_sync_display_to_all(const char *rom_path,
                               const DrasticRuntimeConfig *config);
/* The menu exposes the same two independent batch actions as nds_stub.
 * Keeping them separate avoids accidentally replacing an existing PNG mask
 * when the player only wants to share a screen layout (and vice versa). */
int gamedb_sync_layout_to_all(const char *rom_path,
                              const DrasticRuntimeConfig *config);
int gamedb_sync_overlay_to_all(const char *rom_path,
                               const DrasticRuntimeConfig *config);
int gamedb_sync_shader_to_all(const char *rom_path,
                              const DrasticRuntimeConfig *config);

#endif
