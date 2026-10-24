/* The prebuilt program's shared_fonts_config.h (TOTK_WIN_RUNTIME_CONFIG): the rows prepare_shared_fonts.py writes are
   read at start from the header in TOTK_WIN_CONFIG_DIR (runtime/game_config.c). shared_font.c fills its table through
   SHARED_FONT_ROWS_AT_RUN_TIME before the first font is loaded. */
#pragma once
#define SHARED_FONT_COUNT 6
void game_config_font_rows(void *rows, int count);   /* rows: shared_font.c's FontRow[count] */
#define SHARED_FONT_ROWS_AT_RUN_TIME(rows) game_config_font_rows((rows), SHARED_FONT_COUNT)
