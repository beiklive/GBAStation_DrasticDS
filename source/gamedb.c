#include "gamedb.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "debug_log.h"

static const char *const db_paths[] = {
  "sdmc:/GBAStation/data/GameData_NDS.json",
  "/GBAStation/data/GameData_NDS.json",
};

static char *read_text(const char *path, size_t *size) {
  FILE *file = fopen(path, "rb");
  if (!file) return NULL;
  if (fseek(file, 0, SEEK_END)) { fclose(file); return NULL; }
  long length = ftell(file);
  if (length < 2 || fseek(file, 0, SEEK_SET)) { fclose(file); return NULL; }
  char *data = malloc((size_t)length + 1);
  if (!data) { fclose(file); return NULL; }
  const size_t read = fread(data, 1, (size_t)length, file);
  fclose(file);
  if (read != (size_t)length) { free(data); return NULL; }
  data[read] = '\0';
  if (size) *size = read;
  return data;
}

static int write_text(const char *path, const char *data, size_t size) {
  char temporary[1100];
  snprintf(temporary, sizeof(temporary), "%s.tmp", path);
  FILE *file = fopen(temporary, "wb");
  if (!file) return 0;
  int ok = fwrite(data, 1, size, file) == size && fflush(file) == 0;
  if (fclose(file) != 0) ok = 0;
  if (!ok) { remove(temporary); return 0; }
  remove(path);
  if (rename(temporary, path)) { remove(temporary); return 0; }
  return 1;
}

static void normalize_path(char *path) {
  if (!path) return;
  for (char *p = path; *p; p++) if (*p == '\\') *p = '/';
  if (!strncmp(path, "sdmc:", 5)) memmove(path, path + 5, strlen(path + 5) + 1);
}

static const char *skip_string(const char *p) {
  if (*p != '"') return p;
  for (p++; *p; p++) {
    if (*p == '\\' && p[1]) { p++; continue; }
    if (*p == '"') return p + 1;
  }
  return p;
}

static const char *object_end(const char *start) {
  int depth = 0;
  for (const char *p = start; *p; p++) {
    if (*p == '"') { p = skip_string(p) - 1; continue; }
    if (*p == '{') depth++;
    else if (*p == '}' && --depth == 0) return p + 1;
  }
  return NULL;
}

static const char *field_value(const char *object, const char *key) {
  char quoted[96];
  snprintf(quoted, sizeof(quoted), "\"%s\"", key);
  const char *p = object;
  while ((p = strstr(p, quoted))) {
    p += strlen(quoted);
    while (isspace((unsigned char)*p)) p++;
    if (*p != ':') continue;
    p++;
    while (isspace((unsigned char)*p)) p++;
    return p;
  }
  return NULL;
}

static int read_string_field(const char *object, const char *key,
                             char *out, size_t out_size) {
  const char *p = field_value(object, key);
  if (!p || *p != '"' || !out_size) return 0;
  size_t used = 0;
  for (p++; *p && *p != '"'; p++) {
    if (*p == '\\' && p[1]) p++;
    if (used + 1 < out_size) out[used++] = *p;
  }
  out[used] = '\0';
  return *p == '"';
}

static int read_int_field(const char *object, const char *key, int fallback) {
  const char *p = field_value(object, key);
  return p ? (int)strtol(p, NULL, 10) : fallback;
}

static char *set_field(char *object, const char *key, const char *value) {
  const char *value_start = field_value(object, key);
  size_t old_start, old_end;
  if (value_start) {
    old_start = (size_t)(value_start - object);
    const char *p = value_start;
    if (*p == '"') p = skip_string(p);
    else while (*p && *p != ',' && *p != '}') p++;
    old_end = (size_t)(p - object);
  } else {
    const size_t length = strlen(object);
    if (!length || object[length - 1] != '}') return object;
    old_start = old_end = length - 1;
  }
  char member[1200];
  if (value_start) snprintf(member, sizeof(member), "%s", value);
  else snprintf(member, sizeof(member), "%s\"%s\":%s", old_start > 1 ? "," : "", key, value);
  const size_t length = strlen(object);
  const size_t replacement = strlen(member);
  char *updated = realloc(object, length - (old_end - old_start) + replacement + 1);
  if (!updated) return object;
  memmove(updated + old_start + replacement, updated + old_end,
          length - old_end + 1);
  memcpy(updated + old_start, member, replacement);
  return updated;
}

/* GameData_NDS.json schema contract.  Keep these wrappers instead of passing
 * ad-hoc JSON fragments at call sites: BeikLiveStation deserializes strings,
 * booleans, integral counters and floating-point layout values with distinct
 * C++ value() types. */
static char *set_string_field(char *object, const char *key,
                              const char *value) {
  char encoded[1100];
  /* GameDB paths and labels are launcher-generated UTF-8.  Backslashes are
   * normalized by the launcher; quote and slash escaping keeps this JSON
   * value a string even for an unusual filename. */
  size_t used = 0;
  encoded[used++] = '"';
  for (const unsigned char *p = (const unsigned char *)(value ? value : "");
       *p && used + 3 < sizeof(encoded); p++) {
    if (*p == '"' || *p == '\\') encoded[used++] = '\\';
    else if (*p < 0x20) continue;
    encoded[used++] = (char)*p;
  }
  encoded[used++] = '"';
  encoded[used] = '\0';
  return set_field(object, key, encoded);
}

static char *set_int_field(char *object, const char *key, int value) {
  char encoded[32];
  snprintf(encoded, sizeof(encoded), "%d", value);
  return set_field(object, key, encoded);
}

static char *set_float_field(char *object, const char *key, float value) {
  char encoded[48];
  /* A decimal point preserves GameDB's number_float type even for whole
   * values such as 1.0 and 0.0. */
  snprintf(encoded, sizeof(encoded), "%.6f", (double)value);
  return set_field(object, key, encoded);
}

static char *set_bool_field(char *object, const char *key, int value) {
  return set_field(object, key, value ? "true" : "false");
}

enum {
  GAMEDB_DISPLAY_LAYOUT = 1,
  GAMEDB_DISPLAY_OVERLAY = 2,
  GAMEDB_DISPLAY_SHADER = 4,
  GAMEDB_DISPLAY_ALL = GAMEDB_DISPLAY_LAYOUT | GAMEDB_DISPLAY_OVERLAY |
                       GAMEDB_DISPLAY_SHADER,
};

static int patch_database(const char *rom_path, int all_nds,
                          const DrasticRuntimeConfig *config,
                          int display_fields,
                          int *play_count, int *play_time,
                          const char *last_played, const char *screenshot) {
  char wanted[1024];
  snprintf(wanted, sizeof(wanted), "%s", rom_path ? rom_path : "");
  normalize_path(wanted);
  for (unsigned file_index = 0; file_index < sizeof(db_paths) / sizeof(*db_paths); file_index++) {
    size_t input_size = 0;
    char *input = read_text(db_paths[file_index], &input_size);
    if (!input) continue;
    /* A GameDB entry expands by less than one KiB. Reserve enough room for
     * every NDS entry instead of risking a fixed single-entry margin. */
    char *output = calloc(1, input_size * 3 + 16384);
    size_t output_size = 0;
    int updates = 0;
    const char *cursor = input;
    while (*cursor) {
      const char *start = strchr(cursor, '{');
      if (!start) {
        const size_t tail = strlen(cursor);
        memcpy(output + output_size, cursor, tail + 1);
        output_size += tail;
        break;
      }
      memcpy(output + output_size, cursor, (size_t)(start - cursor));
      output_size += (size_t)(start - cursor);
      const char *end = object_end(start);
      if (!end) {
        const size_t tail = strlen(start);
        memcpy(output + output_size, start, tail + 1);
        output_size += tail;
        break;
      }
      const size_t object_size = (size_t)(end - start);
      char *object = malloc(object_size + 1);
      memcpy(object, start, object_size); object[object_size] = '\0';
      char item_path[1024] = "";
      read_string_field(object, "path", item_path, sizeof(item_path));
      normalize_path(item_path);
      const int matches = all_nds ? read_int_field(object, "platform", -1) == 6
                                  : item_path[0] && !strcmp(item_path, wanted);
      if (matches) {
        if (play_count) {
          object = set_int_field(object, "playCount", *play_count);
        }
        if (play_time) {
          object = set_int_field(object, "playTime", *play_time);
        }
        if (last_played && last_played[0])
          object = set_string_field(object, "lastPlayed", last_played);
        if (screenshot && screenshot[0])
          object = set_string_field(object, "screenShotPath", screenshot);
        if (config && (display_fields & GAMEDB_DISPLAY_LAYOUT)) {
          static const char *layouts[] = {"vertical", "horizontal", "top", "bottom",
                                          "priority_top", "priority_bottom", "custom"};
          const unsigned layout_index = (unsigned)config->layout < 7
              ? (unsigned)config->layout : 0;
          object = set_string_field(object, "ndsScreenLayout", layouts[layout_index]);
          {
            char orientation[8];
            snprintf(orientation, sizeof(orientation), "%d",
                     (config->rotation & 3) * 90);
            object = set_string_field(object, "ndsScreenOrientation", orientation);
          }
          object = set_int_field(object, "ndsScreenGap", config->screen_gap);
          object = set_bool_field(object, "ndsIntegerScale",
                                  config->integer_scale);
          object = set_float_field(object, "ndsTopScale", config->custom_top_scale);
          object = set_float_field(object, "ndsTopOffsetX", config->custom_top_offset_x);
          object = set_float_field(object, "ndsTopOffsetY", config->custom_top_offset_y);
          object = set_float_field(object, "ndsBottomScale", config->custom_bottom_scale);
          object = set_float_field(object, "ndsBottomOffsetX", config->custom_bottom_offset_x);
          object = set_float_field(object, "ndsBottomOffsetY", config->custom_bottom_offset_y);
        }
        if (config && (display_fields & GAMEDB_DISPLAY_OVERLAY)) {
          object = set_bool_field(object, "overlayEnabled",
                                  config->overlay_enabled);
          object = set_string_field(object, "overlayPath", config->overlay_path);
        }
        if (config && (display_fields & GAMEDB_DISPLAY_SHADER)) {
          /* GameData_NDS uses these exact string fields.  Built-in filters
           * are identified by NdsShaderType; shaderPath remains available
           * for the custom-filter rollout without changing field types. */
          object = set_string_field(object, "NdsShaderType",
                                    drastic_config_filter_name(config->video_filter));
          object = set_string_field(object, "shaderPath", config->custom_shader);
        }
        updates++;
      }
      const size_t length = strlen(object);
      memcpy(output + output_size, object, length);
      output_size += length;
      free(object);
      cursor = end;
    }
    if (updates && write_text(db_paths[file_index], output, output_size)) {
      debug_logf("GameDB updated entries=%d path=%s", updates, db_paths[file_index]);
      free(output); free(input); return updates;
    }
    free(output); free(input);
  }
  return 0;
}

static void now_string(char *out, size_t size) {
  const time_t now = time(NULL);
  struct tm *tm = localtime(&now);
  if (!tm) { out[0] = '\0'; return; }
  strftime(out, size, "%y-%m-%d %H-%M-%S", tm);
}

int gamedb_get_save_path(const char *rom_path, char *out, size_t out_size) {
  if (!out || !out_size) return 0;
  out[0] = '\0';
  if (!rom_path || !rom_path[0]) return 0;

  char wanted[1024];
  snprintf(wanted, sizeof(wanted), "%s", rom_path);
  normalize_path(wanted);
  for (unsigned file_index = 0;
       file_index < sizeof(db_paths) / sizeof(*db_paths); file_index++) {
    size_t input_size = 0;
    char *input = read_text(db_paths[file_index], &input_size);
    if (!input) continue;
    for (char *cursor = input; (cursor = strchr(cursor, '{'));) {
      const char *end = object_end(cursor);
      if (!end) break;
      const size_t object_size = (size_t)(end - cursor);
      char *object = malloc(object_size + 1);
      if (!object) break;
      memcpy(object, cursor, object_size);
      object[object_size] = '\0';
      char path[1024] = "";
      char save_path[1024] = "";
      const int found = read_string_field(object, "path", path, sizeof(path)) &&
          read_string_field(object, "savePath", save_path, sizeof(save_path));
      normalize_path(path);
      if (found && save_path[0] && !strcmp(path, wanted)) {
        snprintf(out, out_size, "%s", save_path);
        free(object);
        free(input);
        return 1;
      }
      free(object);
      cursor = (char *)end;
    }
    free(input);
  }
  return 0;
}

int gamedb_session_started(const char *rom_path, int *play_count, int *play_time) {
  int count = 0, time = 0;
  /* First get current values from the object, then write the increment. */
  for (unsigned i = 0; i < sizeof(db_paths) / sizeof(*db_paths); i++) {
    size_t size; char *data = read_text(db_paths[i], &size); if (!data) continue;
    char wanted[1024]; snprintf(wanted, sizeof(wanted), "%s", rom_path); normalize_path(wanted);
    for (char *p = data; (p = strchr(p, '{'));) {
      const char *end = object_end(p); if (!end) break;
      size_t bytes = (size_t)(end - p); char *object = malloc(bytes + 1);
      memcpy(object, p, bytes); object[bytes] = '\0'; char path[1024] = "";
      read_string_field(object, "path", path, sizeof(path)); normalize_path(path);
      if (!strcmp(path, wanted)) { count = read_int_field(object, "playCount", 0); time = read_int_field(object, "playTime", 0); free(object); break; }
      free(object); p = (char *)end;
    }
    free(data); break;
  }
  count++;
  if (play_count) *play_count = count;
  if (play_time) *play_time = time;
  return patch_database(rom_path, 0, NULL, 0, &count, &time, NULL, NULL);
}

int gamedb_session_finished(const char *rom_path, int play_count, int play_time,
                            const char *screenshot_path) {
  char timestamp[64]; now_string(timestamp, sizeof(timestamp));
  return patch_database(rom_path, 0, NULL, 0, &play_count, &play_time,
                        timestamp, screenshot_path);
}

int gamedb_save_display_current(const char *rom_path,
                                const DrasticRuntimeConfig *config) {
  return patch_database(rom_path, 0, config, GAMEDB_DISPLAY_ALL,
                        NULL, NULL, NULL, NULL);
}

int gamedb_sync_display_to_all(const char *rom_path,
                               const DrasticRuntimeConfig *config) {
  return patch_database(rom_path, 1, config, GAMEDB_DISPLAY_ALL,
                        NULL, NULL, NULL, NULL);
}

int gamedb_sync_layout_to_all(const char *rom_path,
                              const DrasticRuntimeConfig *config) {
  return patch_database(rom_path, 1, config, GAMEDB_DISPLAY_LAYOUT,
                        NULL, NULL, NULL, NULL);
}

int gamedb_sync_overlay_to_all(const char *rom_path,
                               const DrasticRuntimeConfig *config) {
  return patch_database(rom_path, 1, config, GAMEDB_DISPLAY_OVERLAY,
                        NULL, NULL, NULL, NULL);
}

int gamedb_sync_shader_to_all(const char *rom_path,
                              const DrasticRuntimeConfig *config) {
  return patch_database(rom_path, 1, config, GAMEDB_DISPLAY_SHADER,
                        NULL, NULL, NULL, NULL);
}
