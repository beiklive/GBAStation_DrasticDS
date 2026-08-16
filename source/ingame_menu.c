#include <switch.h>

#include <ctype.h>
#include <dirent.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include <png.h>

#include "config.h"
#include "debug_log.h"
#include "drastic_renderer.h"
#include "gamedb.h"
#include "ingame_menu.h"
#include "jni_fake.h"
#include "opensles.h"
#include "overlay.h"
#include "prefs.h"

#define MENU_CHEAT_LIMIT 4096
#define MENU_FOLDER_LIMIT 128
#define MENU_STICK_ACTIVATION 18000
#define MENU_STICK_RELEASE 8000
#define MENU_NAV_REPEAT_DELAY_MS 340
/* 12 steps/second is deliberately the upper bound for held navigation. */
#define MENU_NAV_REPEAT_RATE_MS 85

enum MenuPage {
  MENU_MAIN,
  MENU_SAVE_STATES,
  MENU_LOAD_STATES,
  MENU_CHEATS,
  MENU_DISPLAY,
  MENU_OVERLAY_SIDEBAR,
  MENU_OVERLAY_PICKER,
  MENU_FILTER_PICKER,
  MENU_EMULATION,
  MENU_AUDIO_INPUT,
  MENU_LAYOUT_EDITOR,
  MENU_PAGE_COUNT,
};

typedef struct {
  char name[144];
  char note[320];
  int index;
  int folder;
  int custom;
  int enabled;
  int is_category;
  int parent;
  int depth;
  int expanded;
  int custom_index;
  int32_t *code_words;
  int code_word_count;
} MenuCheat;

struct DrasticIngameMenu {
  DrasticRuntimeConfig *config;
  DrasticMenuCore core;
  int *state_slot;
  enum MenuPage page;
  /* Sidebar navigation and page-content navigation are separate: moving
   * between tabs previews a page; A explicitly enters its controls. */
  int content_focused;
  int selection[MENU_PAGE_COUNT];
  int scroll[MENU_PAGE_COUNT];
  int open;
  int exit_requested;
  int redraw;
  int pending_snapshot;
  int pending_snapshot_slot;
  int confirm_delete_slot;
  int snapshot_valid;
  int32_t *snapshot_top;
  int32_t *snapshot_bottom;
  void *snapshot_top_array;
  void *snapshot_bottom_array;
  MenuCheat *cheats;
  int cheat_count;
  char folders[MENU_FOLDER_LIMIT][96];
  unsigned char folder_multi_select[MENU_FOLDER_LIMIT];
  int folder_count;
  int persisted_cheats_applied;
  int cheats_loaded;
  char cheats_rom_path[1024];
  char cheats_database_path[1024];
  char status[192];
  DrasticVideoFilter filter_backup;
  char filter_backup_shader[DRASTIC_CUSTOM_SHADER_PATH_MAX];
  DrasticCustomShaderEntry custom_shaders[128];
  int custom_shader_count;
  int filter_picker_index;
  int filter_picker_custom;
  int filter_picker_valid;
  char overlay_files[64][1024];
  unsigned char overlay_file_is_directory[64];
  int overlay_file_count;
  int overlay_picker_index;
  char overlay_picker_directory[1024];
  int overlay_preview_visible;
  int overlay_sidebar_focus;
  /* 1=layout, 2=PNG mask, 3=both.  A second explicit confirmation is
   * deliberate: these operations write every NDS entry in GameData_NDS. */
  int confirm_sync;
  u64 marquee_tick;
  u64 hint_until;
  u64 status_until;
  u64 analog_nav_direction;
  u64 held_nav_direction;
  u64 held_nav_since;
  u64 held_nav_last;
  u64 selector_repeat_direction;
  u64 selector_repeat_since;
  u64 selector_repeat_last;
  OpenSLESMicrophoneStatus microphone_status;
  int editor_screen;
};

/* Keep the menu completely opaque. The old translucent dimmer exposed the
 * live game frame in the unused Display-page area and made it appear to
 * flicker during tab changes. */
static const uint32_t COLOR_DIM = 0xff121216u;
static const uint32_t COLOR_PANEL = 0x0dffffffu;
static const uint32_t COLOR_PANEL_ALT = 0x14ffffffu;
static const uint32_t COLOR_ACCENT = 0xff51bff5u;
static const uint32_t COLOR_SELECTED = 0x33216bb3u;
static const uint32_t COLOR_TEXT = 0xfff6fbffu;
static const uint32_t COLOR_MUTED = 0xffb4c0ccu;
static const uint32_t COLOR_GOOD = 0xff63d6a5u;
static const uint32_t COLOR_WARN = 0xffffcf68u;
static const uint32_t COLOR_SWITCH_ON = 0xff3897e9u;
static const uint32_t COLOR_SWITCH_OFF = 0xff68727du;

static void leave_content_focus(DrasticIngameMenu *menu);
static void preview_main_tab(DrasticIngameMenu *menu, int tab);

static int clamp_int(int value, int minimum, int maximum) {
  if (value < minimum) return minimum;
  if (value > maximum) return maximum;
  return value;
}

static float clamp_float(float value, float minimum, float maximum) {
  if (value < minimum) return minimum;
  if (value > maximum) return maximum;
  return value;
}

static void reset_analog_navigation(DrasticIngameMenu *menu) {
  menu->analog_nav_direction = 0;
  menu->held_nav_direction = 0;
  menu->held_nav_since = 0;
  menu->held_nav_last = 0;
  menu->selector_repeat_direction = 0;
  menu->selector_repeat_since = 0;
  menu->selector_repeat_last = 0;
}

static u64 analog_navigation_direction(const DrasticIngameMenu *menu,
                                       HidAnalogStickState stick) {
  const int x = abs(stick.x);
  const int y = abs(stick.y);
  if (x >= MENU_STICK_ACTIVATION || y >= MENU_STICK_ACTIVATION) {
    if (y >= x)
      return stick.y > 0 ? HidNpadButton_Up : HidNpadButton_Down;
    return stick.x > 0 ? HidNpadButton_Right : HidNpadButton_Left;
  }

  /* Keep the current direction latched until the stick returns close to its
   * centre.  This hysteresis prevents a slightly noisy stick from producing
   * extra menu steps around the activation threshold. */
  switch (menu->analog_nav_direction) {
    case HidNpadButton_Up:
      return stick.y > MENU_STICK_RELEASE ? HidNpadButton_Up : 0;
    case HidNpadButton_Down:
      return stick.y < -MENU_STICK_RELEASE ? HidNpadButton_Down : 0;
    case HidNpadButton_Left:
      return stick.x < -MENU_STICK_RELEASE ? HidNpadButton_Left : 0;
    case HidNpadButton_Right:
      return stick.x > MENU_STICK_RELEASE ? HidNpadButton_Right : 0;
    default: return 0;
  }
}

static int ui_width(void) { return overlay_width(); }

static int ui_height(void) { return overlay_height(); }

static int ui_is_portrait(void) { return ui_height() > ui_width(); }

static int menu_sidebar_x(void) { return ui_is_portrait() ? 30 : 48; }
static int menu_sidebar_width(void) { return ui_is_portrait() ? 250 : 336; }
static int menu_content_x(void) { return ui_is_portrait() ? 300 : 432; }
static int menu_content_y(void) { return 110; }
static int menu_content_width(void) {
  return ui_width() - menu_content_x() - (ui_is_portrait() ? 36 : 58);
}

static u64 held_navigation_direction(DrasticIngameMenu *menu, u64 held,
                                     HidAnalogStickState stick) {
  const u64 dpad = held & (HidNpadButton_Up | HidNpadButton_Down |
                           HidNpadButton_Left | HidNpadButton_Right);
  if (dpad & HidNpadButton_Up) return HidNpadButton_Up;
  if (dpad & HidNpadButton_Down) return HidNpadButton_Down;
  if (dpad & HidNpadButton_Left) return HidNpadButton_Left;
  if (dpad & HidNpadButton_Right) return HidNpadButton_Right;
  const u64 direction = analog_navigation_direction(menu, stick);
  menu->analog_nav_direction = direction;
  return direction;
}

static u64 held_navigation_pressed(DrasticIngameMenu *menu, u64 held,
                                   HidAnalogStickState stick) {
  const u64 direction = held_navigation_direction(menu, held, stick);
  const u64 now = armGetSystemTick();
  if (direction != menu->held_nav_direction) {
    menu->held_nav_direction = direction;
    menu->held_nav_since = now;
    menu->held_nav_last = now;
    return direction;
  }
  if (!direction) return 0;
  const u64 frequency = armGetSystemTickFreq();
  if (!frequency) return 0;
  const u64 delay = frequency * MENU_NAV_REPEAT_DELAY_MS / 1000u;
  const u64 rate = frequency * MENU_NAV_REPEAT_RATE_MS / 1000u;
  if (now - menu->held_nav_since < delay ||
      now - menu->held_nav_last < rate)
    return 0;
  menu->held_nav_last = now;
  return direction;
}

static u64 selector_repeat_pressed(DrasticIngameMenu *menu, u64 held) {
  const u64 direction = (held & HidNpadButton_L) ? HidNpadButton_L :
                        (held & HidNpadButton_R) ? HidNpadButton_R : 0;
  const u64 now = armGetSystemTick();
  if (direction != menu->selector_repeat_direction) {
    menu->selector_repeat_direction = direction;
    menu->selector_repeat_since = now;
    menu->selector_repeat_last = now;
    return direction;
  }
  if (!direction) return 0;
  const u64 frequency = armGetSystemTickFreq();
  if (!frequency) return 0;
  const u64 delay = frequency * MENU_NAV_REPEAT_DELAY_MS / 1000u;
  const u64 rate = frequency * MENU_NAV_REPEAT_RATE_MS / 1000u;
  if (now - menu->selector_repeat_since < delay ||
      now - menu->selector_repeat_last < rate)
    return 0;
  menu->selector_repeat_last = now;
  return direction;
}
static int menu_content_height(void) { return ui_height() - 200; }

static void set_status(DrasticIngameMenu *menu, const char *status) {
  snprintf(menu->status, sizeof(menu->status), "%s", status ? status : "");
  const u64 frequency = armGetSystemTickFreq();
  menu->status_until = frequency ? armGetSystemTick() + frequency * 3 : 0;
  menu->redraw = 1;
}

static void save_bool(const char *key, int value) {
  prefs_set_bool(key, value != 0);
  prefs_save_runtime_key(key);
}

static void save_int(const char *key, int value) {
  prefs_set_int(key, value);
  prefs_save_runtime_key(key);
}

static void save_string(const char *key, const char *value) {
  prefs_set_string(key, value);
  prefs_save_runtime_key(key);
}

static void save_float(const char *key, float value) {
  prefs_set_float(key, value);
  prefs_save_runtime_key(key);
}

static void apply_core_config(DrasticIngameMenu *menu) {
  menu->config->core_config = drastic_config_build_core_config();
  if (menu->core.apply_config)
    menu->core.apply_config(menu->core.env, menu->core.clazz,
                            (DrasticJLong)menu->config->core_config);
}

static void copy_java_bytes(void *array, char *destination, size_t size) {
  if (!destination || !size) return;
  destination[0] = '\0';
  const uint8_t *data = jni_byte_array_data(array);
  int length = jni_byte_array_length(array);
  if (data && length > 0) {
    while (length > 0 && !data[length - 1]) length--;
    if ((size_t)length >= size) length = (int)size - 1;
    memcpy(destination, data, (size_t)length);
    destination[length] = '\0';
  }
  jni_release_byte_array(array);
}

static void refresh_snapshot_for_slot(DrasticIngameMenu *menu, int slot) {
  if (!menu->core.get_snapshots || !menu->snapshot_top ||
      !menu->snapshot_bottom) {
    menu->snapshot_valid = 0;
    return;
  }
  memset(menu->snapshot_top, 0, 256 * 192 * sizeof(*menu->snapshot_top));
  memset(menu->snapshot_bottom, 0, 256 * 192 * sizeof(*menu->snapshot_bottom));
  menu->core.get_snapshots(menu->core.env, menu->core.clazz,
                           slot, menu->snapshot_top_array,
                           menu->snapshot_bottom_array);
  menu->snapshot_valid = 0;
  for (int index = 0; index < 256 * 192; index++) {
    if (menu->snapshot_top[index] || menu->snapshot_bottom[index]) {
      menu->snapshot_valid = 1;
      break;
    }
  }
  menu->redraw = 1;
}

static int state_filename_matches_slot(const char *name, int slot) {
  if (!name) return 0;
  char suffix[24];
  snprintf(suffix, sizeof(suffix), "_%d.dss", slot);
  const size_t name_length = strlen(name);
  const size_t suffix_length = strlen(suffix);
  return name_length > suffix_length &&
         !strcmp(name + name_length - suffix_length, suffix);
}

static const char *state_directory(const DrasticIngameMenu *menu) {
  return menu && menu->config && menu->config->save_path[0]
      ? menu->config->save_path : SAVESTATES_DIR;
}

/* The core owns state filenames. Derive a preview name from the actual .dss
 * file so per-game save roots never need to guess the ROM title. */
static int state_preview_path_for_slot(const DrasticIngameMenu *menu, int slot,
                                       char *path, size_t path_size) {
  if (!path || !path_size) return 0;
  path[0] = '\0';
  const char *directory_path = state_directory(menu);
  DIR *directory = opendir(directory_path);
  struct dirent *entry;
  while (directory && (entry = readdir(directory)) != NULL) {
    if (!state_filename_matches_slot(entry->d_name, slot)) continue;
    const size_t name_length = strlen(entry->d_name);
    const int written = snprintf(path, path_size, "%s/%.*s.png",
                                 directory_path, (int)(name_length - 4),
                                 entry->d_name);
    closedir(directory);
    return written > 0 && (size_t)written < path_size;
  }
  if (directory) closedir(directory);
  return 0;
}

static int snapshot_uses_argb(const int32_t *pixels, int pixel_count) {
  if (!pixels || pixel_count <= 0) return 0;
  for (int index = 0; index < pixel_count; index += 257) {
    const uint32_t pixel = (uint32_t)pixels[index];
    if ((pixel & 0xffff0000u) && pixel != 0xffffffffu) return 1;
  }
  return 0;
}

static void snapshot_to_rgba(const int32_t *source, uint8_t *destination,
                             int pixel_count) {
  const int argb = snapshot_uses_argb(source, pixel_count);
  for (int index = 0; index < pixel_count; index++) {
    uint32_t pixel = (uint32_t)source[index];
    if (!argb) {
      const uint16_t rgb565 = (uint16_t)pixel;
      const unsigned red = (rgb565 >> 11) & 31;
      const unsigned green = (rgb565 >> 5) & 63;
      const unsigned blue = rgb565 & 31;
      pixel = 0xff000000u | ((red * 255 / 31) << 16) |
              ((green * 255 / 63) << 8) | (blue * 255 / 31);
    } else if (!(pixel & 0xff000000u)) {
      pixel |= 0xff000000u;
    }
    uint8_t *rgba = destination + (size_t)index * 4;
    rgba[0] = (uint8_t)(pixel >> 16);
    rgba[1] = (uint8_t)(pixel >> 8);
    rgba[2] = (uint8_t)pixel;
    rgba[3] = (uint8_t)(pixel >> 24);
  }
}

static int write_state_preview_png(const DrasticIngameMenu *menu, int slot) {
  if (!menu || !menu->snapshot_valid) return 0;
  char path[1200];
  if (!state_preview_path_for_slot(menu, slot, path, sizeof(path))) return 0;

  const size_t screen_pixels = 256u * 192u;
  uint8_t *rgba = malloc(screen_pixels * 2u * 4u);
  if (!rgba) return 0;
  snapshot_to_rgba(menu->snapshot_top, rgba, (int)screen_pixels);
  snapshot_to_rgba(menu->snapshot_bottom, rgba + screen_pixels * 4u,
                   (int)screen_pixels);
  png_image image;
  memset(&image, 0, sizeof(image));
  image.version = PNG_IMAGE_VERSION;
  image.width = 256;
  image.height = 384;
  image.format = PNG_FORMAT_RGBA;
  const int written = png_image_write_to_file(&image, path, 0, rgba, 0, NULL);
  free(rgba);
  if (written) debug_logf("state preview saved slot=%d path=%s", slot, path);
  else debug_logf("state preview write failed slot=%d path=%s", slot, path);
  return written;
}

void drastic_menu_note_state_save(DrasticIngameMenu *menu, int slot) {
  if (!menu || slot < 0 || slot > 9) return;
  menu->pending_snapshot = 1;
  menu->pending_snapshot_slot = slot;
}

void drastic_menu_poll(DrasticIngameMenu *menu) {
  if (!menu || !menu->pending_snapshot) return;
  if (menu->core.is_saving &&
      menu->core.is_saving(menu->core.env, menu->core.clazz)) return;
  const int slot = menu->pending_snapshot_slot;
  menu->pending_snapshot = 0;
  menu->pending_snapshot_slot = -1;
  refresh_snapshot_for_slot(menu, slot);
  set_status(menu, write_state_preview_png(menu, slot)
                       ? "即时存档完成，已生成预览图"
                       : "即时存档完成，但预览图写入失败");
}

static int delete_matching_state(DrasticIngameMenu *menu, int slot) {
  if (!menu->snapshot_valid || !menu->core.get_snapshots_direct) return 0;
  void *top_array = jni_make_int_array(256 * 192);
  void *bottom_array = jni_make_int_array(256 * 192);
  int32_t *top = jni_int_array_data(top_array);
  int32_t *bottom = jni_int_array_data(bottom_array);
  if (!top || !bottom) {
    jni_release_int_array(top_array);
    jni_release_int_array(bottom_array);
    return 0;
  }
  int matches = 0;
  char matched_path[1200] = {0};
  const char *save_directory = state_directory(menu);
  DIR *directory = opendir(save_directory);
  struct dirent *entry;
  while (directory && (entry = readdir(directory))) {
    if (!state_filename_matches_slot(entry->d_name, slot)) continue;
    char path[1200];
    snprintf(path, sizeof(path), "%s/%s", save_directory, entry->d_name);
    memset(top, 0, 256 * 192 * sizeof(*top));
    memset(bottom, 0, 256 * 192 * sizeof(*bottom));
    void *java_path = jni_make_string(path);
    menu->core.get_snapshots_direct(menu->core.env, menu->core.clazz,
                                    java_path, top_array, bottom_array);
    jni_release_string(java_path);
    if (memcmp(top, menu->snapshot_top, 256 * 192 * sizeof(*top)) ||
        memcmp(bottom, menu->snapshot_bottom, 256 * 192 * sizeof(*bottom)))
      continue;
    matches++;
    snprintf(matched_path, sizeof(matched_path), "%s", path);
  }
  if (directory) closedir(directory);
  const int deleted = matches == 1 && remove(matched_path) == 0;
  if (deleted) {
    const size_t state_length = strlen(matched_path);
    if (state_length > 4) {
      char preview_path[1200];
      snprintf(preview_path, sizeof(preview_path), "%.*s.png",
               (int)(state_length - 4), matched_path);
      (void)remove(preview_path);
    }
  }
  jni_release_int_array(top_array);
  jni_release_int_array(bottom_array);
  return deleted;
}

static void free_cheats(DrasticIngameMenu *menu) {
  for (int index = 0; menu && index < menu->cheat_count; index++)
    free(menu->cheats[index].code_words);
  free(menu->cheats);
  menu->cheats = NULL;
  menu->cheat_count = 0;
  menu->folder_count = 0;
}

static void refresh_cheats(DrasticIngameMenu *menu);
static int set_database_cheat_enabled(DrasticIngameMenu *menu,
                                      MenuCheat *cheat, int enabled);

static void invalidate_cheat_cache(DrasticIngameMenu *menu) {
  if (!menu) return;
  menu->cheats_loaded = 0;
  menu->cheats_rom_path[0] = '\0';
  menu->cheats_database_path[0] = '\0';
}

/* DraStic retains its custom-cheat list independently of the database list.
 * In particular, records registered by an earlier game (or by the old broken
 * repeated-registration path) survive into the next launch.  The R4 database
 * selection below is our source of truth, so clear that stale list before
 * restoring the selected records for the current ROM. */
static void clear_stale_custom_cheats(DrasticIngameMenu *menu) {
  if (!menu || !menu->core.get_custom_cheat_count ||
      !menu->core.remove_custom_cheat)
    return;
  const int before = clamp_int(
      menu->core.get_custom_cheat_count(menu->core.env, menu->core.clazz),
      0, MENU_CHEAT_LIMIT);
  for (int index = before - 1; index >= 0; index--)
    menu->core.remove_custom_cheat(menu->core.env, menu->core.clazz, index);
  const int after = clamp_int(
      menu->core.get_custom_cheat_count(menu->core.env, menu->core.clazz),
      0, MENU_CHEAT_LIMIT);
  if (before || after)
    debug_logf("cheats custom reset count=%d->%d", before, after);
}

static int hex_digit_value(int character) {
  if (character >= '0' && character <= '9') return character - '0';
  character = toupper((unsigned char)character);
  return character >= 'A' && character <= 'F' ? character - 'A' + 10 : -1;
}

static int enabled_list_contains(const char *list, int wanted) {
  if (!list) return 0;
  if (!strncmp(list, "hex:", 4)) {
    const char *bitmap = list + 4;
    const size_t digit = (unsigned)wanted >> 2;
    if (digit >= strlen(bitmap)) return 0;
    const int value = hex_digit_value((unsigned char)bitmap[digit]);
    return value >= 0 && (value & (1 << (wanted & 3)));
  }
  const char *cursor = list;
  while (*cursor) {
    while (*cursor == ',' || isspace((unsigned char)*cursor)) cursor++;
    if (!*cursor) break;
    char *end = NULL;
    const long value = strtol(cursor, &end, 10);
    if (end == cursor) {
      while (*cursor && *cursor != ',') cursor++;
      continue;
    }
    if (value == wanted) return 1;
    cursor = end;
    while (*cursor && *cursor != ',') cursor++;
  }
  return 0;
}

void drastic_menu_apply_persisted_cheats(DrasticIngameMenu *menu) {
  if (!menu || menu->persisted_cheats_applied) return;
  menu->persisted_cheats_applied = 1;
  clear_stale_custom_cheats(menu);
  if (!prefs_contains("Wrapper/EnabledDatabaseCheats")) return;
  const char *enabled = prefs_get_string("Wrapper/EnabledDatabaseCheats", "");
  const int count = clamp_int(
      menu->core.get_cheat_count
          ? menu->core.get_cheat_count(menu->core.env, menu->core.clazz) : 0,
      0, MENU_CHEAT_LIMIT);
  debug_logf("cheats restore core_count=%d enabled=%s", count, enabled);
  if (count > 0 && menu->core.set_cheat_enabled) {
    for (int index = 0; index < count; index++)
      menu->core.set_cheat_enabled(menu->core.env, menu->core.clazz, index,
                                   enabled_list_contains(enabled, index));
  } else {
    /* This core build exposes the custom-cheat JNI API but does not populate
     * its database list.  Inject the R4 records selected in our menu instead
     * of treating their parsed indices as nonexistent core indices. */
    refresh_cheats(menu);
    int injected = 0;
    for (int index = 0; index < menu->cheat_count; index++) {
      MenuCheat *cheat = &menu->cheats[index];
      if (!cheat->is_category && cheat->enabled &&
          set_database_cheat_enabled(menu, cheat, 1))
        injected++;
    }
    debug_logf("cheats restore injected=%d parsed=%d", injected,
               menu->cheat_count);
  }
  if (menu->core.update_cheats)
    menu->core.update_cheats(menu->core.env, menu->core.clazz, 1);
}

static void persist_database_cheats(DrasticIngameMenu *menu) {
  static const char hexadecimal[] = "0123456789abcdef";
  unsigned char bitmap[MENU_CHEAT_LIMIT / 8] = {0};
  int highest = -1;
  for (int index = 0; index < menu->cheat_count; index++) {
    const MenuCheat *cheat = &menu->cheats[index];
    if (cheat->custom || !cheat->enabled) continue;
    if ((unsigned)cheat->index >= MENU_CHEAT_LIMIT) continue;
    bitmap[(unsigned)cheat->index >> 3] |=
        (unsigned char)(1u << (cheat->index & 7));
    if (cheat->index > highest) highest = cheat->index;
  }
  char enabled[5 + MENU_CHEAT_LIMIT / 4] = "hex:";
  const int digits = highest < 0 ? 0 : highest / 4 + 1;
  for (int digit = 0; digit < digits; digit++)
    enabled[4 + digit] = hexadecimal[
        (bitmap[digit >> 1] >> ((digit & 1) * 4)) & 0x0f];
  enabled[4 + digits] = '\0';
  save_string("Wrapper/EnabledDatabaseCheats", enabled);
}

static uint32_t cheat_le32(const unsigned char *data) {
  return (uint32_t)data[0] | (uint32_t)data[1] << 8 |
         (uint32_t)data[2] << 16 | (uint32_t)data[3] << 24;
}

static uint32_t cheat_crc32(const unsigned char *data, size_t size) {
  uint32_t value = 0xffffffffu;
  for (size_t index = 0; index < size; index++) {
    value ^= data[index];
    for (int bit = 0; bit < 8; bit++)
      value = (value >> 1) ^ (UINT32_C(0xedb88320) & -(int)(value & 1));
  }
  return value ^ 0xffffffffu;
}

static int cheat_string(const unsigned char *data, size_t size, size_t *pos,
                        char *out, size_t out_size) {
  size_t used = 0;
  while (*pos < size) {
    const unsigned char c = data[(*pos)++];
    if (!c) { if (out_size) out[used] = '\0'; return 1; }
    if (used + 1 < out_size) out[used++] = (char)c;
  }
  return 0;
}

static void configured_cheat_file(const DrasticIngameMenu *menu,
                                  char *path, size_t path_size) {
  const char *configured = menu && menu->config ? menu->config->cheat_path : "";
  const char *extension = configured[0] ? strrchr(configured, '.') : NULL;
  if (configured[0] && extension && !strcasecmp(extension, ".dat"))
    snprintf(path, path_size, "%s", configured);
  else if (configured[0])
    snprintf(path, path_size, "%s/usrcheat.dat", configured);
  else
    snprintf(path, path_size, "%s", CHEAT_DATABASE_PATH);
}

/* R4 usrcheat.dat parser copied in behaviour from nds_stub.  The UI no
 * longer trusts the core's flattened Java list: folders, descriptions and
 * code order are all taken from the GameDB-selected usrcheat.dat itself. */
static void refresh_cheats(DrasticIngameMenu *menu) {
  char path[1024];
  configured_cheat_file(menu, path, sizeof(path));
  if (menu->cheats_loaded &&
      !strcmp(menu->cheats_rom_path, menu->config->rom_path) &&
      !strcmp(menu->cheats_database_path, path))
    return;
  free_cheats(menu);
  menu->cheats_loaded = 1;
  snprintf(menu->cheats_rom_path, sizeof(menu->cheats_rom_path), "%s",
           menu->config->rom_path);
  snprintf(menu->cheats_database_path, sizeof(menu->cheats_database_path),
           "%s", path);
  FILE *file = fopen(path, "rb");
  FILE *rom = fopen(menu->config->rom_path, "rb");
  unsigned char rom_header[512];
  if (!file || !rom || fread(rom_header, 1, sizeof(rom_header), rom) != sizeof(rom_header)) {
    if (file) fclose(file);
    if (rom) fclose(rom);
    set_status(menu, "无法读取 usrcheat.dat 或 ROM 信息"); return;
  }
  fclose(rom);
  if (fseek(file, 0, SEEK_END)) { fclose(file); return; }
  long file_length = ftell(file);
  if (file_length < 0x110 || fseek(file, 0, SEEK_SET)) { fclose(file); return; }
  unsigned char *data = malloc((size_t)file_length);
  if (!data || fread(data, 1, (size_t)file_length, file) != (size_t)file_length) {
    free(data); fclose(file); set_status(menu, "无法载入 usrcheat.dat"); return;
  }
  fclose(file);
  const size_t size = (size_t)file_length;
  if (memcmp(data, "R4 CheatCode", 12)) { free(data); set_status(menu, "usrcheat.dat 格式无效"); return; }
  const uint32_t game_code = cheat_le32(rom_header + 12);
  const uint32_t checksum = cheat_crc32(rom_header, sizeof(rom_header));
  size_t matched = 0, fallback = 0;
  for (size_t pos = 0x100; pos + 16 <= size; pos += 16) {
    const uint32_t code = cheat_le32(data + pos);
    const uint32_t sum = cheat_le32(data + pos + 4);
    const uint32_t offset = cheat_le32(data + pos + 8);
    if (!code) break;
    if (code == game_code && offset >= 0x100 && offset < size) {
      if (!fallback) fallback = offset;
      if (sum == checksum) { matched = offset; break; }
    }
  }
  size_t pos = matched ? matched : fallback;
  if (!pos) { free(data); set_status(menu, "usrcheat.dat 未找到当前游戏"); return; }
  char game_name[160] = {0};
  if (!cheat_string(data, size, &pos, game_name, sizeof(game_name))) { free(data); return; }
  pos = (pos + 3u) & ~3u;
  if (pos + 36 > size) { free(data); return; }
  const uint32_t item_count = cheat_le32(data + pos) & 0x00ffffffu;
  pos += 36;
  menu->cheats = calloc(MENU_CHEAT_LIMIT, sizeof(*menu->cheats));
  if (!menu->cheats) { free(data); set_status(menu, "金手指内存不足"); return; }
  int stack_index[32], stack_remaining[32], stack_size = 0;
  int code_index = 0;
  for (uint32_t item = 0; item < item_count && pos + 4 <= size &&
                          menu->cheat_count < MENU_CHEAT_LIMIT; item++) {
    const uint32_t flags = cheat_le32(data + pos); pos += 4;
    const uint32_t length = flags & 0x00ffffffu;
    const int category = (flags & (1u << 28)) != 0;
    char name[144] = {0}, note[320] = {0};
    if (!cheat_string(data, size, &pos, name, sizeof(name)) ||
        !cheat_string(data, size, &pos, note, sizeof(note))) break;
    pos = (pos + 3u) & ~3u;
    const int parent = stack_size ? stack_index[stack_size - 1] : -1;
    const int depth = stack_size;
    if (category) {
      MenuCheat *entry = &menu->cheats[menu->cheat_count++];
      snprintf(entry->name, sizeof(entry->name), "%s", name[0] ? name : note);
      entry->is_category = 1; entry->parent = parent; entry->depth = depth;
      /* A database can contain many large folders.  Like nds_stub, open
       * with every folder collapsed and let the player opt into expansion. */
      entry->expanded = 0;
      if (stack_size) {
        --stack_remaining[stack_size - 1];
        while (stack_size && stack_remaining[stack_size - 1] <= 0) --stack_size;
      }
      if (stack_size < 32 && length > 0) {
        stack_index[stack_size] = menu->cheat_count - 1;
        stack_remaining[stack_size++] = (int)length;
      }
    } else {
      if (pos + 4 > size) break;
      const uint32_t words = cheat_le32(data + pos); pos += 4;
      if (words > 0x100000u || (words & 1u) || pos + (size_t)words * 4 > size) break;
      MenuCheat *entry = &menu->cheats[menu->cheat_count++];
      snprintf(entry->name, sizeof(entry->name), "%s", name[0] ? name : note);
      snprintf(entry->note, sizeof(entry->note), "%s", note);
      entry->index = code_index++; entry->parent = parent; entry->depth = depth;
      entry->custom_index = -1;
      entry->code_word_count = (int)words;
      entry->code_words = malloc((size_t)words * sizeof(*entry->code_words));
      if (!entry->code_words) { menu->cheat_count--; break; }
      for (uint32_t word = 0; word < words; word++)
        entry->code_words[word] = (int32_t)cheat_le32(data + pos + word * 4);
      const int core_count = menu->core.get_cheat_count
          ? menu->core.get_cheat_count(menu->core.env, menu->core.clazz) : 0;
      entry->enabled = core_count > 0 && menu->core.get_cheat_enabled
          ? menu->core.get_cheat_enabled(menu->core.env, menu->core.clazz,
                                         entry->index)
          : enabled_list_contains(
                prefs_get_string("Wrapper/EnabledDatabaseCheats", ""),
                entry->index);
      pos += (size_t)words * 4;
    }
    if (!category && stack_size) {
      --stack_remaining[stack_size - 1];
      while (stack_size && stack_remaining[stack_size - 1] <= 0) --stack_size;
    }
  }
  free(data);
  if (!menu->cheat_count) { free_cheats(menu); set_status(menu, "当前游戏没有可用金手指"); }
  debug_logf("cheats database=%s parsed=%d core_count=%d", path,
             menu->cheat_count,
             menu->core.get_cheat_count
                 ? menu->core.get_cheat_count(menu->core.env, menu->core.clazz)
                 : -1);
  menu->selection[MENU_CHEATS] = 0;
  menu->redraw = 1;
}

static void refresh_snapshot(DrasticIngameMenu *menu) {
  refresh_snapshot_for_slot(menu, *menu->state_slot);
}

static int set_database_cheat_enabled(DrasticIngameMenu *menu,
                                      MenuCheat *cheat, int enabled) {
  if (!menu || !cheat || cheat->is_category) return 0;
  const int core_count = menu->core.get_cheat_count
      ? menu->core.get_cheat_count(menu->core.env, menu->core.clazz) : 0;
  if (core_count > 0 && menu->core.set_cheat_enabled) {
    menu->core.set_cheat_enabled(menu->core.env, menu->core.clazz,
                                 cheat->index, enabled);
    cheat->enabled = enabled;
    return 1;
  }
  if (!menu->core.add_custom_cheat || !menu->core.set_custom_cheat_enabled ||
      !cheat->code_words || cheat->code_word_count < 2)
    return 0;
  if (enabled && cheat->custom_index < 0) {
    void *array = jni_make_int_array(cheat->code_word_count);
    int32_t *words = jni_int_array_data(array);
    if (!words) return 0;
    memcpy(words, cheat->code_words,
           (size_t)cheat->code_word_count * sizeof(*words));
    void *name = jni_make_string(cheat->name);
    const int before = menu->core.get_custom_cheat_count
        ? menu->core.get_custom_cheat_count(menu->core.env, menu->core.clazz) : 0;
    const int added = menu->core.add_custom_cheat(
        menu->core.env, menu->core.clazz, name, array,
        cheat->code_word_count, 1);
    jni_release_string(name);
    jni_release_int_array(array);
    /* DraStic's JNI method returns 0 even when it accepted the record.  The
     * only reliable acknowledgement is that its custom-cheat list grew.
     * Treating that return value as a boolean left custom_index at -1, so the
     * UI appeared off while every click registered another, shifted record. */
    const int after = menu->core.get_custom_cheat_count
        ? menu->core.get_custom_cheat_count(menu->core.env, menu->core.clazz)
        : before;
    debug_logf("cheats custom add database_index=%d words=%d result=%d slot=%d count=%d->%d",
               cheat->index, cheat->code_word_count, added, before, before,
               after);
    if (after <= before) return 0;
    cheat->custom_index = before;
  }
  if (cheat->custom_index < 0) return !enabled;
  menu->core.set_custom_cheat_enabled(menu->core.env, menu->core.clazz,
                                       cheat->custom_index, enabled);
  cheat->enabled = enabled;
  const int reported = menu->core.get_custom_cheat_enabled
      ? menu->core.get_custom_cheat_enabled(menu->core.env, menu->core.clazz,
                                             cheat->custom_index) != 0
      : -1;
  debug_logf("cheats custom state database_index=%d slot=%d requested=%d reported=%d",
             cheat->index, cheat->custom_index, enabled, reported);
  return 1;
}

static int prompt_keyboard(const char *header, const char *guide,
                           const char *initial, char *output,
                           size_t output_size, int multiline) {
  if (!output || output_size < 2) return 0;
  SwkbdConfig keyboard;
  Result result = swkbdCreate(&keyboard, 0);
  if (R_FAILED(result)) return 0;
  swkbdConfigMakePresetDefault(&keyboard);
  swkbdConfigSetHeaderText(&keyboard, header ? header : "Drastic DS");
  if (guide) swkbdConfigSetGuideText(&keyboard, guide);
  if (initial && *initial) swkbdConfigSetInitialText(&keyboard, initial);
  swkbdConfigSetStringLenMax(&keyboard, (u32)output_size - 1);
  swkbdConfigSetReturnButtonFlag(&keyboard, multiline ? 1 : 0);
  result = swkbdShow(&keyboard, output, output_size);
  swkbdClose(&keyboard);
  return R_SUCCEEDED(result) && output[0];
}

static int parse_cheat_words(const char *text, int32_t *words, int capacity) {
  int count = 0;
  const char *cursor = text;
  while (cursor && *cursor) {
    while (*cursor && (isspace((unsigned char)*cursor) || *cursor == ',' ||
                       *cursor == ';' || *cursor == ':')) cursor++;
    if (!*cursor) break;
    if (cursor[0] == '0' && (cursor[1] == 'x' || cursor[1] == 'X'))
      cursor += 2;
    const char *start = cursor;
    unsigned long value = 0;
    int digits = 0;
    while (isxdigit((unsigned char)*cursor) && digits < 8) {
      const int character = *cursor++;
      value <<= 4;
      value |= character >= '0' && character <= '9' ? character - '0'
          : (character & ~32) - 'A' + 10;
      digits++;
    }
    if (!digits || isxdigit((unsigned char)*cursor) || cursor == start ||
        count >= capacity) return -1;
    words[count++] = (int32_t)value;
  }
  return count >= 2 && !(count & 1) ? count : -1;
}

static void add_custom_cheat(DrasticIngameMenu *menu) {
  if (!menu->core.add_custom_cheat) {
    set_status(menu, "当前核心不支持自定义金手指");
    return;
  }
  char name[96] = {0};
  char codes[4096] = {0};
  if (!prompt_keyboard("New custom cheat", "Enter a name", "", name,
                       sizeof(name), 0)) {
    menu->redraw = 1;
    return;
  }
  if (!prompt_keyboard("Action Replay code",
                       "Enter hexadecimal address/value pairs", "", codes,
                       sizeof(codes), 1)) {
    menu->redraw = 1;
    return;
  }
  int32_t words[512];
  const int count = parse_cheat_words(codes, words,
                                      (int)(sizeof(words) / sizeof(*words)));
  if (count < 0) {
    set_status(menu, "代码无效：请使用 8 位地址/数值对");
    return;
  }
  void *array = jni_make_int_array(count);
  int32_t *data = jni_int_array_data(array);
  if (!data) {
    set_status(menu, "无法分配金手指代码内存");
    return;
  }
  memcpy(data, words, (size_t)count * sizeof(*data));
  void *java_name = jni_make_string(name);
  const int result = menu->core.add_custom_cheat(
      menu->core.env, menu->core.clazz, java_name, array, count, 1);
  jni_release_string(java_name);
  jni_release_int_array(array);
  if (menu->core.update_cheats)
    menu->core.update_cheats(menu->core.env, menu->core.clazz, 1);
  invalidate_cheat_cache(menu);
  refresh_cheats(menu);
  set_status(menu, result ? "已添加自定义金手指" :
                            "DraStic 拒绝了此自定义金手指");
}

static int active_main_tab(const DrasticIngameMenu *menu) {
  switch (menu->page) {
    case MENU_SAVE_STATES: return 1;
    case MENU_LOAD_STATES: return 2;
    case MENU_CHEATS: return 3;
    case MENU_DISPLAY: return 4;
    case MENU_OVERLAY_SIDEBAR: return 4;
    case MENU_OVERLAY_PICKER: return 4;
    case MENU_EMULATION: return 5;
    default: return menu->selection[MENU_MAIN];
  }
}

enum MenuIcon {
  MENU_ICON_RESUME,
  MENU_ICON_SAVE,
  MENU_ICON_LOAD,
  MENU_ICON_CHEAT,
  MENU_ICON_DISPLAY,
  MENU_ICON_EMULATION,
  MENU_ICON_RESET,
  MENU_ICON_EXIT,
  MENU_ICON_ROW,
};

/* Compact line icons keep the menu usable even when optional external
 * Material Icons resources have not been installed yet. */
static void draw_menu_icon(int x, int y, int size, uint32_t color,
                           enum MenuIcon icon) {
  const int s = size < 10 ? 10 : size;
  const int t = s >= 20 ? 3 : 2;
  const int mid = s / 2;
  switch (icon) {
    case MENU_ICON_RESUME:
      overlay_fill_rect(x + s / 3, y + t, t, s - t * 2, color);
      for (int row = 0; row < s / 2; row++)
        overlay_fill_rect(x + s / 3 + t, y + s / 4 + row,
                          s / 2 - row, 1, color);
      break;
    case MENU_ICON_SAVE:
      overlay_border_rect(x + 2, y + 2, s - 4, s - 4, t, color);
      overlay_fill_rect(x + s / 4, y + 3, s / 2, s / 4, color);
      overlay_border_rect(x + s / 4, y + s / 2, s / 2, s / 3, 1, color);
      break;
    case MENU_ICON_LOAD:
      overlay_border_rect(x + 2, y + 2, s - 4, s - 4, t, color);
      overlay_fill_rect(x + mid - t / 2, y + 4, t, s / 2, color);
      overlay_fill_rect(x + s / 4, y + s / 2 - t, s / 2, t, color);
      overlay_fill_rect(x + s / 4, y + s / 2 - t, t, s / 4, color);
      overlay_fill_rect(x + s - s / 4 - t, y + s / 2 - t, t, s / 4, color);
      break;
    case MENU_ICON_CHEAT:
      overlay_fill_rect(x + mid - t / 2, y + 2, t, s - 4, color);
      overlay_fill_rect(x + 2, y + mid - t / 2, s - 4, t, color);
      break;
    case MENU_ICON_DISPLAY:
      overlay_border_rect(x + 1, y + 2, s - 2, s - 7, t, color);
      overlay_fill_rect(x + s / 4, y + s - 4, s / 2, t, color);
      break;
    case MENU_ICON_EMULATION:
      overlay_border_rect(x + 2, y + 2, s - 4, s - 4, t, color);
      overlay_fill_rect(x + 4, y + mid - t / 2, s - 8, t, color);
      overlay_fill_rect(x + mid - t / 2, y + 4, t, s - 8, color);
      break;
    case MENU_ICON_RESET:
      overlay_border_rect(x + 3, y + 3, s - 6, s - 6, t, color);
      overlay_fill_rect(x + 2, y + 2, s / 2, t, color);
      overlay_fill_rect(x + 2, y + 2, t, s / 2, color);
      break;
    case MENU_ICON_EXIT:
      overlay_border_rect(x + 2, y + 1, s / 2, s - 2, t, color);
      overlay_fill_rect(x + s / 2 - t, y + mid - t / 2, s / 2, t, color);
      overlay_fill_rect(x + s - 6, y + mid - 5, 6, 10, color);
      break;
    default:
      overlay_border_rect(x + 3, y + 3, s - 6, s - 6, t, color);
      overlay_fill_rect(x + 6, y + mid - t / 2, s - 12, t, color);
      break;
  }
}

static enum MenuIcon tab_icon(int index) {
  static const enum MenuIcon icons[] = {
    MENU_ICON_RESUME, MENU_ICON_SAVE, MENU_ICON_LOAD, MENU_ICON_CHEAT,
    MENU_ICON_DISPLAY, MENU_ICON_EMULATION, MENU_ICON_RESET, MENU_ICON_EXIT,
  };
  return (unsigned)index < sizeof(icons) / sizeof(*icons)
      ? icons[index] : MENU_ICON_ROW;
}

static void draw_shell(const DrasticIngameMenu *menu, const char *title,
                       const char *help) {
  static const char *tabs[] = {
    "返回游戏", "保存状态", "读取状态", "金手指",
    "画面设置", "模拟设置", "重置游戏", "退出模拟器",
  };
  const int width = ui_width();
  const int height = ui_height();
  const int header_x = ui_is_portrait() ? 36 : 64;
  overlay_fill_rect(0, 0, width, height, COLOR_DIM);
  for (int band = 0; band < 8; band++) {
    const unsigned shade = 20u - (unsigned)((band * 9) / 7);
    overlay_fill_rect(0, band * height / 8, width, height / 8,
                      0xa0000000u | (shade << 16) | (shade << 8) | shade);
  }

  overlay_draw_text_scaled(header_x, 28, 2, COLOR_TEXT, "游戏菜单");
  overlay_fill_rect(header_x - 8, 92, width - (header_x - 8) * 2, 1,
                    0x33ffffffu);
  overlay_draw_text_right(width - header_x, 48, COLOR_MUTED, title);

  const int side_x = menu_sidebar_x();
  const int side_y = 116;
  const int side_w = menu_sidebar_width();
  const int active = active_main_tab(menu);
  const int item_height = ui_is_portrait() ? 60 : 58;
  const int item_gap = ui_is_portrait() ? 6 : 7;
  for (int index = 0; index < 8; index++) {
    const int y = side_y + index * (item_height + item_gap) +
                  (index >= 6 ? 16 : 0);
    if (index == 6)
      overlay_fill_rect(side_x + 18, y - 9, side_w - 36, 1, 0x24ffffffu);
    const int selected = index == active;
    if (selected) {
      overlay_fill_rect(side_x, y, side_w, item_height, COLOR_SELECTED);
      overlay_border_rect(side_x - 3, y - 3, side_w + 6, item_height + 6,
                          1, 0x803ca8e8u);
      overlay_border_rect(side_x, y, side_w, item_height, 1, COLOR_ACCENT);
    } else {
      overlay_fill_rect(side_x, y, side_w, item_height, 0x0affffffu);
    }
    const uint32_t item_color = selected ? COLOR_TEXT : COLOR_MUTED;
    draw_menu_icon(side_x + 18, y + (item_height - 22) / 2, 22, item_color,
                   tab_icon(index));
    /* Slightly smaller than the former 2x tab label. */
    overlay_draw_text(side_x + 54, y + (item_height - 16) / 2,
                      item_color, tabs[index]);
  }

  if (!title || !title[0]) return;
  overlay_fill_rect(menu_content_x() - 28, menu_content_y() + 2, 1,
                    menu_content_height() - 14, 0x20ffffffu);
  /* nds_stub's content is not enclosed by a page card: only the title
   * separator frames it, while selectors and switches own their own focus. */
  overlay_draw_text(menu_content_x() + 22, menu_content_y() + 13,
                    COLOR_ACCENT, title);
  overlay_fill_rect(menu_content_x() + 18, menu_content_y() + 41,
                    menu_content_width() - 36, 1, 0x22ffffffu);
  if (help) {
    const int hint_width = ui_is_portrait() ? ui_width() - 56 : 390;
    const int hint_x = ui_width() - hint_width - (ui_is_portrait() ? 28 : 32);
    const int hint_y = ui_height() - 56;
    overlay_fill_rect(hint_x, hint_y, hint_width, 42, 0xf019222bu);
    overlay_border_rect(hint_x, hint_y, hint_width, 42, 1, 0x5a79c7f2u);
    overlay_draw_text_clipped(hint_x + 16, hint_y + 15, hint_width - 32,
                              COLOR_MUTED, help);
  }
}

static void draw_status(const DrasticIngameMenu *menu) {
  if (!menu->status[0]) return;
  const int width = ui_width();
  const int status_width = ui_is_portrait() ? width - 56 : 420;
  const int x = width - status_width - (ui_is_portrait() ? 28 : 32);
  const int y = ui_is_portrait() ? 34 : 108;
  overlay_fill_rect(x, y, status_width, 42, 0xf019222bu);
  overlay_border_rect(x, y, status_width, 42, 1, 0x5a79c7f2u);
  overlay_draw_text_clipped(x + 16, y + 8, status_width - 32,
                            COLOR_WARN, menu->status);
}

static void draw_row(int x, int y, int width, int selected,
                     const char *label, const char *value, int enabled) {
  overlay_fill_rect(x, y, width, 44, selected ? COLOR_SELECTED : COLOR_PANEL);
  overlay_border_rect(x, y, width, 44, 1,
                      selected ? 0x99ffffffu : 0x1cffffffu);
  if (selected)
    overlay_border_rect(x - 3, y - 3, width + 6, 50, 1, COLOR_ACCENT);
  const uint32_t label_color = enabled ? COLOR_TEXT : COLOR_MUTED;
  draw_menu_icon(x + 14, y + 12, 20, label_color, MENU_ICON_ROW);
  overlay_draw_text_clipped(x + 48, y + 14,
                            value ? width - 282 : width - 64,
                            label_color, label);
  if (value)
    overlay_draw_text_right(x + width - 16, y + 14,
                            enabled ? 0xffb5e4ffu : COLOR_MUTED, value);
}

static void draw_tab_empty(const char *message) {
  const int x = menu_content_x() + 32;
  const int y = menu_content_y() + 82;
  overlay_draw_text(x, y, COLOR_MUTED, message);
}

static void draw_row_scrolling_value(int x, int y, int width, int selected,
                                     const char *label, const char *value,
                                     int enabled) {
  draw_row(x, y, width, selected, label, NULL, enabled);
  const int value_width = width / 2 - 24;
  overlay_draw_text_scrolling_right(
      x + width - 16, y + 14, value_width,
      enabled ? COLOR_ACCENT : COLOR_MUTED, value);
}

static void draw_selector_row(int x, int y, int width, int selected,
                              const char *label, const char *value,
                              int enabled) {
  draw_row(x, y, width, selected, label, NULL, enabled);
  const uint32_t color = enabled ? COLOR_ACCENT : COLOR_MUTED;
  const int right = x + width - 16;
  const int icon_size = 24;
  const int left_icon = right - 132;
  const int right_icon = right - icon_size;
  const int value_left = left_icon + icon_size + 8;
  const int value_right = right_icon - 8;
  const int text_width = overlay_text_width(value ? value : "");
  overlay_draw_nintendo_glyph(left_icon, y + 10, icon_size, color, 0xE0E4);
  overlay_draw_nintendo_glyph(right_icon, y + 10, icon_size, color, 0xE0E5);
  overlay_draw_text_clipped(value_left + (value_right - value_left - text_width) / 2,
                            y + 14, value_right - value_left, color,
                            value ? value : "");
}

static void draw_switch_row(int x, int y, int width, int selected,
                            const char *label, int on, int enabled) {
  draw_row(x, y, width, selected, label, NULL, enabled);
  const int switch_w = 46, switch_h = 22;
  const int switch_x = x + width - switch_w - 18;
  const int switch_y = y + 11;
  const uint32_t color = on && enabled ? COLOR_SWITCH_ON : COLOR_SWITCH_OFF;
  overlay_fill_rect(switch_x, switch_y, switch_w, switch_h, color);
  overlay_border_rect(switch_x, switch_y, switch_w, switch_h, 1,
                      on && enabled ? 0xff9bd6ffu : 0xff8b949du);
  overlay_fill_rect(switch_x + (on && enabled ? switch_w - switch_h : 0) + 3,
                    switch_y + 3, switch_h - 6, switch_h - 6, COLOR_TEXT);
}

static void draw_link_row(int x, int y, int width, int selected,
                          const char *label, int enabled) {
  draw_row(x, y, width, selected, label, NULL, enabled);
  overlay_draw_text_right(x + width - 16, y + 14,
                          enabled ? COLOR_ACCENT : COLOR_MUTED, ">");
}

enum MainMenuItem {
  MAIN_RESUME,
  MAIN_SAVE_STATES,
  MAIN_LOAD_STATES,
  MAIN_CHEATS,
  MAIN_DISPLAY,
  MAIN_EMULATION,
  MAIN_RESET,
  MAIN_QUIT,
  MAIN_ITEM_COUNT,
};

static int main_item_available(int item) {
  (void)item;
  return 1;
}

static void render_main(DrasticIngameMenu *menu) {
  const int item = menu->selection[MENU_MAIN];
  if (item == MAIN_RESUME || item == MAIN_RESET || item == MAIN_QUIT) {
    draw_shell(menu, NULL, NULL);
    return;
  }
  draw_shell(menu, "主菜单", "A 确定   B 返回游戏");
  const int x = menu_content_x() + 34;
  const int y = menu_content_y() + 72;
  const int width = menu_content_width() - 68;
  overlay_draw_text_scaled(x, y, 2, COLOR_TEXT, "GBAStation DraStic");
  overlay_fill_rect(x, y + 48, width, 1, 0x5556748du);
  overlay_draw_wrapped(x, y + 78, width, 5, COLOR_MUTED,
      "使用方向键或左摇杆选择左侧功能，按 A 确定。菜单打开时游戏已暂停。");
  overlay_draw_wrapped(x, y + 178, width, 5, COLOR_MUTED,
      "即时存档、金手指、画面布局、滤镜、模拟器参数和输入设置都会保存到当前配置。");
  draw_status(menu);
}

static void render_states(DrasticIngameMenu *menu) {
  const int saving_page = menu->page == MENU_SAVE_STATES;
  draw_shell(menu, saving_page ? "保存状态" : "读取状态",
             saving_page ? (menu->content_focused ? "A 保存/覆盖   B 返回 tab" : "A 进入槽位选择")
                         : (menu->content_focused ? "A 读取   Y 删除   B 返回 tab" : "A 进入槽位选择"));
  const int saving = menu->core.is_saving &&
      menu->core.is_saving(menu->core.env, menu->core.clazz);
  int saving_slot = -1;
  if (saving && menu->core.get_saving_slot)
    saving_slot = menu->core.get_saving_slot(menu->core.env,
                                              menu->core.clazz);
  const int x = menu_content_x() + 24;
  const int y = menu_content_y() + 64;
  const int width = menu_content_width() - 48;
  overlay_draw_text(x, y, COLOR_TEXT, saving_page ? "保存状态" : "读取状态");

  int state_exists[10] = {0};
  char state_saved_at[10][32] = {{0}};
  DIR *directory = opendir(state_directory(menu));
  struct dirent *entry;
  while (directory && (entry = readdir(directory)) != NULL)
    for (int slot = 0; slot < 10; slot++)
      if (state_filename_matches_slot(entry->d_name, slot)) {
        state_exists[slot] = 1;
        char path[1200];
        struct stat info;
        snprintf(path, sizeof(path), "%s/%s", state_directory(menu),
                 entry->d_name);
        if (!stat(path, &info)) {
          struct tm *saved = localtime(&info.st_mtime);
          if (saved)
            strftime(state_saved_at[slot], sizeof(state_saved_at[slot]),
                     "%Y-%m-%d %H:%M", saved);
        }
      }
  if (directory) closedir(directory);

  const int row_height = 44;
  const int row_gap = 5;
  const int landscape = !ui_is_portrait();
  const int preview_height = landscape
      ? clamp_int(menu_content_height() - 102, 260, 420)
      : clamp_int(ui_height() - (y + 10 * (row_height + row_gap) + 66),
                  180, 520);
  const int preview_width = preview_height * 2 / 3;
  const int preview_x = landscape ? x + width - preview_width
                                  : x + (width - preview_width) / 2;
  const int list_width = landscape ? preview_x - x - 26 : width;
  const int visible_rows = landscape ? 9 : 12;
  int scroll = menu->scroll[menu->page];
  const int focused_slot = clamp_int(menu->selection[menu->page], 0, 9);
  if (focused_slot < scroll) scroll = focused_slot;
  if (focused_slot >= scroll + visible_rows)
    scroll = focused_slot - visible_rows + 1;
  scroll = clamp_int(scroll, 0, 10 > visible_rows ? 10 - visible_rows : 0);
  menu->scroll[menu->page] = scroll;
  for (int slot = 0; slot < 10; slot++) {
    const int row = slot - scroll;
    if (row < 0 || row >= visible_rows) continue;
    const int card_y = y + 40 + row * (row_height + row_gap);
    char label[64];
    snprintf(label, sizeof(label), "槽位 %d%s", slot,
             slot == *menu->state_slot ? "  [当前]" : "");
    char value[64];
    snprintf(value, sizeof(value), "%s", state_exists[slot]
             ? (state_saved_at[slot][0] ? state_saved_at[slot] : "已有状态")
             : "空槽");
    draw_row(x, card_y, list_width,
              menu->content_focused && slot == focused_slot,
             label, value, 1);
  }
  const int preview_y = landscape ? y + 38
                                  : y + 10 * (row_height + row_gap) + 64;
  overlay_fill_rect(preview_x - 4, preview_y - 4, preview_width + 8,
                    preview_height + 8, COLOR_PANEL_ALT);
  overlay_border_rect(preview_x - 4, preview_y - 4, preview_width + 8,
                      preview_height + 8, 1, 0x3cffffffu);
  if (menu->snapshot_valid) {
    overlay_blit_snapshot(preview_x, preview_y, preview_width,
                          preview_height / 2, menu->snapshot_top, 256, 192);
    overlay_blit_snapshot(preview_x, preview_y + preview_height / 2,
                          preview_width, preview_height - preview_height / 2,
                          menu->snapshot_bottom, 256, 192);
  } else {
    overlay_draw_text_clipped(preview_x + 16, preview_y + preview_height / 2 - 8,
                              preview_width - 32, COLOR_MUTED, "此槽位没有预览图");
  }
  if (saving) {
    char label[64];
    snprintf(label, sizeof(label), "正在保存槽位 %d...", saving_slot);
    overlay_draw_text(x, y + menu_content_height() - 38, COLOR_WARN, label);
  }
  draw_status(menu);
}

static void render_cheats(DrasticIngameMenu *menu) {
  draw_shell(menu, "金手指", "A 展开/开关   B 返回");
  const int list_x = menu_content_x() + 24;
  const int list_y = menu_content_y() + 58;
  const int list_width = menu_content_width() - 48;
  int visible_items[MENU_CHEAT_LIMIT];
  int count = 0;
  for (int item = 0; item < menu->cheat_count; item++) {
    int parent = menu->cheats[item].parent, visible = 1;
    while (parent >= 0) { if (!menu->cheats[parent].expanded) { visible = 0; break; } parent = menu->cheats[parent].parent; }
    if (visible) visible_items[count++] = item;
  }
  if (!count) { draw_tab_empty("当前游戏没有可用金手指。"); draw_status(menu); return; }
  int selection = clamp_int(menu->selection[MENU_CHEATS], 0, count - 1);
  int scroll = menu->scroll[MENU_CHEATS];
  if (selection < scroll) scroll = selection;
  const int visible_rows = ui_is_portrait() ? 12 : 9;
  if (selection >= scroll + visible_rows) scroll = selection - visible_rows + 1;
  scroll = clamp_int(scroll, 0, count > visible_rows ? count - visible_rows : 0);
  menu->scroll[MENU_CHEATS] = scroll;
  for (int row = 0; row < visible_rows && scroll + row < count; row++) {
    MenuCheat *cheat = &menu->cheats[visible_items[scroll + row]];
    char label[200];
    snprintf(label, sizeof(label), "%*s%s%s", cheat->depth * 2, "",
             cheat->is_category ? (cheat->expanded ? "▼ " : "▶ ") : "  ", cheat->name);
    draw_row(list_x, list_y + row * 52, list_width,
              menu->content_focused && selection == scroll + row,
              label, NULL, 1);
    if (!cheat->is_category) {
      const int switch_w = 46, switch_h = 22;
      const int switch_x = list_x + list_width - switch_w - 18;
      const int switch_y = list_y + row * 52 + 11;
      const uint32_t color = cheat->enabled ? COLOR_SWITCH_ON : COLOR_SWITCH_OFF;
      overlay_fill_rect(switch_x, switch_y, switch_w, switch_h, color);
      overlay_border_rect(switch_x, switch_y, switch_w, switch_h, 1,
                          cheat->enabled ? 0xff9bd6ffu : 0xff8b949du);
      overlay_fill_rect(switch_x + (cheat->enabled ? switch_w - switch_h : 0) + 3,
                        switch_y + 3, switch_h - 6, switch_h - 6, COLOR_TEXT);
    }
  }
  draw_status(menu);
}

static const char *layout_label(DrasticLayoutMode layout) {
  static const char *labels[] = {
    "纵向", "横向", "仅上屏", "仅触摸屏", "上屏优先",
    "触摸屏优先", "自定义"
  };
  return (unsigned)layout < sizeof(labels) / sizeof(*labels)
      ? labels[layout] : labels[0];
}

static const char *filter_label(DrasticVideoFilter filter) {
  static const char *labels[DRASTIC_FILTER_COUNT] = {
    "最近邻", "线性", "Quilez", "（已移除）", "Scale2x", "HQ2x", "FXAA",
    "FXAA 高质量", "SMAA", "自定义"
  };
  return filter == DRASTIC_FILTER_SCANLINE ? labels[DRASTIC_FILTER_NEAREST]
      : (unsigned)filter < DRASTIC_FILTER_COUNT ? labels[filter] : labels[0];
}

static const DrasticVideoFilter builtin_filters[] = {
  DRASTIC_FILTER_NEAREST, DRASTIC_FILTER_LINEAR, DRASTIC_FILTER_QUILEZ,
  DRASTIC_FILTER_SCALE2X, DRASTIC_FILTER_HQ2X, DRASTIC_FILTER_FXAA,
  DRASTIC_FILTER_FXAA_HQ, DRASTIC_FILTER_SMAA,
};

static int builtin_filter_index(DrasticVideoFilter filter) {
  for (int index = 0; index < (int)(sizeof(builtin_filters) / sizeof(*builtin_filters)); index++)
    if (builtin_filters[index] == filter) return index;
  return -1;
}

static const char *custom_shader_name(const DrasticIngameMenu *menu,
                                      const char *relative_path) {
  for (int index = 0; index < menu->custom_shader_count; index++)
    if (!strcmp(menu->custom_shaders[index].relative_path, relative_path))
      return menu->custom_shaders[index].name;
  const char *slash = relative_path ? strrchr(relative_path, '/') : NULL;
  return relative_path && relative_path[0] ? (slash ? slash + 1 : relative_path)
                                           : "未选择";
}

static const char *filter_picker_label(const DrasticIngameMenu *menu) {
  if (!menu->filter_picker_custom)
    return menu->filter_picker_index >= 0 &&
           menu->filter_picker_index < (int)(sizeof(builtin_filters) / sizeof(*builtin_filters))
        ? filter_label(builtin_filters[menu->filter_picker_index])
        : "最近邻";
  return menu->filter_picker_index >= 0 &&
         menu->filter_picker_index < menu->custom_shader_count
      ? menu->custom_shaders[menu->filter_picker_index].name
      : "未选择";
}

static void render_filter_picker(DrasticIngameMenu *menu) {
  char selection[160];
  snprintf(selection, sizeof(selection), "%s   <  %s  >",
           menu->filter_picker_custom ? "自定义滤镜" : "滤镜",
           filter_picker_label(menu));

  /* Keep the game unobstructed while previewing filters. The overlay buffer
   * is transparent outside this compact, opaque control bar. */
  const int portrait = ui_is_portrait();
  const int x = portrait ? 20 : 180;
  const int show_error = menu->status[0] != '\0';
  const int y = ui_height() - (show_error ? 98 : 70);
  const int width = portrait ? ui_width() - 40 : 920;
  const int height = show_error ? 80 : 52;
  overlay_fill_rect(x, y, width, height, 0xff18202cu);
  overlay_border_rect(x, y, width, height, 2, COLOR_ACCENT);
  overlay_draw_text(x + 24, y + 18, COLOR_MUTED, "B 取消");
  const int selection_x = x + (portrait ? 176 : 190);
  const int selection_width = width - (portrait ? 336 : 380);
  overlay_draw_text_scrolling(selection_x, y + 18, selection_width,
                              COLOR_TEXT, selection);
  overlay_draw_text_right(x + width - 24, y + 18,
                          COLOR_MUTED, "A 应用");
  if (show_error)
    overlay_draw_text_clipped(x + 24, y + 50, width - 48,
                              COLOR_WARN, menu->status);
}

static int has_png_extension(const char *name) {
  const char *extension = name ? strrchr(name, '.') : NULL;
  return extension && (!strcmp(extension, ".png") || !strcmp(extension, ".PNG"));
}

static void add_overlay_file(DrasticIngameMenu *menu, const char *path,
                             int is_directory) {
  if (!path || !path[0] || menu->overlay_file_count >= 64) return;
  for (int index = 0; index < menu->overlay_file_count; index++)
    if (!strcmp(menu->overlay_files[index], path)) return;
  const int index = menu->overlay_file_count++;
  snprintf(menu->overlay_files[index],
            sizeof(menu->overlay_files[0]), "%s", path);
  menu->overlay_file_is_directory[index] = is_directory != 0;
}

static void refresh_overlay_files_in_directory(DrasticIngameMenu *menu,
                                               const char *directory) {
  menu->overlay_file_count = 0;
  menu->overlay_picker_index = 0;
  snprintf(menu->overlay_picker_directory, sizeof(menu->overlay_picker_directory),
           "%s", directory ? directory : "sdmc:/GBAStation/overlays");
  char parent[1024];
  snprintf(parent, sizeof(parent), "%s", menu->overlay_picker_directory);
  char *slash = strrchr(parent, '/');
  if (slash && slash != parent) {
    *slash = '\0';
    add_overlay_file(menu, parent, 1);
  }
  DIR *dir = opendir(directory);
  if (!dir) return;
  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL) {
    if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name);
    struct stat info;
    if (!stat(path, &info) && S_ISDIR(info.st_mode))
      add_overlay_file(menu, path, 1);
    else if (has_png_extension(entry->d_name))
      add_overlay_file(menu, path, 0);
  }
  closedir(dir);
}

static void refresh_overlay_files(DrasticIngameMenu *menu) {
  const char *directory = menu->overlay_picker_directory[0]
      ? menu->overlay_picker_directory : "sdmc:/GBAStation/overlays";
  refresh_overlay_files_in_directory(menu, directory);
  if (!menu->overlay_file_count && !strcmp(directory, "sdmc:/GBAStation/overlays"))
    refresh_overlay_files_in_directory(menu, "/GBAStation/overlays");
  for (int index = 0; index < menu->overlay_file_count; index++)
    if (!strcmp(menu->overlay_files[index], menu->config->overlay_path))
      menu->overlay_picker_index = index;
}

static const char *file_basename(const char *path) {
  const char *slash = path ? strrchr(path, '/') : NULL;
  return slash ? slash + 1 : (path ? path : "");
}

static void render_overlay_sidebar(DrasticIngameMenu *menu) {
  const int canvas_width = ui_width();
  const int canvas_height = ui_height();
  const int panel_width = ui_is_portrait() ? 408 : 468;
  const int panel_x = canvas_width - panel_width;
  const int row_x = panel_x + 29;
  const int row_width = panel_width - 58;
  const int header_y = ui_is_portrait() ? 38 : 30;
  const int hint_y = header_y + 38;
  const int section_y = ui_is_portrait() ? 158 : 122;
  const int row_y = section_y + 44;
  const int row_gap = 67;
  const char *path = menu->config->overlay_path[0]
      ? file_basename(menu->config->overlay_path) : "未选择";

  overlay_fill_rect(0, 0, canvas_width, canvas_height, 0x26000000u);
  overlay_fill_rect(panel_x, 0, panel_width, canvas_height, 0xc805080cu);
  overlay_fill_rect(panel_x, 0, 1, canvas_height, 0x33ffffffu);
  overlay_draw_text_scaled(panel_x + 30, header_y, 2, COLOR_TEXT, "遮罩设置");
  overlay_draw_text(panel_x + 30, hint_y, COLOR_MUTED, "B 保存返回   A 选择");
  overlay_fill_rect(panel_x + 28, section_y + 12, 88, 1, 0x33ffffffu);
  overlay_draw_text(panel_x + 128, section_y - 2, COLOR_MUTED, "遮罩设置");
  overlay_fill_rect(panel_x + 230, section_y + 12, panel_width - 258, 1,
                    0x33ffffffu);
  draw_switch_row(row_x, row_y, row_width, menu->overlay_sidebar_focus == 0,
                  "遮罩开关", menu->config->overlay_enabled, 1);
  draw_link_row(row_x, row_y + row_gap, row_width,
                menu->overlay_sidebar_focus == 1, "遮罩选择", 1);
  const int path_width = overlay_text_width(path);
  overlay_draw_text_clipped(row_x + row_width - 48 - path_width,
                            row_y + row_gap + 14, row_width / 2 - 32,
                            COLOR_ACCENT, path);
}

static void render_overlay_picker(DrasticIngameMenu *menu) {
  const int screen_width = ui_width();
  const int screen_height = ui_height();
  const int width = ui_is_portrait() ? screen_width - 48 : 900;
  const int height = screen_height - 72;
  const int x = (screen_width - width) / 2;
  const int y = (screen_height - height) / 2;
  overlay_fill_rect(0, 0, screen_width, screen_height, 0x30000000u);
  overlay_fill_rect(x, y, width, height, 0xd019222bu);
  overlay_border_rect(x, y, width, height, 2, COLOR_ACCENT);
  overlay_draw_text_scaled(x + 28, y + 22, 2, COLOR_TEXT, "选择 PNG 遮罩");
  overlay_draw_text_right(x + width - 28, y + 28, COLOR_MUTED,
                          "A 打开/应用   X 预览   B 返回");
  overlay_fill_rect(x + 24, y + 66, width - 48, 1, 0x33ffffffu);
  const int list_x = x + 24;
  const int list_y = y + 86;
  const int list_width = width - 48;
  if (!menu->overlay_file_count) {
    overlay_draw_text(list_x + 8, list_y + 18, COLOR_MUTED,
                      "未找到 PNG 遮罩。请将文件放到 sdmc:/GBAStation/overlays。");
    draw_status(menu);
    return;
  }
  const int visible = clamp_int((height - 130) / 52, 4, 11);
  int scroll = menu->scroll[MENU_OVERLAY_PICKER];
  if (menu->overlay_picker_index < scroll) scroll = menu->overlay_picker_index;
  if (menu->overlay_picker_index >= scroll + visible)
    scroll = menu->overlay_picker_index - visible + 1;
  scroll = clamp_int(scroll, 0, menu->overlay_file_count - visible > 0
                              ? menu->overlay_file_count - visible : 0);
  menu->scroll[MENU_OVERLAY_PICKER] = scroll;
  for (int row = 0; row < visible && scroll + row < menu->overlay_file_count; row++) {
    const int item = scroll + row;
    draw_row(list_x, list_y + row * 52, list_width,
             item == menu->overlay_picker_index,
             file_basename(menu->overlay_files[item]),
             menu->overlay_file_is_directory[item] ? "文件夹" :
             !strcmp(menu->overlay_files[item], menu->config->overlay_path)
                 ? "当前" : "PNG", 1);
  }
  const int preview_item = clamp_int(menu->overlay_picker_index, 0,
                                     menu->overlay_file_count - 1);
  if (menu->overlay_preview_visible &&
      !menu->overlay_file_is_directory[preview_item]) {
    const int preview_width = clamp_int(width - 160, 280, 520);
    const int preview_height = preview_width * 9 / 16;
    const int preview_x = x + (width - preview_width) / 2;
    const int preview_y = y + (height - preview_height) / 2;
    overlay_fill_rect(preview_x - 10, preview_y - 42, preview_width + 20,
                      preview_height + 52, 0xf019222bu);
    overlay_border_rect(preview_x - 10, preview_y - 42, preview_width + 20,
                        preview_height + 52, 2, COLOR_ACCENT);
    overlay_draw_text_clipped(preview_x, preview_y - 28, preview_width,
                              COLOR_TEXT, file_basename(menu->overlay_files[preview_item]));
    if (!overlay_draw_png_preview(menu->overlay_files[preview_item], preview_x,
                                  preview_y, preview_width, preview_height))
      overlay_draw_text(preview_x + 16, preview_y + preview_height / 2,
                        COLOR_WARN, "无法预览该 PNG 文件");
  }
  draw_status(menu);
}

static void render_display(DrasticIngameMenu *menu) {
  static const char *labels[] = {
    "画面布局", "自定义画面布局", "屏幕间距", "整数倍缩放", "画面方向",
    "帧生成（Lossless）", "内置滤镜", "遮罩设置", "自定义滤镜（即将推出）",
    "同步画面设置", "同步遮罩设置", "同步着色器设置"
  };
  char values[12][112] = {{0}};
  snprintf(values[0], sizeof(values[0]), "%s", layout_label(menu->config->layout));
  snprintf(values[1], sizeof(values[1]), "编辑");
  snprintf(values[2], sizeof(values[2]), "%d px", menu->config->screen_gap);
  snprintf(values[4], sizeof(values[4]), "%d 度", (menu->config->rotation & 3) * 90);
  snprintf(values[6], sizeof(values[6]), "%s",
           filter_label(menu->config->video_filter));
  const int lsfg_allowed = prefs_get_bool("Wrapper/LSFGAllowed", false);
  if (!lsfg_allowed)
    snprintf(values[5], sizeof(values[5]), "由启动器关闭");
  else if (!drastic_renderer_lsfg_dll_available())
    snprintf(values[5], sizeof(values[5]), "缺少 Lossless.dll");
  else if (drastic_renderer_lsfg_enabled())
    snprintf(values[5], sizeof(values[5]), "开");
  else if (drastic_renderer_lsfg_available())
    snprintf(values[5], sizeof(values[5]), "关");
  else if (prefs_get_bool("Wrapper/LSFGEnabled", false))
    snprintf(values[5], sizeof(values[5]), "不可用");
  else
    snprintf(values[5], sizeof(values[5]), "关（重启后可开启）");
  draw_shell(menu, "画面设置",
              "左右 调整   A 选择   B 返回");
  const int panel_x = menu_content_x();
  const int panel_width = menu_content_width();
  const int visible = ui_is_portrait() ? 12 : 8;
  int scroll = menu->scroll[MENU_DISPLAY];
  const int selected_index = menu->selection[MENU_DISPLAY];
  if (selected_index < scroll) scroll = selected_index;
  if (selected_index >= scroll + visible) scroll = selected_index - visible + 1;
  scroll = clamp_int(scroll, 0, 12 > visible ? 12 - visible : 0);
  menu->scroll[MENU_DISPLAY] = scroll;
  int row_y = menu_content_y() + 58;
  for (int row = 0; row < visible && scroll + row < 12; row++) {
    const int index = scroll + row;
    const char *section = index == 0 ? "屏幕布局" :
                          index == 5 ? "高级功能" :
                          index == 9 ? "同步设置" : NULL;
    if (section) {
      overlay_fill_rect(panel_x + 24, row_y + 11, 72, 1, 0x33ffffffu);
      overlay_draw_text(panel_x + 108, row_y, COLOR_MUTED, section);
      overlay_fill_rect(panel_x + 108 + 128, row_y + 11,
                        panel_width - 180, 1, 0x33ffffffu);
      row_y += 25;
    }
    const int row_x = panel_x + 24;
    const int row_width = panel_width - 48;
    const int selected = menu->content_focused &&
                          menu->selection[MENU_DISPLAY] == index;
    if (index == 0 || index == 2 || index == 4)
      draw_selector_row(row_x, row_y, row_width, selected, labels[index],
                        values[index], 1);
    else if (index == 1)
      draw_link_row(row_x, row_y, row_width, selected, labels[index],
                    menu->config->layout == DRASTIC_LAYOUT_CUSTOM);
    else if (index == 3)
      draw_switch_row(row_x, row_y, row_width, selected, labels[index],
                      menu->config->integer_scale, 1);
    else if (index == 5)
      draw_switch_row(row_x, row_y, row_width, selected, labels[index],
                      drastic_renderer_lsfg_enabled(), lsfg_allowed);
    else if (index == 8)
      draw_link_row(row_x, row_y, row_width, selected, labels[index], 0);
    else
      draw_link_row(row_x, row_y, row_width, selected, labels[index], 1);
    row_y += 52;
  }
  if (menu->confirm_sync) {
    const char *kind = menu->confirm_sync == 1 ? "画面设置" :
                       menu->confirm_sync == 2 ? "PNG 遮罩" : "着色器设置";
    const int modal_w = ui_is_portrait() ? ui_width() - 80 : 620;
    const int modal_x = (ui_width() - modal_w) / 2;
    const int modal_y = ui_height() / 2 - 72;
    overlay_fill_rect(modal_x, modal_y, modal_w, 144, 0xf019222bu);
    overlay_border_rect(modal_x, modal_y, modal_w, 144, 2, COLOR_ACCENT);
    overlay_draw_text(modal_x + 22, modal_y + 22, COLOR_TEXT, "同步到所有 NDS 游戏？");
    char text[180];
    snprintf(text, sizeof(text), "这会覆盖所有 NDS 游戏的%s设置。", kind);
    overlay_draw_text(modal_x + 22, modal_y + 58, COLOR_MUTED, text);
    overlay_draw_text(modal_x + 22, modal_y + 104, COLOR_ACCENT, "A 确认同步     B 取消");
  }
  draw_status(menu);
}

static const char *on_off(int value) { return value ? "开" : "关"; }

static const char *stylus_mode_label(DrasticStylusMode mode) {
  static const char *labels[] = {"关闭", "右摇杆", "体感控制"};
  return (unsigned)mode < sizeof(labels) / sizeof(*labels)
      ? labels[mode] : labels[0];
}

static void render_emulation(DrasticIngameMenu *menu) {
  static const char *labels[] = {
    "跳帧数量", "跳帧方式", "安全跳帧",
    "快进速度", "音量", "声音", "麦克风", "麦克风来源", "麦克风级别",
    "震动", "体感控制"
  };
  static const char *methods[] = {"自动", "固定", "激进", "最大"};
  char values[11][64] = {{0}};
  const int frameskip = prefs_get_int("Drastic/FrameskipValue", 0);
  const int method = clamp_int(prefs_get_int("Drastic/FrameskipType", 0), 0, 3);
  const int ff = clamp_int(prefs_get_int("Drastic/FastForwardSpeed", 5), 0, 5);
  snprintf(values[0], sizeof(values[0]), "%d", frameskip);
  snprintf(values[1], sizeof(values[1]), "%s", methods[method]);
  snprintf(values[2], sizeof(values[2]), "%s", on_off(prefs_get_bool("Drastic/FrameskipSafe", false)));
  static const char *ff_speeds[] = {
    "50%", "150%", "200%", "300%", "400%", "不限"
  };
  snprintf(values[3], sizeof(values[3]), "%s", ff_speeds[ff]);
  snprintf(values[4], sizeof(values[4]), "%d%%", menu->config->volume);
  snprintf(values[7], sizeof(values[7]), "%s", menu->config->microphone_source == DRASTIC_MICROPHONE_EXTERNAL ? "外接" : "模拟噪声");
  snprintf(values[8], sizeof(values[8]), "%s", (const char *[]){"低", "普通", "高", "最高"}[clamp_int(prefs_get_int("Drastic/MicLevel", 1), 0, 3)]);
  draw_shell(menu, "模拟设置",
             "左右 调整   A 开关   B 返回");
  const int panel_x = menu_content_x();
  const int panel_width = menu_content_width();
  const int visible = ui_is_portrait() ? 11 : 8;
  int scroll = menu->scroll[MENU_EMULATION];
  const int selected = menu->selection[MENU_EMULATION];
  if (selected < scroll) scroll = selected;
  if (selected >= scroll + visible) scroll = selected - visible + 1;
  scroll = clamp_int(scroll, 0, 11 > visible ? 11 - visible : 0);
  menu->scroll[MENU_EMULATION] = scroll;
  int row_y = menu_content_y() + 58;
  for (int row = 0; row < visible && scroll + row < 11; row++) {
    const int index = scroll + row;
    const char *section = index == 0 ? "运行与快进" :
                          index == 4 ? "音频与输入" : NULL;
    if (section) {
      overlay_fill_rect(panel_x + 24, row_y + 11, 72, 1, 0x33ffffffu);
      overlay_draw_text(panel_x + 108, row_y, COLOR_MUTED, section);
      overlay_fill_rect(panel_x + 108 + 128, row_y + 11,
                        panel_width - 180, 1, 0x33ffffffu);
      row_y += 25;
    }
    const int selected = menu->content_focused && menu->selection[MENU_EMULATION] == index;
    if (index == 0 || index == 1 || index == 3 || index == 4 || index == 7 || index == 8)
      draw_selector_row(panel_x + 24, row_y, panel_width - 48, selected,
                        labels[index], values[index], 1);
    else if (index == 2)
      draw_switch_row(panel_x + 24, row_y, panel_width - 48, selected,
                      labels[index], prefs_get_bool("Drastic/FrameskipSafe", false), 1);
    else if (index == 5)
      draw_switch_row(panel_x + 24, row_y, panel_width - 48, selected,
                      labels[index], prefs_get_bool("Drastic/SoundEnabled", true), 1);
    else if (index == 6)
      draw_switch_row(panel_x + 24, row_y, panel_width - 48, selected,
                      labels[index], menu->config->microphone_enabled, 1);
    else if (index == 9)
      draw_switch_row(panel_x + 24, row_y, panel_width - 48, selected,
                      labels[index], menu->config->vibration, 1);
    else
      draw_switch_row(panel_x + 24, row_y, panel_width - 48, selected,
                      labels[index], menu->config->motion, 1);
    row_y += 52;
  }
  draw_status(menu);
}

static void render_audio_input(DrasticIngameMenu *menu) {
  static const char *labels[] = {
    "音量", "声音", "麦克风", "麦克风来源",
    "麦克风级别", "震动卡震动", "陀螺仪与加速度计",
    "虚拟触笔", "USB 鼠标触笔", "返回"
  };
  static const char *levels[] = {"低", "普通", "高", "最高"};
  char values[10][64] = {{0}};
  snprintf(values[0], sizeof(values[0]), "%d%%", menu->config->volume);
  snprintf(values[1], sizeof(values[1]), "%s", on_off(prefs_get_bool("Drastic/SoundEnabled", true)));
  snprintf(values[2], sizeof(values[2]), "%s",
           on_off(menu->config->microphone_enabled));
  if (menu->config->microphone_source == DRASTIC_MICROPHONE_EXTERNAL) {
    const char *state = "外接";
    if (menu->microphone_status == OPENSLES_MIC_STATUS_ACTIVE)
      state = "外接（已启用）";
    else if (menu->microphone_status == OPENSLES_MIC_STATUS_CONNECTING)
      state = "外接（连接中）";
    else if (menu->microphone_status == OPENSLES_MIC_STATUS_UNAVAILABLE)
      state = "外接（未检测到）";
    snprintf(values[3], sizeof(values[3]), "%s", state);
  } else {
    snprintf(values[3], sizeof(values[3]), "模拟噪声");
  }
  snprintf(values[4], sizeof(values[4]), "%s",
           levels[clamp_int(prefs_get_int("Drastic/MicLevel", 1), 0, 3)]);
  snprintf(values[5], sizeof(values[5]), "%s", on_off(menu->config->vibration));
  snprintf(values[6], sizeof(values[6]), "%s", on_off(menu->config->motion));
  snprintf(values[7], sizeof(values[7]), "%s",
           stylus_mode_label(menu->config->stylus_mode));
  snprintf(values[8], sizeof(values[8]), "%s",
           on_off(menu->config->mouse_stylus));
  draw_shell(menu, "音频与输入",
             "左右 调整   A 开关   B 返回");
  const int panel_x = menu_content_x();
  const int panel_width = menu_content_width();
  for (int index = 0; index < 10; index++)
    draw_row(panel_x + 24, menu_content_y() + 58 + index * 45, panel_width - 48,
             menu->content_focused && menu->selection[MENU_AUDIO_INPUT] == index,
             labels[index],
             index < 9 ? values[index] : NULL, 1);
  overlay_draw_wrapped(panel_x + 24, menu_content_y() + menu_content_height() - 60,
                       panel_width - 48,
                       ui_is_portrait() ? 8 : 3, COLOR_MUTED,
      "外接麦克风可使用 CTIA 耳麦或兼容 USB 麦克风。体感触笔使用回中热键；鼠标左键可触摸。");
  draw_status(menu);
}

static void render_layout_editor(DrasticIngameMenu *menu) {
  const int canvas_width = ui_width();
  const int canvas_height = ui_height();
  const int panel_width = ui_is_portrait() ? 384 : 432;
  const int panel_x = canvas_width - panel_width;
  const int row_x = panel_x + 29;
  const int row_width = panel_width - 58;
  const int header_y = ui_is_portrait() ? 38 : 30;
  const int hint_y = header_y + 38;
  const int top_section_y = ui_is_portrait() ? 158 : 116;
  const int top_row_y = top_section_y + 42;
  const int row_gap = ui_is_portrait() ? 74 : 65;
  const int bottom_section_y = top_row_y + row_gap * 3 +
      (ui_is_portrait() ? 60 : 26);
  const int bottom_row_y = bottom_section_y + 42;
  char values[6][48];
  snprintf(values[0], sizeof(values[0]), "%.1f", menu->config->custom_top_scale);
  snprintf(values[1], sizeof(values[1]), "%.0f px", menu->config->custom_top_offset_x);
  snprintf(values[2], sizeof(values[2]), "%.0f px", menu->config->custom_top_offset_y);
  snprintf(values[3], sizeof(values[3]), "%.1f", menu->config->custom_bottom_scale);
  snprintf(values[4], sizeof(values[4]), "%.0f px", menu->config->custom_bottom_offset_x);
  snprintf(values[5], sizeof(values[5]), "%.0f px", menu->config->custom_bottom_offset_y);
  static const char *labels[] = {"缩放", "X 偏移", "Y 偏移", "缩放", "X 偏移", "Y 偏移"};

  /* Match nds_stub's editor: the paused game remains visible and the
   * controls live in a right-side inspector instead of screen rectangles. */
  overlay_fill_rect(0, 0, canvas_width, canvas_height, 0x26000000u);
  overlay_fill_rect(panel_x, 0, panel_width, canvas_height, 0xc805080cu);
  overlay_fill_rect(panel_x, 0, 1, canvas_height, 0x33ffffffu);
  overlay_draw_text_scaled(panel_x + 30, header_y, 2, COLOR_TEXT, "自定义画面布局");
  overlay_draw_text(panel_x + 30, hint_y, COLOR_MUTED, "B 保存返回   A 重置当前项");
  overlay_fill_rect(panel_x + 28, top_section_y + 12, 88, 1, 0x33ffffffu);
  overlay_draw_text(panel_x + 128, top_section_y - 2, COLOR_MUTED, "上屏布局");
  overlay_fill_rect(panel_x + 230, top_section_y + 12, panel_width - 258, 1, 0x33ffffffu);
  overlay_fill_rect(panel_x + 28, bottom_section_y + 12, 88, 1, 0x33ffffffu);
  overlay_draw_text(panel_x + 128, bottom_section_y - 2, COLOR_MUTED, "下屏布局");
  overlay_fill_rect(panel_x + 230, bottom_section_y + 12, panel_width - 258, 1, 0x33ffffffu);
  for (int item = 0; item < 6; item++) {
    const int y = item < 3 ? top_row_y + item * row_gap
                           : bottom_row_y + (item - 3) * row_gap;
    draw_selector_row(row_x, y, row_width, item == menu->editor_screen,
                      labels[item], values[item], 1);
  }
}

static void render_menu(DrasticIngameMenu *menu) {
  if (!menu || !menu->open) return;
  overlay_begin();
  switch (menu->page) {
    case MENU_SAVE_STATES:
    case MENU_LOAD_STATES: render_states(menu); break;
    case MENU_CHEATS: render_cheats(menu); break;
    case MENU_DISPLAY: render_display(menu); break;
    case MENU_OVERLAY_SIDEBAR: render_overlay_sidebar(menu); break;
    case MENU_OVERLAY_PICKER: render_overlay_picker(menu); break;
    case MENU_FILTER_PICKER: render_filter_picker(menu); break;
    case MENU_EMULATION: render_emulation(menu); break;
    case MENU_AUDIO_INPUT: render_audio_input(menu); break;
    case MENU_LAYOUT_EDITOR: render_layout_editor(menu); break;
    default: render_main(menu); break;
  }
  overlay_finish();
  menu->redraw = 0;
}

static void select_page(DrasticIngameMenu *menu, enum MenuPage page) {
  menu->page = page;
  menu->status[0] = '\0';
  menu->marquee_tick = 0;
  const u64 frequency = armGetSystemTickFreq();
  menu->hint_until = frequency ? armGetSystemTick() + frequency * 3 : 0;
  menu->redraw = 1;
  if (page == MENU_SAVE_STATES || page == MENU_LOAD_STATES) {
    menu->confirm_delete_slot = -1;
    menu->selection[page] = *menu->state_slot;
    refresh_snapshot(menu);
  } else if (page == MENU_CHEATS) {
    refresh_cheats(menu);
  } else if (page == MENU_OVERLAY_PICKER) {
    refresh_overlay_files(menu);
  }
}

static void navigate_list(DrasticIngameMenu *menu, int count, u64 pressed) {
  int *selection = &menu->selection[menu->page];
  if (pressed & HidNpadButton_Up)
    *selection = (*selection + count - 1) % count;
  if (pressed & HidNpadButton_Down)
    *selection = (*selection + 1) % count;
  if (pressed & (HidNpadButton_Up | HidNpadButton_Down))
    menu->redraw = 1;
}

static void update_main(DrasticIngameMenu *menu, u64 pressed) {
  int *selection = &menu->selection[MENU_MAIN];
  int direction = 0;
  if (pressed & HidNpadButton_Up) direction = -1;
  if (pressed & HidNpadButton_Down) direction = 1;
  if (direction) {
    for (int count = 0; count < MAIN_ITEM_COUNT; count++) {
      *selection = (*selection + direction + MAIN_ITEM_COUNT) %
                   MAIN_ITEM_COUNT;
      if (main_item_available(*selection)) break;
    }
    /* A tab previews its child page, but focus stays in the sidebar until A
     * is pressed.  Resume, reset and exit intentionally have no child page. */
    preview_main_tab(menu, *selection);
    return;
  }
  if (pressed & HidNpadButton_B) {
    drastic_menu_close(menu, true);
    return;
  }
  if (!(pressed & HidNpadButton_A)) return;
  switch (menu->selection[MENU_MAIN]) {
    case MAIN_RESUME: drastic_menu_close(menu, true); break;
    case MAIN_SAVE_STATES:
    case MAIN_LOAD_STATES:
    case MAIN_CHEATS:
    case MAIN_DISPLAY:
    case MAIN_EMULATION:
      menu->content_focused = 1;
      {
        const u64 frequency = armGetSystemTickFreq();
        menu->hint_until = frequency ? armGetSystemTick() + frequency * 3 : 0;
      }
      menu->redraw = 1;
      break;
    case MAIN_RESET:
      if (menu->core.reset_ds)
        menu->core.reset_ds(menu->core.env, menu->core.clazz);
      drastic_menu_close(menu, true);
      break;
    case MAIN_QUIT:
      menu->exit_requested = 1;
      /* Android leaves DraStic paused while its Activity is finishing.  Do
       * not resume the core for the small window between selecting Quit and
       * the native shutdown sequence. */
      drastic_menu_close(menu, false);
      break;
  }
}

static void update_states(DrasticIngameMenu *menu, u64 pressed) {
  const int saving_page = menu->page == MENU_SAVE_STATES;
  const int old = menu->selection[menu->page];
  navigate_list(menu, 10, pressed);
  if (old != menu->selection[menu->page]) {
    menu->confirm_delete_slot = -1;
    *menu->state_slot = menu->selection[menu->page];
    save_int("Wrapper/StateSlot", *menu->state_slot);
    refresh_snapshot(menu);
  }
  if (pressed & HidNpadButton_B) {
    leave_content_focus(menu);
    return;
  }
  const int slot = menu->selection[menu->page];
  if (!saving_page && (pressed & HidNpadButton_A) && menu->core.load_state) {
    const int result = menu->core.load_state(menu->core.env, menu->core.clazz,
                                             slot);
    if (result) drastic_menu_close(menu, true);
    else set_status(menu, "无法读取此槽位");
  }
  if (saving_page && (pressed & HidNpadButton_A) && menu->core.save_state) {
    const int result = menu->core.save_state(menu->core.env, menu->core.clazz,
                                             slot, 1);
    if (result) drastic_menu_note_state_save(menu, slot);
    set_status(menu, result ? "已请求保存即时存档" :
                              "无法保存到此槽位");
  }
  if (!saving_page && (pressed & HidNpadButton_Y)) {
    if (!menu->snapshot_valid) {
      set_status(menu, "此槽位没有可删除的即时存档");
      menu->confirm_delete_slot = -1;
    } else if (menu->confirm_delete_slot != slot) {
      menu->confirm_delete_slot = slot;
      set_status(menu, "再次按 Y 永久删除此即时存档");
    } else {
      const int deleted = delete_matching_state(menu, slot);
      menu->confirm_delete_slot = -1;
      refresh_snapshot(menu);
      set_status(menu, deleted ? "即时存档已删除" :
                                 "无法安全识别即时存档文件");
    }
  } else if (pressed & (HidNpadButton_A | HidNpadButton_B)) {
    menu->confirm_delete_slot = -1;
  }
}

static void update_cheats(DrasticIngameMenu *menu, u64 pressed) {
  int visible_items[MENU_CHEAT_LIMIT];
  int count = 0;
  for (int item = 0; item < menu->cheat_count; item++) {
    int parent = menu->cheats[item].parent, visible = 1;
    while (parent >= 0) { if (!menu->cheats[parent].expanded) { visible = 0; break; } parent = menu->cheats[parent].parent; }
    if (visible) visible_items[count++] = item;
  }
  if (!count) return;
  navigate_list(menu, count, pressed);
  if (pressed & HidNpadButton_B) {
    leave_content_focus(menu);
    return;
  }
  const int selected = clamp_int(menu->selection[MENU_CHEATS], 0, count - 1);
  MenuCheat *cheat = &menu->cheats[visible_items[selected]];
  if (cheat->is_category) {
    if (pressed & HidNpadButton_A) {
      cheat->expanded ^= 1;
      /* Keep the category under focus.  Resetting to zero made expansion
       * unexpectedly jump the cursor back to the first database entry. */
      menu->selection[MENU_CHEATS] = selected;
      menu->redraw = 1;
    }
    return;
  }
  if (pressed & HidNpadButton_A) {
    const int enabled = !cheat->enabled;
    if (!set_database_cheat_enabled(menu, cheat, enabled)) {
      set_status(menu, "无法将此金手指注册到 DraStic 核心");
      return;
    }
    if (menu->core.update_cheats)
      menu->core.update_cheats(menu->core.env, menu->core.clazz, 1);
    persist_database_cheats(menu);
    debug_logf("cheats set visible_index=%d database_index=%d custom_index=%d enabled=%d core_count=%d",
               selected, cheat->index, cheat->custom_index, cheat->enabled,
               menu->core.get_cheat_count
                   ? menu->core.get_cheat_count(menu->core.env, menu->core.clazz)
                   : -1);
    set_status(menu, cheat->enabled ? "金手指已启用" : "金手指已关闭");
  }
}

static void leave_content_focus(DrasticIngameMenu *menu) {
  menu->content_focused = 0;
  menu->status[0] = '\0';
  menu->redraw = 1;
}

static void preview_main_tab(DrasticIngameMenu *menu, int tab) {
  menu->content_focused = 0;
  switch (tab) {
    case MAIN_SAVE_STATES: select_page(menu, MENU_SAVE_STATES); break;
    case MAIN_LOAD_STATES: select_page(menu, MENU_LOAD_STATES); break;
    case MAIN_CHEATS: select_page(menu, MENU_CHEATS); break;
    case MAIN_DISPLAY: select_page(menu, MENU_DISPLAY); break;
    case MAIN_EMULATION: select_page(menu, MENU_EMULATION); break;
    default:
      menu->page = MENU_MAIN;
      menu->status[0] = '\0';
      menu->redraw = 1;
      break;
  }
}

static const char *layout_value(DrasticLayoutMode layout) {
  static const char *values[] = {
    "vertical", "horizontal", "top", "bottom", "hybrid_top",
    "hybrid_bottom", "custom"
  };
  return (unsigned)layout < sizeof(values) / sizeof(*values)
      ? values[layout] : values[0];
}

static int change_direction(u64 pressed) {
  if (pressed & (HidNpadButton_Left | HidNpadButton_L)) return -1;
  if (pressed & (HidNpadButton_Right | HidNpadButton_R | HidNpadButton_A)) return 1;
  return 0;
}

static void refresh_custom_shaders(DrasticIngameMenu *menu) {
  menu->custom_shader_count = (int)drastic_custom_shader_scan(
      menu->custom_shaders,
      sizeof(menu->custom_shaders) / sizeof(menu->custom_shaders[0]));
}

static int configured_custom_shader_index(const DrasticIngameMenu *menu) {
  for (int index = 0; index < menu->custom_shader_count; index++) {
    if (!strcmp(menu->custom_shaders[index].relative_path,
                menu->config->custom_shader))
      return index;
  }
  return -1;
}

static int select_builtin_filter_candidate(DrasticIngameMenu *menu,
                                            int candidate) {
  if (candidate < 0 || candidate >= (int)(sizeof(builtin_filters) / sizeof(*builtin_filters))) return 0;
  menu->config->video_filter = builtin_filters[candidate];
  menu->filter_picker_index = candidate;
  menu->filter_picker_valid = 1;
  menu->status[0] = '\0';
  menu->redraw = 1;
  return 1;
}

static int select_custom_shader_candidate(DrasticIngameMenu *menu,
                                          int candidate) {
  if (candidate < 0 || candidate >= menu->custom_shader_count) return 0;
  menu->filter_picker_index = candidate;
  menu->filter_picker_valid = 0;
  char error[192];
  if (!drastic_renderer_set_custom_shader(
          menu->custom_shaders[candidate].relative_path,
          error, sizeof(error))) {
    set_status(menu, error);
    return 0;
  }
  menu->config->video_filter = DRASTIC_FILTER_CUSTOM;
  snprintf(menu->config->custom_shader, sizeof(menu->config->custom_shader),
           "%s", menu->custom_shaders[candidate].relative_path);
  menu->filter_picker_valid = 1;
  menu->status[0] = '\0';
  menu->redraw = 1;
  return 1;
}

static void preview_filter(DrasticIngameMenu *menu, int direction) {
  const int count = menu->filter_picker_custom
      ? menu->custom_shader_count
      : (int)(sizeof(builtin_filters) / sizeof(*builtin_filters));
  if (count <= 0) return;
  const int candidate = (menu->filter_picker_index + direction + count) % count;
  if (menu->filter_picker_custom)
    select_custom_shader_candidate(menu, candidate);
  else
    select_builtin_filter_candidate(menu, candidate);
}

static int restore_filter_backup(DrasticIngameMenu *menu) {
  if (menu->filter_backup == DRASTIC_FILTER_CUSTOM) {
    char error[192];
    if (!drastic_renderer_set_custom_shader(
            menu->filter_backup_shader, error, sizeof(error))) {
      set_status(menu, error);
      return 0;
    }
  }
  menu->config->video_filter = menu->filter_backup;
  snprintf(menu->config->custom_shader, sizeof(menu->config->custom_shader),
           "%s", menu->filter_backup_shader);
  menu->redraw = 1;
  return 1;
}

static void begin_filter_picker(DrasticIngameMenu *menu, int custom,
                                int direction) {
  if (custom) {
    refresh_custom_shaders(menu);
    if (!menu->custom_shader_count) {
      set_status(menu, "未找到自定义滤镜");
      return;
    }
  }

  menu->filter_backup = menu->config->video_filter;
  snprintf(menu->filter_backup_shader, sizeof(menu->filter_backup_shader),
           "%s", menu->config->custom_shader);
  menu->filter_picker_custom = custom;
  menu->filter_picker_valid = 0;
  menu->status[0] = '\0';

  const int count = custom ? menu->custom_shader_count
                           : (int)(sizeof(builtin_filters) / sizeof(*builtin_filters));
  int current = custom ? configured_custom_shader_index(menu)
                       : builtin_filter_index(menu->config->video_filter);
  int candidate;
  if (current < 0)
    candidate = direction < 0 ? count - 1 : 0;
  else
    candidate = (current + direction + count) % count;

  select_page(menu, MENU_FILTER_PICKER);
  if (custom)
    select_custom_shader_candidate(menu, candidate);
  else
    select_builtin_filter_candidate(menu, candidate);
}

static void update_filter_picker(DrasticIngameMenu *menu, u64 pressed) {
  if (pressed & HidNpadButton_B) {
    if (!restore_filter_backup(menu)) return;
    menu->status[0] = '\0';
    select_page(menu, MENU_DISPLAY);
    return;
  }
  if (pressed & HidNpadButton_A) {
    if (!menu->filter_picker_valid) {
      set_status(menu, menu->filter_picker_custom
          ? "无法加载此自定义滤镜"
          : "无法选择此滤镜");
      return;
    }
    save_string("Wrapper/VideoFilter",
                drastic_config_filter_name(menu->config->video_filter));
    if (menu->filter_picker_custom)
      save_string("Wrapper/CustomShader", menu->config->custom_shader);
    menu->status[0] = '\0';
    select_page(menu, MENU_DISPLAY);
    return;
  }
  if (pressed & HidNpadButton_Left)
    preview_filter(menu, -1);
  else if (pressed & HidNpadButton_Right)
    preview_filter(menu, 1);
}

static void update_display(DrasticIngameMenu *menu, u64 pressed) {
  if (menu->confirm_sync) {
    if (pressed & HidNpadButton_B) {
      menu->confirm_sync = 0;
      menu->redraw = 1;
      return;
    }
    if (pressed & HidNpadButton_A) {
      int count;
      if (menu->confirm_sync == 1)
        count = gamedb_sync_layout_to_all(menu->config->rom_path, menu->config);
      else if (menu->confirm_sync == 2)
        count = gamedb_sync_overlay_to_all(menu->config->rom_path, menu->config);
      else
        count = gamedb_sync_shader_to_all(menu->config->rom_path, menu->config);
      menu->confirm_sync = 0;
      set_status(menu, count > 0 ? "已同步到所有 NDS 游戏" : "同步 GameDB 失败");
      return;
    }
    return;
  }
  navigate_list(menu, 12, pressed);
  if (pressed & HidNpadButton_B) {
    leave_content_focus(menu);
    return;
  }
  const int selection = menu->selection[MENU_DISPLAY];
  if (selection >= 9 && selection <= 11 && (pressed & HidNpadButton_A)) {
    menu->confirm_sync = selection - 8;
    menu->redraw = 1;
    return;
  }
  if (selection == 7 && (pressed & HidNpadButton_A)) {
    menu->overlay_sidebar_focus = 0;
    select_page(menu, MENU_OVERLAY_SIDEBAR);
    return;
  }
  if (selection == 1 && menu->config->layout == DRASTIC_LAYOUT_CUSTOM &&
      (pressed & (HidNpadButton_Left | HidNpadButton_Right |
                  HidNpadButton_L | HidNpadButton_R | HidNpadButton_A))) {
    menu->editor_screen = 0;
    drastic_config_calculate_layout(menu->config, panel_width, panel_height);
    select_page(menu, MENU_LAYOUT_EDITOR);
    return;
  }
  if (selection == 5 &&
      (pressed & (HidNpadButton_Left | HidNpadButton_Right |
                   HidNpadButton_L | HidNpadButton_R | HidNpadButton_A))) {
    if (!prefs_get_bool("Wrapper/LSFGAllowed", false)) {
      set_status(menu, "请在 GBAStation 的 DraStic 核心设置中启用帧生成");
      menu->redraw = 1;
      return;
    }
    const int configured = prefs_get_bool("Wrapper/LSFGEnabled", false);
    if (configured) {
      save_bool("Wrapper/LSFGEnabled", 0);
      (void)drastic_renderer_lsfg_request_enabled(false);
      set_status(menu, "帧生成已关闭");
    } else if (!drastic_renderer_lsfg_dll_available()) {
      set_status(menu, "缺少 sdmc:/GBAStation/drastic/lsfg/Lossless.dll");
    } else {
      save_bool("Wrapper/LSFGEnabled", 1);
      if (drastic_renderer_lsfg_available() &&
          drastic_renderer_lsfg_request_enabled(true))
        set_status(menu, "帧生成已开启");
      else
        set_status(menu, "已保存；退出游戏并重新启动后启用帧生成");
    }
    menu->redraw = 1;
    return;
  }
  if (selection == 6 &&
      (pressed & (HidNpadButton_Left | HidNpadButton_Right |
                   HidNpadButton_L | HidNpadButton_R | HidNpadButton_A))) {
    const int direction = (pressed & (HidNpadButton_Left | HidNpadButton_L)) ? -1 :
                          (pressed & (HidNpadButton_Right | HidNpadButton_R)) ? 1 : 0;
    begin_filter_picker(menu, 0, direction);
    return;
  }
  const int direction = change_direction(pressed);
  if (!direction) return;
  switch (selection) {
    case 0:
      menu->config->layout = (DrasticLayoutMode)(
          ((int)menu->config->layout + direction + 7) % 7);
      save_string("Wrapper/Layout", layout_value(menu->config->layout));
      break;
    case 2:
      menu->config->screen_gap = clamp_int(
          menu->config->screen_gap + direction * 2, 0, 128);
      save_int("Wrapper/ScreenGap", menu->config->screen_gap);
      break;
    case 3:
      menu->config->integer_scale ^= 1;
      save_bool("Wrapper/IntegerScale", menu->config->integer_scale);
      break;
    case 4:
      menu->config->rotation = (menu->config->rotation + direction + 4) % 4;
      save_int("Wrapper/Rotation", menu->config->rotation);
      overlay_set_rotation(menu->config->rotation);
      break;
    default: return;
  }
  drastic_config_calculate_layout(menu->config, panel_width, panel_height);
  (void)gamedb_save_display_current(menu->config->rom_path, menu->config);
  menu->redraw = 1;
}

static void update_overlay_sidebar(DrasticIngameMenu *menu, u64 pressed) {
  if (pressed & HidNpadButton_B) {
    save_bool("Wrapper/OverlayEnabled", menu->config->overlay_enabled);
    save_string("Wrapper/OverlayPath", menu->config->overlay_path);
    (void)gamedb_save_display_current(menu->config->rom_path, menu->config);
    select_page(menu, MENU_DISPLAY);
    set_status(menu, "遮罩设置已保存");
    return;
  }
  if (pressed & (HidNpadButton_Up | HidNpadButton_Down)) {
    menu->overlay_sidebar_focus ^= 1;
    menu->redraw = 1;
  }
  if (menu->overlay_sidebar_focus == 0 &&
      (pressed & (HidNpadButton_A | HidNpadButton_Left | HidNpadButton_Right |
                  HidNpadButton_L | HidNpadButton_R))) {
    if (!menu->config->overlay_path[0]) {
      set_status(menu, "请先选择 PNG 遮罩文件");
      return;
    }
    menu->config->overlay_enabled ^= 1;
    if (!overlay_set_png_mask(menu->config->overlay_path,
                              menu->config->overlay_enabled)) {
      menu->config->overlay_enabled = 0;
      set_status(menu, "无法加载 PNG 遮罩");
      return;
    }
    menu->redraw = 1;
    return;
  }
  if (menu->overlay_sidebar_focus == 1 && (pressed & HidNpadButton_A)) {
    select_page(menu, MENU_OVERLAY_PICKER);
  }
}

static void update_overlay_picker(DrasticIngameMenu *menu, u64 pressed) {
  if (menu->overlay_preview_visible &&
      (pressed & (HidNpadButton_A | HidNpadButton_B | HidNpadButton_X |
                  HidNpadButton_Up | HidNpadButton_Down))) {
    menu->overlay_preview_visible = 0;
    menu->redraw = 1;
    return;
  }
  if (pressed & HidNpadButton_B) {
    select_page(menu, MENU_OVERLAY_SIDEBAR);
    return;
  }
  if (pressed & HidNpadButton_X) {
    if (menu->overlay_file_count &&
        !menu->overlay_file_is_directory[menu->overlay_picker_index])
      menu->overlay_preview_visible = 1;
    else
      set_status(menu, "文件夹无法预览，请选择 PNG 文件");
    menu->redraw = 1;
    return;
  }
  if (!menu->overlay_file_count) return;
  if (pressed & HidNpadButton_Up)
    menu->overlay_picker_index = (menu->overlay_picker_index +
                                  menu->overlay_file_count - 1) % menu->overlay_file_count;
  if (pressed & HidNpadButton_Down)
    menu->overlay_picker_index = (menu->overlay_picker_index + 1) % menu->overlay_file_count;
  if (pressed & (HidNpadButton_Up | HidNpadButton_Down))
    menu->redraw = 1;
  if (!(pressed & HidNpadButton_A)) return;
  const int item = menu->overlay_picker_index;
  const char *path = menu->overlay_files[item];
  if (menu->overlay_file_is_directory[item]) {
    refresh_overlay_files_in_directory(menu, path);
    menu->overlay_preview_visible = 0;
    menu->redraw = 1;
    return;
  }
  if (!overlay_set_png_mask(path, true)) {
    set_status(menu, "无法读取该 PNG 遮罩");
    return;
  }
  snprintf(menu->config->overlay_path, sizeof(menu->config->overlay_path), "%s", path);
  menu->config->overlay_enabled = 1;
  save_string("Wrapper/OverlayPath", menu->config->overlay_path);
  save_bool("Wrapper/OverlayEnabled", 1);
  select_page(menu, MENU_OVERLAY_SIDEBAR);
  set_status(menu, "已选择 PNG 遮罩");
}

static void update_emulation(DrasticIngameMenu *menu, u64 pressed) {
  navigate_list(menu, 11, pressed);
  if (pressed & HidNpadButton_B) {
    leave_content_focus(menu);
    return;
  }
  const int selection = menu->selection[MENU_EMULATION];
  const int direction = change_direction(pressed);
  if (!direction) return;
  switch (selection) {
    case 0:
      save_int("Drastic/FrameskipValue", clamp_int(
          prefs_get_int("Drastic/FrameskipValue", 0) + direction, 0, 9));
      break;
    case 1:
      save_int("Drastic/FrameskipType",
               (prefs_get_int("Drastic/FrameskipType", 0) + direction + 4) % 4);
      break;
    case 2: save_bool("Drastic/FrameskipSafe",
                      !prefs_get_bool("Drastic/FrameskipSafe", false)); break;
    case 3: {
      static const int speeds[] = {0, 1, 2, 3, 4, 5};
      const int current = prefs_get_int("Drastic/FastForwardSpeed", 5);
      int index = 0;
      for (int i = 0; i < 6; i++) if (speeds[i] == current) index = i;
      index = (index + direction + 6) % 6;
      save_int("Drastic/FastForwardSpeed", speeds[index]);
      break;
    }
    case 4:
      menu->config->volume = clamp_int(menu->config->volume + direction * 5, 0, 100);
      save_int("Wrapper/Volume", menu->config->volume);
      if (menu->core.set_audio_volume)
        menu->core.set_audio_volume(menu->core.env, menu->core.clazz, menu->config->volume);
      opensles_set_master_volume((unsigned)menu->config->volume);
      break;
    case 5: save_bool("Drastic/SoundEnabled", !prefs_get_bool("Drastic/SoundEnabled", true)); break;
    case 6:
      menu->config->microphone_enabled ^= 1;
      save_bool("Drastic/MicEnabled", menu->config->microphone_enabled);
      opensles_set_microphone_enabled(menu->config->microphone_enabled != 0);
      break;
    case 7:
      menu->config->microphone_source = menu->config->microphone_source == DRASTIC_MICROPHONE_EXTERNAL ? DRASTIC_MICROPHONE_SIMULATED : DRASTIC_MICROPHONE_EXTERNAL;
      save_string("Wrapper/MicrophoneSource", menu->config->microphone_source == DRASTIC_MICROPHONE_EXTERNAL ? "external" : "noise");
      opensles_set_microphone_source(menu->config->microphone_source == DRASTIC_MICROPHONE_EXTERNAL ? OPENSLES_MIC_SOURCE_EXTERNAL : OPENSLES_MIC_SOURCE_SIMULATED);
      break;
    case 8: save_int("Drastic/MicLevel", (prefs_get_int("Drastic/MicLevel", 1) + direction + 4) % 4); break;
    case 9:
      menu->config->vibration ^= 1; save_bool("Wrapper/Vibration", menu->config->vibration); break;
    case 10:
      menu->config->motion ^= 1; save_bool("Wrapper/Motion", menu->config->motion);
      break;
    default: return;
  }
  apply_core_config(menu);
  menu->redraw = 1;
}

static void update_audio_input(DrasticIngameMenu *menu, u64 pressed) {
  navigate_list(menu, 10, pressed);
  if (pressed & HidNpadButton_B) {
    leave_content_focus(menu);
    return;
  }
  const int selection = menu->selection[MENU_AUDIO_INPUT];
  if (selection == 9 && (pressed & HidNpadButton_A)) {
    leave_content_focus(menu);
    return;
  }
  const int direction = change_direction(pressed);
  if (!direction) return;
  switch (selection) {
    case 0:
      menu->config->volume = clamp_int(menu->config->volume + direction * 5,
                                       0, 100);
      save_int("Wrapper/Volume", menu->config->volume);
      if (menu->core.set_audio_volume)
        menu->core.set_audio_volume(menu->core.env, menu->core.clazz,
                                    menu->config->volume);
      opensles_set_master_volume((unsigned)menu->config->volume);
      break;
    case 1: save_bool("Drastic/SoundEnabled",
                      !prefs_get_bool("Drastic/SoundEnabled", true)); break;
    case 2:
      menu->config->microphone_enabled ^= 1;
      save_bool("Drastic/MicEnabled", menu->config->microphone_enabled);
      opensles_set_microphone_enabled(menu->config->microphone_enabled != 0);
      break;
    case 3:
      menu->config->microphone_source =
          menu->config->microphone_source == DRASTIC_MICROPHONE_EXTERNAL
              ? DRASTIC_MICROPHONE_SIMULATED
              : DRASTIC_MICROPHONE_EXTERNAL;
      save_string("Wrapper/MicrophoneSource",
                  menu->config->microphone_source ==
                          DRASTIC_MICROPHONE_EXTERNAL
                      ? "external" : "noise");
      opensles_set_microphone_source(
          menu->config->microphone_source == DRASTIC_MICROPHONE_EXTERNAL
              ? OPENSLES_MIC_SOURCE_EXTERNAL
              : OPENSLES_MIC_SOURCE_SIMULATED);
      set_status(menu,
                 menu->config->microphone_source ==
                         DRASTIC_MICROPHONE_EXTERNAL
                     ? "已选择外接输入；请连接耳麦或 USB 麦克风"
                     : "已选择模拟噪声麦克风");
      break;
    case 4:
      save_int("Drastic/MicLevel",
               (prefs_get_int("Drastic/MicLevel", 1) + direction + 4) % 4);
      break;
    case 5:
      menu->config->vibration ^= 1;
      save_bool("Wrapper/Vibration", menu->config->vibration);
      break;
    case 6:
      menu->config->motion ^= 1;
      save_bool("Wrapper/Motion", menu->config->motion);
      break;
    case 7:
      menu->config->stylus_mode = (DrasticStylusMode)(
          ((int)menu->config->stylus_mode + direction + 3) % 3);
      save_string("Wrapper/StylusMode",
                  menu->config->stylus_mode == DRASTIC_STYLUS_OFF ? "off" :
                  menu->config->stylus_mode == DRASTIC_STYLUS_MOTION
                      ? "motion" : "stick");
      if (menu->config->stylus_mode == DRASTIC_STYLUS_MOTION)
        set_status(menu, "体感触笔会以当前手柄角度作为中心");
      break;
    case 8:
      menu->config->mouse_stylus ^= 1;
      save_bool("Wrapper/MouseStylus", menu->config->mouse_stylus);
      break;
    default: return;
  }
  if (selection == 1 || selection == 2 || selection == 4)
    apply_core_config(menu);
  menu->redraw = 1;
}

static void reset_custom_layout(DrasticRuntimeConfig *config) {
  config->custom_top_scale = 1.0f;
  config->custom_top_offset_x = 0.0f;
  config->custom_top_offset_y = 0.0f;
  config->custom_bottom_scale = 1.0f;
  config->custom_bottom_offset_x = 0.0f;
  config->custom_bottom_offset_y = 0.0f;
}

static void save_custom_layout(DrasticIngameMenu *menu) {
  save_float("Wrapper/CustomTopScale", menu->config->custom_top_scale);
  save_float("Wrapper/CustomTopOffsetX", menu->config->custom_top_offset_x);
  save_float("Wrapper/CustomTopOffsetY", menu->config->custom_top_offset_y);
  save_float("Wrapper/CustomBottomScale", menu->config->custom_bottom_scale);
  save_float("Wrapper/CustomBottomOffsetX", menu->config->custom_bottom_offset_x);
  save_float("Wrapper/CustomBottomOffsetY", menu->config->custom_bottom_offset_y);
  save_string("Wrapper/Layout", "custom");
  (void)gamedb_save_display_current(menu->config->rom_path, menu->config);
}

static void update_layout_editor(DrasticIngameMenu *menu, u64 held,
                                 u64 pressed, HidAnalogStickState left,
                                 HidAnalogStickState right) {
  (void)held;
  (void)left;
  (void)right;
  if (pressed & HidNpadButton_B) {
    menu->config->layout = DRASTIC_LAYOUT_CUSTOM;
    save_custom_layout(menu);
    drastic_config_calculate_layout(menu->config, panel_width, panel_height);
    select_page(menu, MENU_DISPLAY);
    set_status(menu, "已为当前游戏保存自定义布局");
    return;
  }
  if (pressed & HidNpadButton_A) {
    float *value = NULL;
    float default_value = 0.0f;
    switch (menu->editor_screen) {
      case 0: value = &menu->config->custom_top_scale; default_value = 1.0f; break;
      case 1: value = &menu->config->custom_top_offset_x; break;
      case 2: value = &menu->config->custom_top_offset_y; break;
      case 3: value = &menu->config->custom_bottom_scale; default_value = 1.0f; break;
      case 4: value = &menu->config->custom_bottom_offset_x; break;
      case 5: value = &menu->config->custom_bottom_offset_y; break;
    }
    if (value) *value = default_value;
    drastic_config_calculate_layout(menu->config, panel_width, panel_height);
    menu->redraw = 1;
    return;
  }
  if (pressed & HidNpadButton_Up) {
    menu->editor_screen = (menu->editor_screen + 5) % 6;
    menu->redraw = 1;
  }
  if (pressed & HidNpadButton_Down) {
    menu->editor_screen = (menu->editor_screen + 1) % 6;
    menu->redraw = 1;
  }
  if (pressed & HidNpadButton_Y) {
    reset_custom_layout(menu->config);
    drastic_config_calculate_layout(menu->config, panel_width, panel_height);
    menu->redraw = 1;
    return;
  }
  const int direction = (pressed & HidNpadButton_L) ? -1 :
                        (pressed & HidNpadButton_R) ? 1 : 0;
  if (direction) {
    float *value = NULL;
    const int is_scale = menu->editor_screen == 0 || menu->editor_screen == 3;
    switch (menu->editor_screen) {
      case 0: value = &menu->config->custom_top_scale; break;
      case 1: value = &menu->config->custom_top_offset_x; break;
      case 2: value = &menu->config->custom_top_offset_y; break;
      case 3: value = &menu->config->custom_bottom_scale; break;
      case 4: value = &menu->config->custom_bottom_offset_x; break;
      case 5: value = &menu->config->custom_bottom_offset_y; break;
    }
    if (!value) return;
    *value = is_scale ? clamp_float(*value + direction * 0.1f, 1.0f, 10.0f)
                      : clamp_float(*value + direction, -1280.0f, 1280.0f);
    drastic_config_calculate_layout(menu->config, panel_width, panel_height);
    menu->redraw = 1;
  }
}

DrasticIngameMenu *drastic_menu_create(DrasticRuntimeConfig *config,
                                       const DrasticMenuCore *core,
                                       int *state_slot) {
  if (!config || !core || !state_slot) return NULL;
  DrasticIngameMenu *menu = calloc(1, sizeof(*menu));
  if (!menu) return NULL;
  menu->config = config;
  menu->core = *core;
  menu->state_slot = state_slot;
  menu->confirm_delete_slot = -1;
  menu->pending_snapshot_slot = -1;
  menu->snapshot_top_array = jni_make_int_array(256 * 192);
  menu->snapshot_bottom_array = jni_make_int_array(256 * 192);
  menu->snapshot_top = jni_int_array_data(menu->snapshot_top_array);
  menu->snapshot_bottom = jni_int_array_data(menu->snapshot_bottom_array);
  menu->microphone_status = opensles_get_microphone_status();
  refresh_custom_shaders(menu);
  return menu;
}

void drastic_menu_destroy(DrasticIngameMenu *menu) {
  if (!menu) return;
  free_cheats(menu);
  jni_release_int_array(menu->snapshot_top_array);
  jni_release_int_array(menu->snapshot_bottom_array);
  free(menu);
}

void drastic_menu_open(DrasticIngameMenu *menu) {
  if (!menu || menu->open) return;
  menu->open = 1;
  menu->page = MENU_MAIN;
  menu->content_focused = 0;
  menu->selection[MENU_MAIN] = MAIN_RESUME;
  menu->status[0] = '\0';
  const u64 frequency = armGetSystemTickFreq();
  menu->hint_until = frequency ? armGetSystemTick() + frequency * 3 : 0;
  reset_analog_navigation(menu);
  if (menu->core.pause_system)
    menu->core.pause_system(menu->core.env, menu->core.clazz, 1);
  menu->redraw = 1;
  render_menu(menu);
}

void drastic_menu_close(DrasticIngameMenu *menu, bool resume_core) {
  if (!menu || !menu->open) return;
  menu->open = 0;
  overlay_hide();
  if (resume_core && menu->core.pause_system)
    menu->core.pause_system(menu->core.env, menu->core.clazz, 0);
}

bool drastic_menu_is_open(const DrasticIngameMenu *menu) {
  return menu && menu->open;
}

static void update_marquee(DrasticIngameMenu *menu) {
  /* Display owns the only scrolling values.  Do not keep redrawing it while
   * it is merely being previewed from the sidebar; that was the source of the
   * page-specific 30 Hz background flash. */
  const int active = menu->content_focused &&
      (menu->page == MENU_DISPLAY ||
       (menu->page == MENU_FILTER_PICKER && menu->filter_picker_custom));
  if (!active) {
    menu->marquee_tick = 0;
    return;
  }
  const u64 frequency = armGetSystemTickFreq();
  const u64 now = armGetSystemTick();
  const u64 interval = frequency / 30u;
  if (!menu->marquee_tick || now - menu->marquee_tick >= interval) {
    menu->marquee_tick = now;
    menu->redraw = 1;
  }
}

void drastic_menu_update(DrasticIngameMenu *menu, u64 held, u64 pressed,
                          HidAnalogStickState left,
                          HidAnalogStickState right) {
  if (!menu || !menu->open) return;
  const u64 frequency = armGetSystemTickFreq();
  const u64 now = armGetSystemTick();
  if (menu->status[0] && menu->status_until && frequency && now >= menu->status_until) {
    menu->status[0] = '\0';
    menu->redraw = 1;
  }
  if (menu->hint_until && frequency && now >= menu->hint_until) {
    menu->hint_until = 0;
    menu->redraw = 1;
  }
  drastic_menu_poll(menu);
  if (menu->page == MENU_AUDIO_INPUT) {
    const OpenSLESMicrophoneStatus microphone_status =
        opensles_get_microphone_status();
    if (microphone_status != menu->microphone_status) {
      menu->microphone_status = microphone_status;
      menu->redraw = 1;
    }
  }
  /* The same bounded repeat applies in the layout inspector as in every
   * other menu page, so holding Up/Down never requires repeated taps. */
  pressed |= held_navigation_pressed(menu, held, left);
  if (menu->content_focused &&
      (menu->page == MENU_DISPLAY || menu->page == MENU_EMULATION ||
       menu->page == MENU_LAYOUT_EDITOR || menu->page == MENU_OVERLAY_SIDEBAR))
    pressed |= selector_repeat_pressed(menu, held);
  else if (!(held & (HidNpadButton_L | HidNpadButton_R)))
    menu->selector_repeat_direction = 0;
  if (!menu->content_focused) {
    update_main(menu, pressed);
  } else {
    switch (menu->page) {
      case MENU_SAVE_STATES:
      case MENU_LOAD_STATES: update_states(menu, pressed); break;
      case MENU_CHEATS: update_cheats(menu, pressed); break;
      case MENU_DISPLAY: update_display(menu, pressed); break;
      case MENU_OVERLAY_SIDEBAR: update_overlay_sidebar(menu, pressed); break;
      case MENU_OVERLAY_PICKER: update_overlay_picker(menu, pressed); break;
      case MENU_FILTER_PICKER: update_filter_picker(menu, pressed); break;
      case MENU_EMULATION: update_emulation(menu, pressed); break;
      case MENU_AUDIO_INPUT: update_audio_input(menu, pressed); break;
      case MENU_LAYOUT_EDITOR:
        update_layout_editor(menu, held, pressed, left, right); break;
      default: update_main(menu, pressed); break;
    }
  }
  update_marquee(menu);
  if (menu->open && menu->redraw) render_menu(menu);
}

bool drastic_menu_take_exit_request(DrasticIngameMenu *menu) {
  if (!menu || !menu->exit_requested) return false;
  menu->exit_requested = 0;
  return true;
}
