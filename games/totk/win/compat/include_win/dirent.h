/* <dirent.h> for Windows with d_type (MinGW's has none), over FindFirstFileW; names are UTF-8. */
#pragma once
#include <stdint.h>
#define DT_UNKNOWN 0
#define DT_DIR 4
#define DT_REG 8
struct dirent { uint64_t d_ino; unsigned char d_type; char d_name[1024]; };
typedef struct CompatDir DIR;
DIR *opendir(const char *path);
struct dirent *readdir(DIR *d);
int closedir(DIR *d);
