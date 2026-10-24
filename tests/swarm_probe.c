#define _GNU_SOURCE
#include "../service.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc,char **argv) {
    if (argc<4) { fputs("usage: swarm_probe STATE DOWNLOADS status|details|magnet|limits|add|pause|resume|verify|remove|stop [args]\n",stderr); return 2; }
    char error[1024]={0}; Client *client=client_open(argv[1],argv[2],false,error,sizeof(error));
    if (!client) { fprintf(stderr,"%s\n",error); return 1; }
    const char *cmd=argv[3]; bool ok=true;
    if (!strcmp(cmd,"limits") && argc==6) ok=client_limits(client,atoi(argv[4]),atoi(argv[5]),false,error,sizeof(error));
    else if (!strcmp(cmd,"add") && argc>=5) ok=client_add(client,argv[4],argv[2],argc==6 && !strcmp(argv[5],"paused"),error,sizeof(error));
    else if (!strcmp(cmd,"stop")) ok=client_stop(client,error,sizeof(error));
    else {
        Torrent *rows=NULL; int count=0;
        ok=client_snapshot(client,&rows,&count,error,sizeof(error));
        if (ok && !strcmp(cmd,"status")) {
            printf("pid=%d torrents=%d down_limit=%d up_limit=%d\n",(int)client->pid,count,client->down,client->up);
            for (int i=0;i<count;i++) printf("hash=%s name=%s state=%d metadata=%.0f progress=%.9f size=%.0f down=%.0f up=%.0f peers=%d error=%s\n",rows[i].hash,rows[i].name,rows[i].status,rows[i].metadata,rows[i].done,rows[i].size,rows[i].down,rows[i].up,rows[i].peers,rows[i].error);
        } else if (ok && count==1) {
            if (!strcmp(cmd,"details") || !strcmp(cmd,"magnet")) {
                BtDetails d={0}; ok=client_details(client,rows[0].hash,3,&d,error,sizeof(error));
                if (ok && !strcmp(cmd,"magnet")) {
                    printf("magnet:?xt=urn:btih:%s",rows[0].hash);
                    for (int i=0;i<d.tracker_count;i++) {
                        fputs("&tr=",stdout);
                        for (const unsigned char *p=(unsigned char *)d.trackers[i].url;*p;p++) printf("%%%02X",*p);
                    }
                    putchar('\n');
                } else if (ok) {
                    for (int i=0;i<d.tracker_count;i++) printf("tracker=%s error=%s\n",d.trackers[i].url,d.trackers[i].error);
                }
                bt_details_free(&d);
                if (ok && !strcmp(cmd,"details")) {
                    ok=client_details(client,rows[0].hash,2,&d,error,sizeof(error));
                    for (int i=0;ok && i<d.peer_count;i++) printf("peer_client=%s down=%.0f up=%.0f\n",d.peers[i].client,d.peers[i].down,d.peers[i].up);
                    bt_details_free(&d);
                }
            } else {
                BtAction action=!strcmp(cmd,"pause")?BT_PAUSE:!strcmp(cmd,"resume")?BT_RESUME:!strcmp(cmd,"verify")?BT_VERIFY:BT_REMOVE;
                if (strcmp(cmd,"pause") && strcmp(cmd,"resume") && strcmp(cmd,"verify") && strcmp(cmd,"remove")) ok=false;
                else ok=client_action(client,rows[0].hash,action,error,sizeof(error));
            }
        } else if (ok) { snprintf(error,sizeof(error),"Expected one torrent, got %d",count); ok=false; }
        free(rows);
    }
    if (!ok) fprintf(stderr,"%s\n",*error?error:"Invalid command");
    client_close(client); return ok?0:1;
}
