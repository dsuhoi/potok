#ifndef POTOK_SETTINGS_H
#define POTOK_SETTINGS_H
#include <stdbool.h>
#include <stddef.h>
#include <limits.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char directory[PATH_MAX], downloads[PATH_MAX], state[PATH_MAX], browse[PATH_MAX];
    bool russian;
} Settings;
bool private_directory(const char *path,char *error,size_t cap);
bool settings_load(Settings *s,const char *directory,bool russian,char *error,size_t cap);
bool settings_save(const Settings *s,char *error,size_t cap);
#ifdef __cplusplus
}
#endif
#endif
