#ifndef POTOK_PICKER_H
#define POTOK_PICKER_H
#include <stddef.h>
#include <stdbool.h>
#include <limits.h>
#include <signal.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct { char **paths; size_t count; bool limited; char root[PATH_MAX]; } FileList;
bool files_scan(FileList *list,const char *root,char *error,size_t cap);
void files_free(FileList *list);
bool file_matches(const char *path,const char *query);
/* 1: selected, 0: cancelled, 2: fzf unavailable, -1: error. */
int picker_fzf(const char *root,const char *query,char selected[PATH_MAX],const volatile sig_atomic_t *cancel,char *error,size_t cap);
#ifdef __cplusplus
}
#endif
#endif
