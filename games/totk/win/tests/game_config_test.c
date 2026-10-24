/* runtime/game_config.c reads the headers the preparation scripts write (tests/test_win_loader.py GameConfigTests). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
uint64_t game_config_romfs_image_size(void);
const char *game_config_romfs_image_sha256(void);
const char *game_config_romfs_index_sha256(void);
void game_config_font_rows(void *rows, int count);
typedef struct { const char *path; uint64_t file_bytes, font_bytes; const char *sha256; } FontRow;
void runtime_fail(const char *m) { fprintf(stderr, "FAIL %s\n", m); exit(3); }
int main(void) {
    printf("size %llu\nimage %s\nindex %s\n", (unsigned long long)game_config_romfs_image_size(),
           game_config_romfs_image_sha256(), game_config_romfs_index_sha256());
    FontRow rows[6];
    game_config_font_rows(rows, 6);
    for (int i = 0; i < 6; i++)
        printf("font %s %llu %llu %s\n", rows[i].path, (unsigned long long)rows[i].file_bytes,
               (unsigned long long)rows[i].font_bytes, rows[i].sha256);
    return 0;
}
