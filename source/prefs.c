/* Flat, allocation-free preference store shared by the launcher and host. */

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "config.h"
#include "prefs.h"

#define PREFS_MAX_ENTRIES 512
#define PREFS_KEY_LEN 128
#define PREFS_VAL_LEN 1056

typedef struct {
  char key[PREFS_KEY_LEN];
  char value[PREFS_VAL_LEN];
} PrefEntry;

static PrefEntry entries[PREFS_MAX_ENTRIES];
static int entry_count;
static char ini_path[1024];
static char pending_rom[PREFS_VAL_LEN];

static int find_entry(const char *key) {
  if (!key) return -1;
  for (int index = 0; index < entry_count; index++)
    if (!strcmp(entries[index].key, key)) return index;
  return -1;
}

static void put_entry(const char *key, const char *value) {
  if (!key || !*key || !value) return;
  int index = find_entry(key);
  if (index < 0) {
    if (entry_count >= PREFS_MAX_ENTRIES) return;
    index = entry_count++;
    snprintf(entries[index].key, sizeof(entries[index].key), "%s", key);
  }
  snprintf(entries[index].value, sizeof(entries[index].value), "%s", value);
}

static void seed(const char *key, const char *value) {
  if (find_entry(key) < 0) put_entry(key, value);
}

static char *trim(char *text) {
  while (*text && isspace((unsigned char)*text)) text++;
  char *end = text + strlen(text);
  while (end > text && isspace((unsigned char)end[-1])) --end;
  *end = 0;
  return text;
}

static void parse_file(const char *path) {
  FILE *file = fopen(path, "rb");
  if (!file) return;
  char line[2048];
  while (fgets(line, sizeof(line), file)) {
    char *key = trim(line);
    if (!*key || *key == '#' || *key == ';' || *key == '[') continue;
    char *equals = strchr(key, '=');
    if (!equals) continue;
    *equals++ = 0;
    key = trim(key);
    char *value = trim(equals);
    if (*key) put_entry(key, value);
  }
  fclose(file);
}

/* The launcher owns user-facing core settings in config.cfg.  Import the
 * DraStic subset on every NRO launch so the host never silently reuses an old
 * drastic.ini after the user changes a setting in BeikLiveStation. */
static int launcher_config_value(const char *wanted, char *output,
                                 size_t output_size) {
  static const char *paths[] = {
    "sdmc:/GBAStation/config/config.cfg",
    "/GBAStation/config/config.cfg",
  };
  int found = 0;
  if (!wanted || !output || !output_size) return 0;
  output[0] = '\0';
  for (unsigned path_index = 0;
       path_index < sizeof(paths) / sizeof(*paths); path_index++) {
    FILE *file = fopen(paths[path_index], "rb");
    if (!file) continue;
    char line[2048];
    while (fgets(line, sizeof(line), file)) {
      char *key = trim(line);
      char *equals = strchr(key, '=');
      if (!equals) continue;
      *equals++ = '\0';
      key = trim(key);
      if (strcmp(key, wanted)) continue;
      char *value = trim(equals);
      /* config.cfg stores scalar values as `i|42`, `f|0.5`, or `s|text`. */
      char *separator = strchr(value, '|');
      if (separator && separator > value && separator <= value + 3)
        value = separator + 1;
      snprintf(output, output_size, "%s", trim(value));
      found = 1; /* last record wins, matching the launcher config reader */
    }
    fclose(file);
  }
  return found;
}

static void import_launcher_core_config(void) {
  static const struct {
    const char *launcher_key;
    const char *prefs_key;
  } mappings[] = {
    {"core.drastic.layout", "Wrapper/Layout"},
    {"core.drastic.rotation", "Wrapper/Rotation"},
    {"core.drastic.screen_gap", "Wrapper/ScreenGap"},
    {"core.drastic.integer_scale", "Wrapper/IntegerScale"},
    {"core.drastic.video_filter", "Wrapper/VideoFilter"},
    {"core.drastic.volume", "Wrapper/Volume"},
    {"core.drastic.microphone_source", "Wrapper/MicrophoneSource"},
    {"core.drastic.vibration", "Wrapper/Vibration"},
    {"core.drastic.motion", "Wrapper/Motion"},
    {"core.drastic.stylus_mode", "Wrapper/StylusMode"},
    {"core.drastic.stylus_speed", "Wrapper/AnalogStylusSpeed"},
    {"core.drastic.frameskip", "Drastic/FrameskipValue"},
    {"core.drastic.frameskip_type", "Drastic/FrameskipType"},
    {"core.drastic.frameskip_safe", "Drastic/FrameskipSafe"},
    {"core.drastic.fastforward_speed", "Drastic/FastForwardSpeed"},
    {"core.drastic.audio_latency", "Drastic/AudioLatency"},
    {"core.drastic.cpu_threads", "Drastic/CpuThreads"},
    {"core.drastic.threaded_3d", "Drastic/Threaded3D"},
    {"core.drastic.hires_3d", "Drastic/Hires3D"},
    {"core.drastic.sound_enabled", "Drastic/SoundEnabled"},
    {"core.drastic.cheats_enabled", "Drastic/CheatsEnabled"},
    {"core.drastic.mic_enabled", "Drastic/MicEnabled"},
    {"core.drastic.rtc_system_time", "Drastic/RtcSystemTime"},
    {"core.drastic.preload_roms", "Drastic/PreloadRoms"},
    {"core.drastic.show_fps", "Drastic/ShowFPS"},
    {"core.drastic.autosave_interval", "Drastic/AutosaveInterval"},
    {"core.drastic.autofire_speed", "Drastic/AutoFireSpeed"},
    {"core.drastic.mic_level", "Drastic/MicLevel"},
    {"core.drastic.slot2_type", "Drastic/Slot2Type"},
    {"core.drastic.backup_in_savestates", "Drastic/BackupInSavestates"},
    {"core.drastic.ignore_gamecard_limit", "Drastic/IgnoreGamecardLimit"},
    {"core.drastic.use_16bit_color", "Drastic/Use16BitColor"},
    {"core.drastic.auto_trim", "Drastic/AutoTrim"},
    {"core.drastic.fix_main_engine_screen", "Drastic/FixMainEngineScreen"},
    {"core.drastic.disable_edge_marking", "Drastic/DisableEdgeMarking"},
    {"core.drastic.lua_enabled", "Drastic/LuaEnabled"},
    {"core.drastic.blend", "Drastic/Blend"},
    {"core.drastic.raw_save_format", "Drastic/RawSaveFormat"},
    {"core.drastic.firmware_nickname", "Drastic/FirmwareNickname"},
    {"core.drastic.firmware_language", "Drastic/FirmwareLanguage"},
    {"core.drastic.firmware_color", "Drastic/FirmwareColor"},
    {"core.drastic.firmware_birthday_month", "Drastic/FirmwareBirthdayMonth"},
    {"core.drastic.firmware_birthday_day", "Drastic/FirmwareBirthdayDay"},
    {"core.drastic.lsfg_flow_scale", "Wrapper/LSFGFlowScale"},
    {"core.drastic.lsfg_performance", "Wrapper/LSFGPerformance"},
  };
  char value[PREFS_VAL_LEN];
  for (unsigned index = 0; index < sizeof(mappings) / sizeof(*mappings);
       index++) {
    if (launcher_config_value(mappings[index].launcher_key, value,
                              sizeof(value)))
      put_entry(mappings[index].prefs_key, value);
  }
  if (launcher_config_value("core.drastic.lsfg_enabled", value,
                            sizeof(value))) {
    /* The launcher flag is an explicit permission boundary.  When disabled,
     * the in-game menu must show the feature as unavailable rather than let a
     * stale drastic.ini re-enable it. */
    put_entry("Wrapper/LSFGAllowed", value);
    put_entry("Wrapper/LSFGEnabled", value);
  }
}

/* BeikLiveStation writes this one-shot profile immediately before it transfers
 * control to the NRO.  The ROM guard prevents an older profile from affecting
 * a direct launch, or a launch from another frontend. */
static void import_game_launch_profile(void) {
  static const char *paths[] = {
    "sdmc:/GBAStation/drastic/launch.cfg",
    "/GBAStation/drastic/launch.cfg",
  };
  for (unsigned index = 0; index < sizeof(paths) / sizeof(*paths); index++) {
    FILE *file = fopen(paths[index], "rb");
    if (!file) continue;
    char line[2048];
    char profile_rom[PREFS_VAL_LEN] = "";
    while (fgets(line, sizeof(line), file)) {
      char *key = trim(line);
      char *equals = strchr(key, '=');
      if (!equals) continue;
      *equals++ = '\0';
      if (!strcmp(trim(key), "Wrapper/LaunchRom")) {
        snprintf(profile_rom, sizeof(profile_rom), "%s", trim(equals));
        break;
      }
    }
    fclose(file);
    if (pending_rom[0] && !strcmp(profile_rom, pending_rom)) {
      parse_file(paths[index]);
      return;
    }
  }
}

static void seed_defaults(void) {
  seed("Wrapper/CoreSo", DATA_ROOT "/cores/" SO_NAME);
  seed("Drastic/RomPath", pending_rom[0] ? pending_rom : "");
  seed("Wrapper/Layout", "horizontal");
  seed("Wrapper/Rotation", "0");
  seed("Wrapper/ScreenGap", "8");
  seed("Wrapper/IntegerScale", "false");
  seed("Wrapper/TextureFilter", "nearest");
  seed("Wrapper/VideoFilter", "nearest");
  seed("Wrapper/CustomShader", "");
  seed("Wrapper/Volume", "100");
  seed("Wrapper/MicrophoneSource", "noise");
  seed("Wrapper/Vibration", "true");
  seed("Wrapper/Motion", "true");
  seed("Wrapper/StylusMode", "stick");
  seed("Wrapper/MouseStylus", "true");
  seed("Wrapper/AnalogStylusSpeed", "8");
  seed("Wrapper/LSFGAllowed", "false");
  seed("Wrapper/MotionStylusSensitivity", "10");
  seed("Wrapper/StateSlot", "0");
  seed("Wrapper/CustomTopScale", "1.0");
  seed("Wrapper/CustomTopOffsetX", "0");
  seed("Wrapper/CustomTopOffsetY", "0");
  seed("Wrapper/CustomBottomScale", "1.0");
  seed("Wrapper/CustomBottomOffsetX", "0");
  seed("Wrapper/CustomBottomOffsetY", "0");
  seed("Wrapper/OverlayEnabled", "false");
  seed("Wrapper/OverlayPath", "");
  seed("Wrapper/SavePath", "");
  seed("Wrapper/CheatPath", "");

  seed("Drastic/FrameskipValue", "0");
  seed("Drastic/FrameskipType", "0");
  seed("Drastic/FrameskipSafe", "false");
  seed("Drastic/AudioLatency", "2");
  seed("Drastic/FastForwardSpeed", "5");
  seed("Drastic/CpuThreads", "3");
  seed("Drastic/AutoFireSpeed", "2");
  seed("Drastic/MicLevel", "1");
  seed("Drastic/Slot2Type", "1");
  seed("Drastic/SoundEnabled", "true");
  seed("Drastic/ShowFPS", "false");
  seed("Drastic/Threaded3D", "true");
  seed("Drastic/CheatsEnabled", "true");
  seed("Drastic/MicEnabled", "true");
  seed("Drastic/BackupInSavestates", "true");
  seed("Drastic/IgnoreGamecardLimit", "false");
  seed("Drastic/Use16BitColor", "false");
  seed("Drastic/AutoTrim", "false");
  seed("Drastic/FixMainEngineScreen", "false");
  seed("Drastic/RtcSystemTime", "true");
  seed("Drastic/CustomClockEnable", "false");
  seed("Drastic/CustomClock", "0");
  seed("Drastic/DisableEdgeMarking", "false");
  seed("Drastic/Hires3D", "false");
  seed("Drastic/LuaEnabled", "true");
  seed("Drastic/PreloadRoms", "true");
  seed("Drastic/Blend", "false");
  seed("Drastic/RawSaveFormat", "false");
  seed("Drastic/AutosaveInterval", "300");
  seed("Drastic/FirmwareNickname", "Switch");
  seed("Drastic/FirmwareLanguage", "-1");
  seed("Drastic/FirmwareColor", "0");
  seed("Drastic/FirmwareBirthdayMonth", "6");
  seed("Drastic/FirmwareBirthdayDay", "6");
}

static void migrate_fast_forward_speed(void) {
  const int version = prefs_get_int("Wrapper/LauncherSettingsVersion", 0);
  if (version >= 3) return;
  if (prefs_get_int("Drastic/FastForwardSpeed", 5) == 2)
    put_entry("Drastic/FastForwardSpeed", "5");
  put_entry("Wrapper/LauncherSettingsVersion", "3");
}

static void migrate_stylus_mode(void) {
  if (find_entry("Wrapper/StylusMode") >= 0) return;
  const int legacy = find_entry("Wrapper/AnalogStylus");
  if (legacy < 0) return;
  put_entry("Wrapper/StylusMode",
            prefs_get_bool("Wrapper/AnalogStylus", true) ? "stick" : "off");
}

void prefs_set_disc_path(const char *path) {
  snprintf(pending_rom, sizeof(pending_rom), "%s", path ? path : "");
  if (path && *path) put_entry("Drastic/RomPath", path);
}

void prefs_init(const char *path) {
  memset(entries, 0, sizeof(entries));
  entry_count = 0;
  snprintf(ini_path, sizeof(ini_path), "%s", path ? path : PREFS_PATH);
  parse_file(ini_path);
  /* The launcher ROM is an invocation-scoped override.  Loading drastic.ini
   * first used to retain its previous Drastic/RomPath because seed_defaults()
   * only fills missing keys, so every launch reopened the old game. */
  if (pending_rom[0]) put_entry("Drastic/RomPath", pending_rom);
  prefs_remove("Wrapper/CpuBoost");
  prefs_remove("Wrapper/Renderer");
  prefs_remove("Wrapper/VulkanLowLatency");
  /* LSFG is a Vulkan-only runtime option.  Preserve its user settings so the
   * renderer can reserve its device/swapchain resources at the next launch. */
  prefs_remove("Wrapper/LSFGDllPath");
  /* Screen order is a runtime-only action controlled by the launcher's NDS
   * mapping. Older per-core toggles must not reactivate it at next launch. */
  prefs_remove("Wrapper/SwapScreens");
  migrate_stylus_mode();
  seed_defaults();
  migrate_fast_forward_speed();
  import_launcher_core_config();
  import_game_launch_profile();
  /* Older launcher builds materialized their UI fallback of 200% into
   * config.cfg.  It is not an explicit user choice, so migrate that legacy
   * default after every import; other selected rates remain untouched. */
  if (prefs_get_int("Drastic/FastForwardSpeed", 5) == 2)
    put_entry("Drastic/FastForwardSpeed", "5");
  /* This Vulkan host always runs DraStic's 3D engine at 2x.  The core has no
   * higher multiplier and this intentionally overrides stale launcher data. */
  put_entry("Drastic/Hires3D", "true");
  /* The shared NDS save directory is also consumed by nds_stub.  DraStic's
   * native .dsv format appends a DeSmuME footer and cannot safely share that
   * file, so always request its raw, interoperable .sav output.  Do this
   * after both launcher imports: a stale per-game profile must not undo it. */
  put_entry("Drastic/RawSaveFormat", "true");
}

void prefs_save(void) {
  if (!ini_path[0]) return;
  char temporary[sizeof(ini_path) + 8];
  snprintf(temporary, sizeof(temporary), "%s.tmp", ini_path);
  FILE *file = fopen(temporary, "wb");
  if (!file) return;
  fputs("# DrasticDS_nx launch configuration\n", file);
  for (int index = 0; index < entry_count; index++)
    fprintf(file, "%s = %s\n", entries[index].key, entries[index].value);
  int failed = fflush(file) != 0;
  if (fclose(file) != 0) failed = 1;
  if (failed) {
    remove(temporary);
    return;
  }
  remove(ini_path);
  rename(temporary, ini_path);
}

void prefs_save_runtime_key(const char *key) {
  prefs_save();
  if (!key || !*key) return;
  const char *profile = prefs_get_string("Wrapper/GameConfigPath", "");
  static const char allowed_root[] = DATA_ROOT "/gamecfg/";
  if (strncmp(profile, allowed_root, sizeof(allowed_root) - 1) ||
      strstr(profile, "..")) return;
  const int entry = find_entry(key);
  if (entry < 0) return;

  char temporary[1200];
  snprintf(temporary, sizeof(temporary), "%s.tmp", profile);
  FILE *input = fopen(profile, "rb");
  FILE *output = fopen(temporary, "wb");
  if (!output) {
    if (input) fclose(input);
    return;
  }
  int replaced = 0;
  char line[2048];
  while (input && fgets(line, sizeof(line), input)) {
    char copy[sizeof(line)];
    snprintf(copy, sizeof(copy), "%s", line);
    char *candidate = trim(copy);
    char *equals = strchr(candidate, '=');
    if (equals) {
      *equals = '\0';
      candidate = trim(candidate);
    }
    if (equals && !strcmp(candidate, key)) {
      fprintf(output, "%s = %s\n", key, entries[entry].value);
      replaced = 1;
    } else {
      fputs(line, output);
    }
  }
  if (input) fclose(input);
  if (!replaced) fprintf(output, "%s = %s\n", key, entries[entry].value);
  int failed = fflush(output) != 0;
  if (fclose(output) != 0) failed = 1;
  if (failed) {
    remove(temporary);
    return;
  }
  remove(profile);
  rename(temporary, profile);
}

bool prefs_contains(const char *key) { return find_entry(key) >= 0; }

const char *prefs_get_string(const char *key, const char *fallback) {
  const int index = find_entry(key);
  return index < 0 ? fallback : entries[index].value;
}

bool prefs_get_bool(const char *key, bool fallback) {
  const char *value = prefs_get_string(key, fallback ? "true" : "false");
  if (!strcasecmp(value, "true") || !strcmp(value, "1") || !strcasecmp(value, "on")) return true;
  if (!strcasecmp(value, "false") || !strcmp(value, "0") || !strcasecmp(value, "off")) return false;
  return fallback;
}

int prefs_get_int(const char *key, int fallback) {
  const int index = find_entry(key);
  return index < 0 ? fallback : (int)strtol(entries[index].value, NULL, 0);
}

int64_t prefs_get_int64(const char *key, int64_t fallback) {
  const int index = find_entry(key);
  if (index < 0) return fallback;
  char *end = NULL;
  const long long value = strtoll(entries[index].value, &end, 10);
  return end && *end == '\0' ? (int64_t)value : fallback;
}

float prefs_get_float(const char *key, float fallback) {
  const int index = find_entry(key);
  return index < 0 ? fallback : strtof(entries[index].value, NULL);
}

void prefs_set_bool(const char *key, bool value) { put_entry(key, value ? "true" : "false"); }
void prefs_set_string(const char *key, const char *value) { put_entry(key, value ? value : ""); }

void prefs_set_int(const char *key, int value) {
  char text[32];
  snprintf(text, sizeof(text), "%d", value);
  put_entry(key, text);
}

void prefs_set_float(const char *key, float value) {
  char text[32];
  snprintf(text, sizeof(text), "%g", value);
  put_entry(key, text);
}

void prefs_remove(const char *key) {
  const int index = find_entry(key);
  if (index < 0) return;
  entries[index] = entries[--entry_count];
  memset(&entries[entry_count], 0, sizeof(entries[entry_count]));
}
