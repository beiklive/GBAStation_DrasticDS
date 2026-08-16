#include <switch.h>

#include <ctype.h>
#include <dirent.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "drastic_renderer.h"
#include "ingame_menu.h"
#include "jni_fake.h"
#include "opensles.h"
#include "overlay.h"
#include "prefs.h"

#define MENU_CHEAT_LIMIT 4096
#define MENU_FOLDER_LIMIT 128
#define MENU_STICK_ACTIVATION 18000
#define MENU_STICK_RELEASE 8000
#define MENU_STICK_REPEAT_DELAY_MS 360
#define MENU_STICK_REPEAT_RATE_MS 85

enum MenuPage {
  MENU_MAIN,
  MENU_STATES,
  MENU_CHEATS,
  MENU_DISPLAY,
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
} MenuCheat;

struct DrasticIngameMenu {
  DrasticRuntimeConfig *config;
  DrasticMenuCore core;
  int *state_slot;
  enum MenuPage page;
  int selection[MENU_PAGE_COUNT];
  int scroll[MENU_PAGE_COUNT];
  int open;
  int exit_requested;
  int redraw;
  int pending_snapshot;
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
  char status[192];
  DrasticVideoFilter filter_backup;
  char filter_backup_shader[DRASTIC_CUSTOM_SHADER_PATH_MAX];
  DrasticCustomShaderEntry custom_shaders[128];
  int custom_shader_count;
  int filter_picker_index;
  int filter_picker_custom;
  int filter_picker_valid;
  u64 marquee_tick;
  u64 analog_nav_direction;
  u64 analog_nav_since;
  u64 analog_nav_last;
  OpenSLESMicrophoneStatus microphone_status;
  DrasticLayoutMode editor_old_layout;
  DrasticScreenRect editor_backup[2];
  int editor_screen;
};

/* Shared with the GBAStation NDS stub: dark blue glass panels, soft borders,
 * and a bright cyan focus outline. */
static const uint32_t COLOR_DIM = 0xb60a1019u;
static const uint32_t COLOR_PANEL = 0xe9162434u;
static const uint32_t COLOR_PANEL_ALT = 0xd52a4058u;
static const uint32_t COLOR_ACCENT = 0xff52bfffu;
static const uint32_t COLOR_SELECTED = 0xd92a6c9eu;
static const uint32_t COLOR_TEXT = 0xfff6fbffu;
static const uint32_t COLOR_MUTED = 0xffa9b9c9u;
static const uint32_t COLOR_GOOD = 0xff66d99au;
static const uint32_t COLOR_WARN = 0xffffc857u;

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
  menu->analog_nav_since = 0;
  menu->analog_nav_last = 0;
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

static u64 analog_navigation_pressed(DrasticIngameMenu *menu,
                                     HidAnalogStickState stick) {
  const u64 direction = analog_navigation_direction(menu, stick);
  const u64 now = armGetSystemTick();
  if (direction != menu->analog_nav_direction) {
    menu->analog_nav_direction = direction;
    menu->analog_nav_since = now;
    menu->analog_nav_last = now;
    return direction;
  }
  if (!direction) return 0;

  const u64 frequency = armGetSystemTickFreq();
  if (!frequency) return 0;
  const u64 delay = frequency * MENU_STICK_REPEAT_DELAY_MS / 1000u;
  const u64 rate = frequency * MENU_STICK_REPEAT_RATE_MS / 1000u;
  if (now - menu->analog_nav_since < delay ||
      now - menu->analog_nav_last < rate)
    return 0;
  menu->analog_nav_last = now;
  return direction;
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
static int menu_content_height(void) { return ui_height() - 200; }

static void set_status(DrasticIngameMenu *menu, const char *status) {
  snprintf(menu->status, sizeof(menu->status), "%s", status ? status : "");
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

static void refresh_snapshot(DrasticIngameMenu *menu) {
  if (!menu->core.get_snapshots || !menu->snapshot_top ||
      !menu->snapshot_bottom) {
    menu->snapshot_valid = 0;
    return;
  }
  memset(menu->snapshot_top, 0, 256 * 192 * sizeof(*menu->snapshot_top));
  memset(menu->snapshot_bottom, 0, 256 * 192 * sizeof(*menu->snapshot_bottom));
  menu->core.get_snapshots(menu->core.env, menu->core.clazz,
                           *menu->state_slot, menu->snapshot_top_array,
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
  DIR *directory = opendir(SAVESTATES_DIR);
  struct dirent *entry;
  while (directory && (entry = readdir(directory))) {
    if (!state_filename_matches_slot(entry->d_name, slot)) continue;
    char path[1200];
    snprintf(path, sizeof(path), "%s/%s", SAVESTATES_DIR, entry->d_name);
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
  jni_release_int_array(top_array);
  jni_release_int_array(bottom_array);
  return deleted;
}

static void free_cheats(DrasticIngameMenu *menu) {
  free(menu->cheats);
  menu->cheats = NULL;
  menu->cheat_count = 0;
  menu->folder_count = 0;
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
  if (!prefs_contains("Wrapper/EnabledDatabaseCheats") ||
      !menu->core.get_cheat_count || !menu->core.set_cheat_enabled) return;
  const char *enabled = prefs_get_string("Wrapper/EnabledDatabaseCheats", "");
  const int count = clamp_int(
      menu->core.get_cheat_count(menu->core.env, menu->core.clazz),
      0, MENU_CHEAT_LIMIT);
  for (int index = 0; index < count; index++)
    menu->core.set_cheat_enabled(menu->core.env, menu->core.clazz, index,
                                 enabled_list_contains(enabled, index));
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

static void refresh_cheats(DrasticIngameMenu *menu) {
  drastic_menu_apply_persisted_cheats(menu);
  free_cheats(menu);
  const int database_count = menu->core.get_cheat_count
      ? clamp_int(menu->core.get_cheat_count(menu->core.env, menu->core.clazz),
                  0, MENU_CHEAT_LIMIT)
      : 0;
  const int custom_count = menu->core.get_custom_cheat_count
      ? clamp_int(menu->core.get_custom_cheat_count(menu->core.env,
                                                    menu->core.clazz),
                  0, MENU_CHEAT_LIMIT)
      : 0;
  const int total = clamp_int(database_count + custom_count,
                              0, MENU_CHEAT_LIMIT);
  if (total) menu->cheats = calloc((size_t)total, sizeof(*menu->cheats));
  if (total && !menu->cheats) {
    set_status(menu, "内存不足，无法加载金手指列表");
    return;
  }
  menu->folder_count = menu->core.get_cheat_folder_count
      ? clamp_int(menu->core.get_cheat_folder_count(menu->core.env,
                                                    menu->core.clazz),
                  0, MENU_FOLDER_LIMIT)
      : 0;
  for (int index = 0; index < menu->folder_count; index++)
    {
      menu->folder_multi_select[index] =
          !menu->core.get_cheat_folder_multi_select ||
          menu->core.get_cheat_folder_multi_select(
              menu->core.env, menu->core.clazz, index);
      if (menu->core.get_cheat_folder_name)
      copy_java_bytes(menu->core.get_cheat_folder_name(
                          menu->core.env, menu->core.clazz, index),
                      menu->folders[index], sizeof(menu->folders[index]));
    }

  int output = 0;
  for (int index = 0; index < database_count && output < total; index++) {
    MenuCheat *cheat = &menu->cheats[output++];
    cheat->index = index;
    cheat->folder = menu->core.get_cheat_folder_id
        ? menu->core.get_cheat_folder_id(menu->core.env, menu->core.clazz,
                                         index)
        : -1;
    cheat->enabled = menu->core.get_cheat_enabled &&
        menu->core.get_cheat_enabled(menu->core.env, menu->core.clazz, index);
    if (menu->core.get_cheat_name)
      copy_java_bytes(menu->core.get_cheat_name(menu->core.env,
                                                menu->core.clazz, index),
                      cheat->name, sizeof(cheat->name));
    if (menu->core.get_cheat_note)
      copy_java_bytes(menu->core.get_cheat_note(menu->core.env,
                                                menu->core.clazz, index),
                      cheat->note, sizeof(cheat->note));
    if (!cheat->name[0]) snprintf(cheat->name, sizeof(cheat->name),
                                  "Cheat %d", index + 1);
  }
  for (int index = 0; index < custom_count && output < total; index++) {
    MenuCheat *cheat = &menu->cheats[output++];
    cheat->index = index;
    cheat->folder = -1;
    cheat->custom = 1;
    cheat->enabled = menu->core.get_custom_cheat_enabled &&
        menu->core.get_custom_cheat_enabled(menu->core.env, menu->core.clazz,
                                            index);
    if (menu->core.get_custom_cheat_name)
      copy_java_bytes(menu->core.get_custom_cheat_name(
                          menu->core.env, menu->core.clazz, index),
                      cheat->name, sizeof(cheat->name));
    if (!cheat->name[0]) snprintf(cheat->name, sizeof(cheat->name),
                                  "Custom cheat %d", index + 1);
  }
  menu->cheat_count = output;
  const int maximum = menu->cheat_count; /* + Add entry at index zero. */
  menu->selection[MENU_CHEATS] =
      clamp_int(menu->selection[MENU_CHEATS], 0, maximum);
  menu->redraw = 1;
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
  refresh_cheats(menu);
  set_status(menu, result ? "已添加自定义金手指" :
                            "DraStic 拒绝了此自定义金手指");
}

static int active_main_tab(const DrasticIngameMenu *menu) {
  switch (menu->page) {
    case MENU_STATES: return 1;
    case MENU_CHEATS: return 2;
    case MENU_DISPLAY: return 3;
    case MENU_EMULATION: return 4;
    case MENU_AUDIO_INPUT: return 5;
    default: return menu->selection[MENU_MAIN];
  }
}

static void draw_shell(const DrasticIngameMenu *menu, const char *title,
                       const char *help) {
  static const char *tabs[] = {
    "返回游戏", "即时存档", "金手指", "画面设置",
    "模拟设置", "音频与输入", "重置游戏", "退出模拟器",
  };
  const int width = ui_width();
  const int height = ui_height();
  overlay_fill_rect(0, 0, width, height, COLOR_DIM);
  for (int band = 0; band < 8; band++)
    overlay_fill_rect(0, band * height / 8, width, height / 8,
                      0x0b1b3148u + (uint32_t)(band * 0x03000000u));

  overlay_fill_rect(0, 0, width, 86, 0xef122033u);
  overlay_fill_rect(0, 84, width, 2, 0x7752bfffu);
  overlay_draw_text_scaled(menu_sidebar_x(), 22, 2, COLOR_TEXT, "游戏菜单");
  overlay_draw_text_right(width - 42, 32, COLOR_MUTED, title);

  const int side_x = menu_sidebar_x();
  const int side_y = 116;
  const int side_w = menu_sidebar_width();
  const int active = active_main_tab(menu);
  overlay_fill_rect(side_x, side_y - 10, side_w, height - side_y - 100,
                    0x91132132u);
  overlay_border_rect(side_x, side_y - 10, side_w, height - side_y - 100,
                      1, 0x443f5d78u);
  for (int index = 0; index < 8; index++) {
    const int y = side_y + index * 58 + (index >= 6 ? 16 : 0);
    if (index == 6)
      overlay_fill_rect(side_x + 18, y - 9, side_w - 36, 1, 0x5556748du);
    const int selected = index == active;
    if (selected) {
      overlay_fill_rect(side_x + 10, y, side_w - 20, 48, COLOR_SELECTED);
      overlay_border_rect(side_x + 10, y, side_w - 20, 48, 2, COLOR_ACCENT);
    } else {
      overlay_fill_rect(side_x + 10, y, side_w - 20, 48, 0x161b3045u);
    }
    overlay_draw_text(side_x + 30, y + 16, selected ? COLOR_TEXT : COLOR_MUTED,
                      tabs[index]);
  }

  overlay_fill_rect(menu_content_x(), menu_content_y(), menu_content_width(),
                    menu_content_height(), 0xa9142234u);
  overlay_border_rect(menu_content_x(), menu_content_y(), menu_content_width(),
                      menu_content_height(), 1, 0x554c6f8cu);
  overlay_fill_rect(0, height - 56, width, 56, 0xe8122030u);
  overlay_fill_rect(0, height - 56, width, 1, 0x554b7090u);
  overlay_draw_text_clipped(36, height - 36, width - 72, COLOR_MUTED, help);
}

static void draw_status(const DrasticIngameMenu *menu) {
  if (!menu->status[0]) return;
  const int width = ui_width();
  const int y = ui_height() - 96;
  overlay_fill_rect(24, y, width - 48, 36, 0xe52a4058u);
  overlay_draw_text_clipped(40, y + 10, width - 80,
                            COLOR_WARN, menu->status);
}

static void draw_row(int x, int y, int width, int selected,
                     const char *label, const char *value, int enabled) {
  overlay_fill_rect(x, y, width, 44,
                    selected ? COLOR_SELECTED : 0x261b3045u);
  overlay_border_rect(x, y, width, 44, 1,
                      selected ? COLOR_ACCENT : 0x334c6f8cu);
  overlay_draw_text_clipped(x + 16, y + 14,
                            value ? width - 250 : width - 32,
                            enabled ? COLOR_TEXT : COLOR_MUTED, label);
  if (value)
    overlay_draw_text_right(x + width - 16, y + 14,
                            enabled ? COLOR_ACCENT : COLOR_MUTED, value);
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

enum MainMenuItem {
  MAIN_RESUME,
  MAIN_STATES,
  MAIN_CHEATS,
  MAIN_DISPLAY,
  MAIN_EMULATION,
  MAIN_AUDIO_INPUT,
  MAIN_RESET,
  MAIN_QUIT,
  MAIN_ITEM_COUNT,
};

static int main_item_available(int item) {
  (void)item;
  return 1;
}

static void render_main(DrasticIngameMenu *menu) {
  draw_shell(menu, "主菜单", "A 确定   B 返回游戏");
  const int x = menu_content_x() + 34;
  const int y = menu_content_y() + 42;
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
  draw_shell(menu, "即时存档",
             "A 读取   X 保存/覆盖   Y 删除   B 返回");
  const int saving = menu->core.is_saving &&
      menu->core.is_saving(menu->core.env, menu->core.clazz);
  int saving_slot = -1;
  if (saving && menu->core.get_saving_slot)
    saving_slot = menu->core.get_saving_slot(menu->core.env,
                                              menu->core.clazz);
  const int x = menu_content_x() + 24;
  const int y = menu_content_y() + 22;
  const int width = menu_content_width() - 48;
  overlay_draw_text(x, y, COLOR_TEXT, "选择槽位后：A 读取，X 保存，Y 删除");
  const int columns = ui_is_portrait() ? 1 : 2;
  const int card_width = (width - (columns - 1) * 18) / columns;
  for (int slot = 0; slot < 10; slot++) {
    const int column = slot % columns;
    const int row = slot / columns;
    const int card_x = x + column * (card_width + 18);
    const int card_y = y + 42 + row * 78;
    char label[64];
    snprintf(label, sizeof(label), "槽位 %d%s", slot,
             slot == *menu->state_slot ? "  [当前]" : "");
    draw_row(card_x, card_y, card_width,
             slot == menu->selection[MENU_STATES], label,
             slot == menu->selection[MENU_STATES] && menu->snapshot_valid
                 ? "已有状态" : "空槽", 1);
  }
  if (saving) {
    char label[64];
    snprintf(label, sizeof(label), "正在保存槽位 %d...", saving_slot);
    overlay_draw_text(x, y + menu_content_height() - 38, COLOR_WARN, label);
  }
  draw_status(menu);
}

static void render_cheats(DrasticIngameMenu *menu) {
  draw_shell(menu, "金手指设置",
             "A 添加/开关   X 删除自定义   B 返回");
  const int portrait = ui_is_portrait();
  const int list_x = menu_content_x() + 18;
  const int list_y = menu_content_y() + 18;
  const int list_width = portrait ? menu_content_width() - 36 :
      (menu_content_width() * 56) / 100;
  const int detail_x = portrait ? list_x : list_x + list_width + 18;
  const int detail_y = portrait ? list_y + 310 : list_y;
  const int detail_width = portrait ? list_width :
      menu_content_width() - (detail_x - menu_content_x()) - 18;
  const int detail_height = portrait ? menu_content_height() - 328 :
      menu_content_height() - 36;
  const int content_x = detail_x + 24;
  const int content_y = detail_y + 28;
  const int content_width = detail_width - 48;
  overlay_fill_rect(list_x, list_y, list_width, portrait ? 292 : detail_height,
                    COLOR_PANEL_ALT);
  overlay_fill_rect(detail_x, detail_y, detail_width, detail_height, COLOR_PANEL);
  const int count = menu->cheat_count + 1;
  int selection = clamp_int(menu->selection[MENU_CHEATS], 0, count - 1);
  int scroll = menu->scroll[MENU_CHEATS];
  if (selection < scroll) scroll = selection;
  const int visible_rows = portrait ? 5 : 10;
  if (selection >= scroll + visible_rows) scroll = selection - visible_rows + 1;
  scroll = clamp_int(scroll, 0, count > visible_rows ? count - visible_rows : 0);
  menu->scroll[MENU_CHEATS] = scroll;
  for (int row = 0; row < visible_rows && scroll + row < count; row++) {
    const int item = scroll + row;
    if (!item) {
      draw_row(list_x + 12, list_y + 14 + row * 48, list_width - 24,
               selection == item,
               "+ 添加自定义 Action Replay 金手指", NULL, 1);
      continue;
    }
    MenuCheat *cheat = &menu->cheats[item - 1];
    draw_row(list_x + 12, list_y + 14 + row * 48, list_width - 24,
             selection == item, cheat->name,
             cheat->enabled ? "开" : "关", 1);
  }
  if (!selection) {
    overlay_draw_text(content_x, content_y, COLOR_ACCENT,
                      "自定义金手指编辑器");
    overlay_draw_wrapped(content_x, content_y + 42, content_width,
                         portrait ? 22 : 14, COLOR_MUTED,
        "通过 Switch 软件键盘创建并命名 Action Replay 代码。请输入以空格或换行分隔的十六进制地址/数值对。自定义金手指会由 DraStic 按游戏保存。");
  } else {
    const MenuCheat *cheat = &menu->cheats[selection - 1];
    overlay_draw_text_clipped(content_x, content_y, content_width,
                              COLOR_TEXT, cheat->name);
    overlay_draw_text(content_x, content_y + 40,
                      cheat->enabled ? COLOR_GOOD : COLOR_MUTED,
                      cheat->enabled ? "已启用" : "已关闭");
    overlay_draw_text(content_x, content_y + 78, COLOR_ACCENT,
                      cheat->custom ? "自定义金手指" : "数据库金手指");
    if (!cheat->custom && cheat->folder >= 0 &&
        cheat->folder < menu->folder_count &&
        menu->folders[cheat->folder][0]) {
      overlay_draw_text(content_x, content_y + 116, COLOR_MUTED, "分类：");
      overlay_draw_text_clipped(content_x, content_y + 144, content_width,
                                COLOR_TEXT,
                                menu->folders[cheat->folder]);
    }
    if (cheat->note[0]) {
      overlay_draw_text(content_x, content_y + 192, COLOR_MUTED, "说明：");
      overlay_draw_wrapped(content_x, content_y + 222, content_width,
                           portrait ? 16 : 14, COLOR_TEXT, cheat->note);
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
    "最近邻", "线性", "Quilez", "扫描线", "Scale2x", "HQ2x", "FXAA",
    "FXAA 高质量", "SMAA", "自定义"
  };
  return (unsigned)filter < DRASTIC_FILTER_COUNT ? labels[filter] : labels[0];
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
           menu->filter_picker_index < DRASTIC_FILTER_CUSTOM
        ? filter_label((DrasticVideoFilter)menu->filter_picker_index)
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

static void render_display(DrasticIngameMenu *menu) {
  static const char *labels[] = {
    "画面布局", "交换上下屏", "旋转", "屏幕间距",
    "整数倍缩放", "内置滤镜", "自定义滤镜",
    "自定义画面布局", "返回"
  };
  char values[9][112] = {{0}};
  snprintf(values[0], sizeof(values[0]), "%s", layout_label(menu->config->layout));
  snprintf(values[1], sizeof(values[1]), "%s", menu->config->swap_screens ? "开" : "关");
  snprintf(values[2], sizeof(values[2]), "%d 度", (menu->config->rotation & 3) * 90);
  snprintf(values[3], sizeof(values[3]), "%d px", menu->config->screen_gap);
  snprintf(values[4], sizeof(values[4]), "%s", menu->config->integer_scale ? "开" : "关");
  snprintf(values[5], sizeof(values[5]), "%s",
           filter_label(menu->config->video_filter));
  snprintf(values[6], sizeof(values[6]), "%s",
           custom_shader_name(menu, menu->config->custom_shader));
  draw_shell(menu, "画面设置",
             "左右 调整   A 选择   B 返回");
  const int panel_x = menu_content_x();
  const int panel_width = menu_content_width();
  for (int index = 0; index < 9; index++) {
    const int row_x = panel_x + 24;
    const int row_y = menu_content_y() + 22 + index * 52;
    const int row_width = panel_width - 48;
    const int selected = menu->selection[MENU_DISPLAY] == index;
    if (index == 6)
      draw_row_scrolling_value(row_x, row_y, row_width, selected,
                               labels[index], values[index], 1);
    else
      draw_row(row_x, row_y, row_width, selected, labels[index],
               index < 7 ? values[index] : NULL, 1);
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
    "快进速度", "多线程 3D", "金手指总开关",
    "显示 FPS", "自动存档间隔", "返回"
  };
  static const char *methods[] = {"自动", "固定", "激进", "最大"};
  char values[9][48] = {{0}};
  const int frameskip = prefs_get_int("Drastic/FrameskipValue", 0);
  const int method = clamp_int(prefs_get_int("Drastic/FrameskipType", 0), 0, 3);
  const int ff = clamp_int(prefs_get_int("Drastic/FastForwardSpeed", 2), 0, 5);
  const int autosave = menu->config->autosave_seconds;
  snprintf(values[0], sizeof(values[0]), "%d", frameskip);
  snprintf(values[1], sizeof(values[1]), "%s", methods[method]);
  snprintf(values[2], sizeof(values[2]), "%s", on_off(prefs_get_bool("Drastic/FrameskipSafe", false)));
  static const char *ff_speeds[] = {
    "50%", "150%", "200%", "300%", "400%", "不限"
  };
  snprintf(values[3], sizeof(values[3]), "%s", ff_speeds[ff]);
  snprintf(values[4], sizeof(values[4]), "%s", on_off(prefs_get_bool("Drastic/Threaded3D", true)));
  snprintf(values[5], sizeof(values[5]), "%s", on_off(prefs_get_bool("Drastic/CheatsEnabled", true)));
  snprintf(values[6], sizeof(values[6]), "%s", on_off(menu->config->show_fps));
  if (autosave)
    snprintf(values[7], sizeof(values[7]), "%d 分钟", autosave / 60);
  else
    snprintf(values[7], sizeof(values[7]), "关闭");
  draw_shell(menu, "模拟设置",
             "左右 调整   A 开关   B 返回");
  const int panel_x = menu_content_x();
  const int panel_width = menu_content_width();
  for (int index = 0; index < 9; index++)
    draw_row(panel_x + 24, menu_content_y() + 22 + index * 52, panel_width - 48,
             menu->selection[MENU_EMULATION] == index, labels[index],
             index < 8 ? values[index] : NULL, 1);
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
    draw_row(panel_x + 24, menu_content_y() + 18 + index * 45, panel_width - 48,
             menu->selection[MENU_AUDIO_INPUT] == index, labels[index],
             index < 9 ? values[index] : NULL, 1);
  overlay_draw_wrapped(panel_x + 24, menu_content_y() + menu_content_height() - 60,
                       panel_width - 48,
                       ui_is_portrait() ? 8 : 3, COLOR_MUTED,
      "外接麦克风可使用 CTIA 耳麦或兼容 USB 麦克风。体感触笔使用回中热键；鼠标左键可触摸。");
  draw_status(menu);
}

static void custom_rect_to_ui(const DrasticRuntimeConfig *config,
                              const DrasticScreenRect *rect,
                              DrasticScreenRect *ui) {
  *ui = *rect;
  switch (config->rotation & 3) {
    case 1:
      ui->x = rect->y;
      ui->y = 1.0f - rect->x - rect->width;
      ui->width = rect->height;
      ui->height = rect->width;
      break;
    case 2:
      ui->x = 1.0f - rect->x - rect->width;
      ui->y = 1.0f - rect->y - rect->height;
      break;
    case 3:
      ui->x = 1.0f - rect->y - rect->height;
      ui->y = rect->x;
      ui->width = rect->height;
      ui->height = rect->width;
      break;
    default:
      break;
  }
}

static void custom_rect_from_ui(const DrasticRuntimeConfig *config,
                                const DrasticScreenRect *ui,
                                DrasticScreenRect *rect) {
  const int screen = rect->screen;
  const int touch_target = rect->touch_target;
  switch (config->rotation & 3) {
    case 1:
      rect->x = 1.0f - ui->y - ui->height;
      rect->y = ui->x;
      rect->width = ui->height;
      rect->height = ui->width;
      break;
    case 2:
      rect->x = 1.0f - ui->x - ui->width;
      rect->y = 1.0f - ui->y - ui->height;
      rect->width = ui->width;
      rect->height = ui->height;
      break;
    case 3:
      rect->x = ui->y;
      rect->y = 1.0f - ui->x - ui->width;
      rect->width = ui->height;
      rect->height = ui->width;
      break;
    default:
      rect->x = ui->x;
      rect->y = ui->y;
      rect->width = ui->width;
      rect->height = ui->height;
      break;
  }
  rect->screen = screen;
  rect->touch_target = touch_target;
}

static void custom_rect_for_ui(const DrasticRuntimeConfig *config,
                               const DrasticScreenRect *rect,
                               int *x, int *y, int *width, int *height) {
  DrasticScreenRect ui;
  custom_rect_to_ui(config, rect, &ui);
  *x = (int)(ui.x * overlay_width() + 0.5f);
  *y = (int)(ui.y * overlay_height() + 0.5f);
  *width = (int)(ui.width * overlay_width() + 0.5f);
  *height = (int)(ui.height * overlay_height() + 0.5f);
}

static void render_layout_editor(DrasticIngameMenu *menu) {
  const int canvas_width = ui_width();
  const int canvas_height = ui_height();
  overlay_fill_rect(0, 0, canvas_width, canvas_height, 0x33000000u);
  overlay_fill_rect(0, 0, canvas_width, 70, COLOR_PANEL);
  overlay_draw_text_scaled(24, 16, 2, COLOR_TEXT, "自定义画面布局");
  for (int screen = 0; screen < 2; screen++) {
    const DrasticScreenRect *rect = &menu->config->custom_screens[screen];
    int x, y, width, height;
    custom_rect_for_ui(menu->config, rect, &x, &y, &width, &height);
    const uint32_t color = screen == menu->editor_screen
        ? COLOR_ACCENT : 0xffe7edf2u;
    overlay_border_rect(x, y, width, height, screen == menu->editor_screen ? 6 : 3,
                        color);
    overlay_fill_rect(x + 8, y + 8, 136, 30, 0xd0101820u);
    overlay_draw_text(x + 16, y + 15, color,
                      screen ? "触摸屏" : "上屏");
  }
  overlay_fill_rect(0, canvas_height - 88, canvas_width, 88, COLOR_PANEL);
  overlay_draw_text_clipped(24, canvas_height - 72, canvas_width - 48,
      COLOR_TEXT,
      "左摇杆/方向键：移动    右摇杆/ZL+方向键：调整尺寸");
  overlay_draw_text_clipped(24, canvas_height - 40, canvas_width - 48,
      COLOR_MUTED,
      "X：选择屏幕    Y：重置    A：保存    B：取消");
}

static void render_menu(DrasticIngameMenu *menu) {
  if (!menu || !menu->open) return;
  overlay_begin();
  switch (menu->page) {
    case MENU_STATES: render_states(menu); break;
    case MENU_CHEATS: render_cheats(menu); break;
    case MENU_DISPLAY: render_display(menu); break;
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
  menu->redraw = 1;
  if (page == MENU_STATES) {
    menu->confirm_delete_slot = -1;
    menu->selection[page] = *menu->state_slot;
    refresh_snapshot(menu);
  } else if (page == MENU_CHEATS) {
    refresh_cheats(menu);
  }
}

static void navigate_list(DrasticIngameMenu *menu, int count, u64 pressed) {
  int *selection = &menu->selection[menu->page];
  if (pressed & HidNpadButton_Up)
    *selection = (*selection + count - 1) % count;
  if (pressed & HidNpadButton_Down)
    *selection = (*selection + 1) % count;
  if (pressed & HidNpadButton_L)
    *selection = clamp_int(*selection - 8, 0, count - 1);
  if (pressed & HidNpadButton_R)
    *selection = clamp_int(*selection + 8, 0, count - 1);
  if (pressed & (HidNpadButton_Up | HidNpadButton_Down |
                 HidNpadButton_L | HidNpadButton_R))
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
    menu->redraw = 1;
  }
  if (pressed & HidNpadButton_B) {
    drastic_menu_close(menu, true);
    return;
  }
  if (!(pressed & HidNpadButton_A)) return;
  switch (menu->selection[MENU_MAIN]) {
    case MAIN_RESUME: drastic_menu_close(menu, true); break;
    case MAIN_STATES: select_page(menu, MENU_STATES); break;
    case MAIN_CHEATS: select_page(menu, MENU_CHEATS); break;
    case MAIN_DISPLAY: select_page(menu, MENU_DISPLAY); break;
    case MAIN_EMULATION: select_page(menu, MENU_EMULATION); break;
    case MAIN_AUDIO_INPUT: select_page(menu, MENU_AUDIO_INPUT); break;
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
  const int old = menu->selection[MENU_STATES];
  navigate_list(menu, 10, pressed);
  if (old != menu->selection[MENU_STATES]) {
    menu->confirm_delete_slot = -1;
    *menu->state_slot = menu->selection[MENU_STATES];
    save_int("Wrapper/StateSlot", *menu->state_slot);
    refresh_snapshot(menu);
  }
  if (pressed & HidNpadButton_B) {
    select_page(menu, MENU_MAIN);
    return;
  }
  const int slot = menu->selection[MENU_STATES];
  if ((pressed & HidNpadButton_A) && menu->core.load_state) {
    const int result = menu->core.load_state(menu->core.env, menu->core.clazz,
                                             slot);
    if (result) drastic_menu_close(menu, true);
    else set_status(menu, "无法读取此槽位");
  }
  if ((pressed & HidNpadButton_X) && menu->core.save_state) {
    const int result = menu->core.save_state(menu->core.env, menu->core.clazz,
                                             slot, 1);
    menu->pending_snapshot = result != 0;
    set_status(menu, result ? "已请求保存即时存档" :
                              "无法保存到此槽位");
  }
  if (pressed & HidNpadButton_Y) {
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
  } else if (pressed & (HidNpadButton_A | HidNpadButton_B |
                        HidNpadButton_X)) {
    menu->confirm_delete_slot = -1;
  }
}

static void update_cheats(DrasticIngameMenu *menu, u64 pressed) {
  const int count = menu->cheat_count + 1;
  navigate_list(menu, count, pressed);
  if (pressed & HidNpadButton_B) {
    select_page(menu, MENU_MAIN);
    return;
  }
  const int selected = menu->selection[MENU_CHEATS];
  if ((pressed & HidNpadButton_A) && !selected) {
    add_custom_cheat(menu);
    return;
  }
  if (selected <= 0 || selected > menu->cheat_count) return;
  MenuCheat *cheat = &menu->cheats[selected - 1];
  if (pressed & HidNpadButton_A) {
    cheat->enabled ^= 1;
    if (!cheat->custom && cheat->enabled && cheat->folder >= 0 &&
        cheat->folder < menu->folder_count &&
        !menu->folder_multi_select[cheat->folder]) {
      for (int index = 0; index < menu->cheat_count; index++) {
        MenuCheat *other = &menu->cheats[index];
        if (other == cheat || other->custom ||
            other->folder != cheat->folder || !other->enabled) continue;
        other->enabled = 0;
        if (menu->core.set_cheat_enabled)
          menu->core.set_cheat_enabled(menu->core.env, menu->core.clazz,
                                       other->index, 0);
      }
    }
    if (cheat->custom && menu->core.set_custom_cheat_enabled)
      menu->core.set_custom_cheat_enabled(menu->core.env, menu->core.clazz,
                                          cheat->index, cheat->enabled);
    else if (!cheat->custom && menu->core.set_cheat_enabled)
      menu->core.set_cheat_enabled(menu->core.env, menu->core.clazz,
                                   cheat->index, cheat->enabled);
    if (menu->core.update_cheats)
      menu->core.update_cheats(menu->core.env, menu->core.clazz, 1);
    if (!cheat->custom) persist_database_cheats(menu);
    set_status(menu, cheat->enabled ? "金手指已启用" : "金手指已关闭");
  }
  if ((pressed & HidNpadButton_X) && cheat->custom &&
      menu->core.remove_custom_cheat) {
    menu->core.remove_custom_cheat(menu->core.env, menu->core.clazz,
                                   cheat->index);
    if (menu->core.update_cheats)
      menu->core.update_cheats(menu->core.env, menu->core.clazz, 1);
    refresh_cheats(menu);
    set_status(menu, "自定义金手指已删除");
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
  if (pressed & HidNpadButton_Left) return -1;
  if (pressed & (HidNpadButton_Right | HidNpadButton_A)) return 1;
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
  if (candidate < 0 || candidate >= DRASTIC_FILTER_CUSTOM) return 0;
  menu->config->video_filter = (DrasticVideoFilter)candidate;
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
      ? menu->custom_shader_count : DRASTIC_FILTER_CUSTOM;
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
                           : DRASTIC_FILTER_CUSTOM;
  int current = custom ? configured_custom_shader_index(menu)
                       : (menu->config->video_filter < DRASTIC_FILTER_CUSTOM
                           ? (int)menu->config->video_filter : -1);
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
  navigate_list(menu, 9, pressed);
  if (pressed & HidNpadButton_B) {
    select_page(menu, MENU_MAIN);
    return;
  }
  const int selection = menu->selection[MENU_DISPLAY];
  if (selection == 8 && (pressed & HidNpadButton_A)) {
    select_page(menu, MENU_MAIN);
    return;
  }
  if (selection == 7 && (pressed & HidNpadButton_A)) {
    menu->editor_old_layout = menu->config->layout;
    memcpy(menu->editor_backup, menu->config->custom_screens,
           sizeof(menu->editor_backup));
    menu->editor_screen = 0;
    menu->config->layout = DRASTIC_LAYOUT_CUSTOM;
    drastic_config_calculate_layout(menu->config, panel_width, panel_height);
    select_page(menu, MENU_LAYOUT_EDITOR);
    return;
  }
  if ((selection == 5 || selection == 6) &&
      (pressed & (HidNpadButton_Left | HidNpadButton_Right |
                  HidNpadButton_A))) {
    const int direction = (pressed & HidNpadButton_Left) ? -1 :
                          (pressed & HidNpadButton_Right) ? 1 : 0;
    begin_filter_picker(menu, selection == 6, direction);
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
    case 1:
      menu->config->swap_screens ^= 1;
      save_bool("Wrapper/SwapScreens", menu->config->swap_screens);
      break;
    case 2:
      menu->config->rotation = (menu->config->rotation + direction + 4) % 4;
      save_int("Wrapper/Rotation", menu->config->rotation);
      overlay_set_rotation(menu->config->rotation);
      break;
    case 3:
      menu->config->screen_gap = clamp_int(
          menu->config->screen_gap + direction * 2, 0, 128);
      save_int("Wrapper/ScreenGap", menu->config->screen_gap);
      break;
    case 4:
      menu->config->integer_scale ^= 1;
      save_bool("Wrapper/IntegerScale", menu->config->integer_scale);
      break;
    default: return;
  }
  drastic_config_calculate_layout(menu->config, panel_width, panel_height);
  menu->redraw = 1;
}

static void update_emulation(DrasticIngameMenu *menu, u64 pressed) {
  navigate_list(menu, 9, pressed);
  if (pressed & HidNpadButton_B) {
    select_page(menu, MENU_MAIN);
    return;
  }
  const int selection = menu->selection[MENU_EMULATION];
  if (selection == 8 && (pressed & HidNpadButton_A)) {
    select_page(menu, MENU_MAIN);
    return;
  }
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
      const int current = prefs_get_int("Drastic/FastForwardSpeed", 2);
      int index = 0;
      for (int i = 0; i < 6; i++) if (speeds[i] == current) index = i;
      index = (index + direction + 6) % 6;
      save_int("Drastic/FastForwardSpeed", speeds[index]);
      break;
    }
    case 4: save_bool("Drastic/Threaded3D",
                      !prefs_get_bool("Drastic/Threaded3D", true)); break;
    case 5:
      save_bool("Drastic/CheatsEnabled",
                !prefs_get_bool("Drastic/CheatsEnabled", true));
      if (menu->core.update_cheats)
        menu->core.update_cheats(menu->core.env, menu->core.clazz, 1);
      break;
    case 6:
      menu->config->show_fps ^= 1;
      save_bool("Drastic/ShowFPS", menu->config->show_fps);
      break;
    case 7: {
      static const int intervals[] = {0, 60, 300, 600, 1800};
      int index = 0;
      for (int i = 0; i < 5; i++)
        if (intervals[i] == menu->config->autosave_seconds) index = i;
      index = (index + direction + 5) % 5;
      menu->config->autosave_seconds = intervals[index];
      save_int("Drastic/AutosaveInterval", menu->config->autosave_seconds);
      if (menu->core.set_autosave_interval)
        menu->core.set_autosave_interval(menu->core.env, menu->core.clazz,
                                         menu->config->autosave_seconds);
      break;
    }
    default: return;
  }
  apply_core_config(menu);
  menu->redraw = 1;
}

static void update_audio_input(DrasticIngameMenu *menu, u64 pressed) {
  navigate_list(menu, 10, pressed);
  if (pressed & HidNpadButton_B) {
    select_page(menu, MENU_MAIN);
    return;
  }
  const int selection = menu->selection[MENU_AUDIO_INPUT];
  if (selection == 9 && (pressed & HidNpadButton_A)) {
    select_page(menu, MENU_MAIN);
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
  config->custom_screens[0] = (DrasticScreenRect){
    .x = 0.30f, .y = 0.04f, .width = 0.40f, .height = 0.40f,
    .screen = 0,
  };
  config->custom_screens[1] = (DrasticScreenRect){
    .x = 0.30f, .y = 0.56f, .width = 0.40f, .height = 0.40f,
    .screen = 1, .touch_target = 1,
  };
}

static void constrain_custom_rect(DrasticScreenRect *rect) {
  rect->width = clamp_float(rect->width, 0.08f, 1.0f);
  rect->height = clamp_float(rect->height, 0.08f, 1.0f);
  rect->x = clamp_float(rect->x, 0.0f, 1.0f - rect->width);
  rect->y = clamp_float(rect->y, 0.0f, 1.0f - rect->height);
}

static void save_custom_layout(DrasticIngameMenu *menu) {
  static const char *keys[2][4] = {
    {"Wrapper/CustomTopX", "Wrapper/CustomTopY",
     "Wrapper/CustomTopW", "Wrapper/CustomTopH"},
    {"Wrapper/CustomBottomX", "Wrapper/CustomBottomY",
     "Wrapper/CustomBottomW", "Wrapper/CustomBottomH"},
  };
  for (int screen = 0; screen < 2; screen++) {
    const DrasticScreenRect *rect = &menu->config->custom_screens[screen];
    save_float(keys[screen][0], rect->x);
    save_float(keys[screen][1], rect->y);
    save_float(keys[screen][2], rect->width);
    save_float(keys[screen][3], rect->height);
  }
  save_string("Wrapper/Layout", "custom");
}

static float normalized_axis(int value) {
  if (abs(value) < 6000) return 0.0f;
  return clamp_float((float)value / 32767.0f, -1.0f, 1.0f);
}

static void update_layout_editor(DrasticIngameMenu *menu, u64 held,
                                 u64 pressed, HidAnalogStickState left,
                                 HidAnalogStickState right) {
  if (pressed & HidNpadButton_B) {
    menu->config->layout = menu->editor_old_layout;
    memcpy(menu->config->custom_screens, menu->editor_backup,
           sizeof(menu->editor_backup));
    drastic_config_calculate_layout(menu->config, panel_width, panel_height);
    select_page(menu, MENU_DISPLAY);
    return;
  }
  if (pressed & HidNpadButton_A) {
    menu->config->layout = DRASTIC_LAYOUT_CUSTOM;
    save_custom_layout(menu);
    drastic_config_calculate_layout(menu->config, panel_width, panel_height);
    select_page(menu, MENU_DISPLAY);
    set_status(menu, "已为当前游戏保存自定义布局");
    return;
  }
  if (pressed & HidNpadButton_X) {
    menu->editor_screen ^= 1;
    menu->redraw = 1;
  }
  if (pressed & HidNpadButton_Y) {
    reset_custom_layout(menu->config);
    menu->redraw = 1;
  }
  DrasticScreenRect *rect = &menu->config->custom_screens[menu->editor_screen];
  float move_x = normalized_axis(left.x) * 0.004f;
  float move_y = -normalized_axis(left.y) * 0.004f;
  float size_x = normalized_axis(right.x) * 0.004f;
  float size_y = -normalized_axis(right.y) * 0.004f;
  const float step = (held & HidNpadButton_ZR) ? 0.02f : 0.006f;
  if (held & HidNpadButton_ZL) {
    if (pressed & HidNpadButton_Left) size_x -= step;
    if (pressed & HidNpadButton_Right) size_x += step;
    if (pressed & HidNpadButton_Up) size_y -= step;
    if (pressed & HidNpadButton_Down) size_y += step;
  } else {
    if (pressed & HidNpadButton_Left) move_x -= step;
    if (pressed & HidNpadButton_Right) move_x += step;
    if (pressed & HidNpadButton_Up) move_y -= step;
    if (pressed & HidNpadButton_Down) move_y += step;
  }
  if (move_x || move_y || size_x || size_y) {
    /* Edit the rectangle in the same rotated coordinate space shown by the
     * overlay.  Otherwise at 90 degrees a rightward resize changes height,
     * and at 270 degrees the axes appear inverted. */
    DrasticScreenRect ui;
    custom_rect_to_ui(menu->config, rect, &ui);
    ui.x += move_x;
    ui.y += move_y;
    ui.width += size_x;
    ui.height += size_y;
    constrain_custom_rect(&ui);
    custom_rect_from_ui(menu->config, &ui, rect);
    constrain_custom_rect(rect);
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
  menu->status[0] = '\0';
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
  const int active = menu->page == MENU_DISPLAY ||
      (menu->page == MENU_FILTER_PICKER && menu->filter_picker_custom);
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
  if (menu->pending_snapshot && (!menu->core.is_saving ||
      !menu->core.is_saving(menu->core.env, menu->core.clazz))) {
    menu->pending_snapshot = 0;
    refresh_snapshot(menu);
    set_status(menu, "即时存档完成");
  }
  if (menu->page == MENU_AUDIO_INPUT) {
    const OpenSLESMicrophoneStatus microphone_status =
        opensles_get_microphone_status();
    if (microphone_status != menu->microphone_status) {
      menu->microphone_status = microphone_status;
      menu->redraw = 1;
    }
  }
  if (menu->page == MENU_LAYOUT_EDITOR)
    reset_analog_navigation(menu);
  else
    pressed |= analog_navigation_pressed(menu, left);
  switch (menu->page) {
    case MENU_STATES: update_states(menu, pressed); break;
    case MENU_CHEATS: update_cheats(menu, pressed); break;
    case MENU_DISPLAY: update_display(menu, pressed); break;
    case MENU_FILTER_PICKER: update_filter_picker(menu, pressed); break;
    case MENU_EMULATION: update_emulation(menu, pressed); break;
    case MENU_AUDIO_INPUT: update_audio_input(menu, pressed); break;
    case MENU_LAYOUT_EDITOR:
      update_layout_editor(menu, held, pressed, left, right); break;
    default: update_main(menu, pressed); break;
  }
  update_marquee(menu);
  if (menu->open && menu->redraw) render_menu(menu);
}

bool drastic_menu_take_exit_request(DrasticIngameMenu *menu) {
  if (!menu || !menu->exit_requested) return false;
  menu->exit_requested = 0;
  return true;
}
