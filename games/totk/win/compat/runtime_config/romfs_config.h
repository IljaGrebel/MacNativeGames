/* The prebuilt program's romfs_config.h (TOTK_WIN_RUNTIME_CONFIG): the values prepare_romfs_backend.py writes for the
   player's RomFS are read at start from the same header in the folder TOTK_WIN_CONFIG_DIR names (runtime/game_config.c),
   so one totk-win.exe serves every dump without a compiler on the player's machine. */
#pragma once
#include <stdint.h>
uint64_t game_config_romfs_image_size(void);
const char *game_config_romfs_image_sha256(void);   /* 64 hex digits and a NUL: 65 bytes, as the literal was */
const char *game_config_romfs_index_sha256(void);
#define ROMFS_IMAGE_SIZE (game_config_romfs_image_size())
#define ROMFS_IMAGE_SHA256 (game_config_romfs_image_sha256())
#define ROMFS_INDEX_SHA256 (game_config_romfs_index_sha256())
