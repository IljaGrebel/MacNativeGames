/* Values made from the player's files, read at start by the prebuilt program (TOTK_WIN_RUNTIME_CONFIG): the
   romfs_config.h and shared_fonts_config.h that prepare_romfs_backend.py / prepare_shared_fonts.py write, in the folder
   TOTK_WIN_CONFIG_DIR names. The headers are parsed, not compiled: only the #define lines those scripts write. A missing
   or malformed value stops the program with its name, as a wrong build-time header would stop the build. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void runtime_fail(const char *message);

static char *read_header(const char *name) {
    const char *dir = getenv("TOTK_WIN_CONFIG_DIR");
    char path[4096];
    if (!dir || snprintf(path, sizeof path, "%s/%s", dir, name) >= (int)sizeof path) return NULL;
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *text = n >= 0 && n < (1 << 20) ? malloc((size_t)n + 1) : NULL;
    if (text && fread(text, 1, (size_t)n, f) == (size_t)n) text[n] = 0;
    else { free(text); text = NULL; }
    fclose(f);
    return text;
}

/* The text after "#define NAME " up to the end of the line, or NULL. */
static const char *define_value(const char *text, const char *name, size_t *len) {
    size_t nl = strlen(name);
    for (const char *p = text; (p = strstr(p, "#define ")) != NULL; p += 8) {
        const char *q = p + 8;
        if (strncmp(q, name, nl) || q[nl] != ' ') continue;
        q += nl + 1;
        size_t n = strcspn(q, "\r\n");
        *len = n;
        return q;
    }
    return NULL;
}

static void fail_value(const char *header, const char *name) {
    static char message[256];
    snprintf(message, sizeof message, "%s: no valid %s (run the preparation step: tools/prepare_game.py)", header, name);
    runtime_fail(message);
}

/* A quoted string at *p: its contents (backslashes kept as they are: Windows paths), *p past the closing quote. */
static char *quoted(const char **p) {
    if (**p != '"') return NULL;
    const char *start = ++*p, *end = strchr(start, '"');
    if (!end) return NULL;
    char *s = malloc((size_t)(end - start) + 1);
    if (!s) return NULL;
    memcpy(s, start, (size_t)(end - start));
    s[end - start] = 0;
    *p = end + 1;
    return s;
}

static int is_sha(const char *s) {
    if (strlen(s) != 64) return 0;
    for (int i = 0; i < 64; i++)
        if (!strchr("0123456789abcdef", s[i])) return 0;
    return 1;
}

static struct { int loaded; uint64_t size; char image[65], index[65]; } romfs;

static void load_romfs(void) {
    if (romfs.loaded) return;
    const char *header = "romfs_config.h";
    char *text = read_header(header);
    if (!text) fail_value(header, "file in TOTK_WIN_CONFIG_DIR");
    size_t n;
    const char *v = define_value(text, "ROMFS_IMAGE_SIZE", &n);
    char *end;
    romfs.size = v ? strtoull(v, &end, 10) : 0;
    if (!v || !romfs.size) fail_value(header, "ROMFS_IMAGE_SIZE");
    const char *names[2] = {"ROMFS_IMAGE_SHA256", "ROMFS_INDEX_SHA256"};
    char *out[2] = {romfs.image, romfs.index};
    for (int i = 0; i < 2; i++) {
        v = define_value(text, names[i], &n);
        char *s = v ? quoted(&v) : NULL;
        if (!s || !is_sha(s)) fail_value(header, names[i]);
        memcpy(out[i], s, 65);
        free(s);
    }
    free(text);
    romfs.loaded = 1;
}

uint64_t game_config_romfs_image_size(void) { load_romfs(); return romfs.size; }
const char *game_config_romfs_image_sha256(void) { load_romfs(); return romfs.image; }
const char *game_config_romfs_index_sha256(void) { load_romfs(); return romfs.index; }

typedef struct { const char *path; uint64_t file_bytes, font_bytes; const char *sha256; } FontRow;   /* shared_font.c's */

void game_config_font_rows(void *out, int count) {
    const char *header = "shared_fonts_config.h";
    char *text = read_header(header);
    if (!text) fail_value(header, "file in TOTK_WIN_CONFIG_DIR");
    size_t n;
    const char *p = define_value(text, "SHARED_FONT_ROWS", &n);
    if (!p) fail_value(header, "SHARED_FONT_ROWS");
    FontRow *rows = out;
    for (int i = 0; i < count; i++) {
        while (*p == ' ' || *p == ',') p++;
        if (*p++ != '{') fail_value(header, "SHARED_FONT_ROWS");
        char *path = quoted(&p), *end;
        if (!path || *p++ != ',') fail_value(header, "SHARED_FONT_ROWS");
        uint64_t file_bytes = strtoull(p, &end, 10);
        p = end;
        while (*p == 'U' || *p == 'L') p++;
        if (*p++ != ',') fail_value(header, "SHARED_FONT_ROWS");
        uint64_t font_bytes = strtoull(p, &end, 10);
        p = end;
        while (*p == 'U' || *p == 'L') p++;
        if (*p++ != ',') fail_value(header, "SHARED_FONT_ROWS");
        char *sha = quoted(&p);
        if (!sha || *p++ != '}' || (sha[0] && !is_sha(sha))) fail_value(header, "SHARED_FONT_ROWS");
        rows[i] = (FontRow){path, file_bytes, font_bytes, sha};
    }
    free(text);
}
