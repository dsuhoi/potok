#ifndef POTOK_ENGINE_H
#define POTOK_ENGINE_H
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Bt Bt;
typedef struct {
    char hash[65], name[1024], dir[1024], error[512];
    int status, peers, queue;
    double done, shown, size, down, up, eta, ratio, metadata, checked;
} Torrent;
typedef struct { char name[4096]; double size, done; bool wanted; } BtFile;
typedef struct { char address[64], client[128]; double down, up; } BtPeer;
typedef struct { char url[1024], error[256]; } BtTracker;
typedef struct {
    BtFile *files; int file_count;
    BtPeer *peers; int peer_count;
    BtTracker *trackers; int tracker_count;
} BtDetails;
typedef enum { BT_PAUSE, BT_RESUME, BT_VERIFY, BT_UP, BT_DOWN, BT_REMOVE, BT_DELETE } BtAction;

Bt *bt_open(const char *state_dir, const char *download_dir, bool local_only, char *error, size_t cap);
bool bt_close(Bt *bt, char *error, size_t cap);
bool bt_add(Bt *bt, const char *source, const char *directory, bool paused, char *error, size_t cap);
bool bt_action(Bt *bt, const char *hash, BtAction action, char *error, size_t cap);
bool bt_file(Bt *bt, const char *hash, int index, bool wanted, char *error, size_t cap);
bool bt_limits(Bt *bt, int down_kib, int up_kib, bool slow, char *error, size_t cap);
void bt_get_limits(Bt *bt, int *down_kib, int *up_kib, bool *slow);
bool bt_snapshot(Bt *bt, Torrent **items, int *count, char *error, size_t cap);
bool bt_details(Bt *bt, const char *hash, int tab, BtDetails *details, char *error, size_t cap);
void bt_details_free(BtDetails *details);
int bt_poll(Bt *bt, char *error, size_t cap);
bool bt_busy(Bt *bt);

#ifdef __cplusplus
}
#endif
#endif
