#ifndef POTOK_SERVICE_H
#define POTOK_SERVICE_H
#include "engine.h"
#include <sys/types.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int fd,down,up;
    bool slow,busy;
    pid_t pid;
    unsigned long long revision;
} Client;
Client *client_open(const char *state,const char *downloads,bool start,char *error,size_t cap);
void client_close(Client *client);
bool client_stop(Client *client,char *error,size_t cap);
int service_run(const char *state,const char *downloads,bool local_only);
int client_poll(Client *client,char *error,size_t cap);
bool client_add(Client *client,const char *source,const char *directory,bool paused,char *error,size_t cap);
bool client_action(Client *client,const char *hash,BtAction action,char *error,size_t cap);
bool client_file(Client *client,const char *hash,int index,bool wanted,char *error,size_t cap);
bool client_limits(Client *client,int down,int up,bool slow,char *error,size_t cap);
void client_get_limits(Client *client,int *down,int *up,bool *slow);
bool client_snapshot(Client *client,Torrent **items,int *count,char *error,size_t cap);
bool client_details(Client *client,const char *hash,int tab,BtDetails *details,char *error,size_t cap);
bool client_busy(Client *client);
#ifdef __cplusplus
}
#endif
#endif
