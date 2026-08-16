/* Shared build and SD-card layout for DrasticDS_nx. */
#ifndef DRASTIC_NX_CONFIG_H
#define DRASTIC_NX_CONFIG_H

/* libdrastic_arm64.so spans just under 64 MiB including BSS. */
#define SO_REGION_MB 96

#define SO_NAME "libdrastic_arm64.so"

#define DATA_ROOT       "sdmc:/GBAStation/drastic"
#define SYSTEM_DIR      DATA_ROOT "/system"
#define USER_DIR        DATA_ROOT "/user"
#define CACHE_DIR       DATA_ROOT "/cache"
#define UNZIP_CACHE_DIR DATA_ROOT "/unzip_cache"
#define GAMES_DIR       DATA_ROOT "/games"
#define CHEATS_DIR      DATA_ROOT "/cheats"
#define SCRIPTS_DIR     DATA_ROOT "/scripts"
#define SHADERS_DIR     DATA_ROOT "/shaders"
#define SLOT2_DIR       DATA_ROOT "/slot2"
#define MICROPHONE_DIR  DATA_ROOT "/microphone"
#define SAVESTATES_DIR  USER_DIR "/savestates"
#define BACKUPS_DIR     USER_DIR "/backup"
#define GBASTATION_DIR  "sdmc:/GBAStation"
#define NDS_BIOS_DIR    GBASTATION_DIR "/bios/NDS"
#define CHEAT_DATABASE_PATH GBASTATION_DIR "/cheats/usrcheat.dat"
#define PREFS_NAME      "drastic.ini"
#define PREFS_PATH      DATA_ROOT "/" PREFS_NAME
#define DRASTIC_APK_VERSION_CODE 109
#define ANDROID_SDK_INT 30
#define ANDROID_MANUFACTURER "Nintendo"
#define ANDROID_MODEL "Switch"
#define ANDROID_DEVICE_NAME "Nintendo Switch"

#define DRASTIC_RENDERER_GL 0
#define DRASTIC_RENDERER DRASTIC_RENDERER_GL

extern int screen_width;
extern int screen_height;
extern int panel_width;
extern int panel_height;

#endif
