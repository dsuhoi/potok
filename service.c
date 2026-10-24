#define _GNU_SOURCE
#include "service.h"
#include "settings.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <poll.h>
#include <signal.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <time.h>

enum { POLL_STATE=1,SNAPSHOT,DETAILS,ADD_TORRENT,ACTION,FILE_PRIORITY,LIMITS,STOP };
enum { MAGIC=0x504f544b,VERSION=1,MAX_FRAME=64*1024*1024 };
typedef struct { uint32_t magic,version,op,length; int32_t error; } Header;
typedef struct {
    char source[16384],directory[PATH_MAX],hash[65];
    int action,index,down,up,tab;
    uint8_t paused,wanted,slow;
} Request;
typedef struct { uint64_t revision; int down,up,event,pid; bool slow,busy; char error[1024]; } Status;
static volatile sig_atomic_t stopped;
static void stop_signal(int sig) { (void)sig; stopped=1; }
static double seconds(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec+t.tv_nsec/1e9; }
static bool transfer(int fd,void *buffer,size_t length,bool write,double deadline) {
    char *data=buffer;
    while (length) {
        double remaining=deadline-seconds(); if (remaining<=0) return false;
        struct pollfd p={.fd=fd,.events=write?POLLOUT:POLLIN};
        int ready=poll(&p,1,(int)(remaining*1000)+1);
        if (ready<0 && errno==EINTR) continue;
        if (ready<=0) return false;
        ssize_t n=write?send(fd,data,length,MSG_NOSIGNAL|MSG_DONTWAIT):recv(fd,data,length,MSG_DONTWAIT);
        if (n<0 && (errno==EINTR || errno==EAGAIN)) continue;
        if (n<=0) return false;
        data+=n; length-=(size_t)n;
    }
    return true;
}
static bool address(const char *state,struct sockaddr_un *addr,char *error,size_t cap) {
    memset(addr,0,sizeof(*addr)); addr->sun_family=AF_UNIX;
    int n=snprintf(addr->sun_path,sizeof(addr->sun_path),"%s/control.sock",state);
    if (n<0 || n>=(int)sizeof(addr->sun_path)) { snprintf(error,cap,"State directory path is too long for a local socket"); return false; }
    return true;
}
static bool same_user(int fd) {
    struct ucred peer; socklen_t len=sizeof(peer);
    return !getsockopt(fd,SOL_SOCKET,SO_PEERCRED,&peer,&len) && peer.uid==getuid();
}
static int connect_to(struct sockaddr_un *addr) {
    int fd=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0);
    if (fd>=0 && (!connect(fd,(struct sockaddr *)addr,sizeof(*addr))) && same_user(fd)) return fd;
    if (fd>=0) close(fd);
    return -1;
}
static bool reply(int fd,int op,const void *data,size_t length,const char *error) {
    if (length>MAX_FRAME) return false;
    Header h={.magic=MAGIC,.version=VERSION,.op=(uint32_t)op,.length=(uint32_t)length};
    if (error) { h.error=1; data=error; h.length=(uint32_t)strlen(error)+1; }
    double deadline=seconds()+10;
    return transfer(fd,&h,sizeof(h),true,deadline) && (!h.length || transfer(fd,(void *)data,h.length,true,deadline));
}
static bool request(Client *c,int op,const Request *req,void **out,size_t *size,char *error,size_t cap) {
    if (out) *out=NULL;
    if (size) *size=0;
    Header h={.magic=MAGIC,.version=VERSION,.op=(uint32_t)op,.length=req?sizeof(*req):0};
    double deadline=seconds()+(op==STOP?30:10);
    if (c->fd<0 || !transfer(c->fd,&h,sizeof(h),true,deadline) || (req && !transfer(c->fd,(void *)req,sizeof(*req),true,deadline)) ||
        !transfer(c->fd,&h,sizeof(h),false,deadline)) goto disconnected;
    if (h.magic!=MAGIC || h.version!=VERSION || h.op!=(uint32_t)op || h.length>MAX_FRAME) goto disconnected;
    unsigned char *data=calloc((size_t)h.length+1,1);
    if (!data) { snprintf(error,cap,"Out of memory"); return false; }
    if (h.length && !transfer(c->fd,data,h.length,false,deadline)) { free(data); goto disconnected; }
    if (h.error) {
        snprintf(error,cap,"%s",h.length && memchr(data,0,h.length)?(char *)data:"Invalid worker response"); free(data); return false;
    }
    if (size) *size=h.length;
    if (out) *out=data; else free(data);
    return true;
disconnected:
    snprintf(error,cap,"Background process disconnected");
    if (c->fd>=0) close(c->fd);
    c->fd=-1; return false;
}
static bool spawn_worker(const char *state,const char *downloads,char *error,size_t cap) {
    char binary[PATH_MAX]; ssize_t n=readlink("/proc/self/exe",binary,sizeof(binary)-1);
    if (n<=0 || n>=(ssize_t)sizeof(binary)-1) { snprintf(error,cap,"Cannot locate executable"); return false; } binary[n]=0;
    pid_t child=fork(); if (child<0) { snprintf(error,cap,"fork: %s",strerror(errno)); return false; }
    if (!child) {
        if (setsid()<0) _exit(1);
        pid_t grandchild=fork(); if (grandchild<0) _exit(1); if (grandchild) _exit(0);
        umask(0077);
        int null=open("/dev/null",O_RDWR),log;
        char path[PATH_MAX+16]; snprintf(path,sizeof(path),"%s/worker.log",state);
        log=open(path,O_WRONLY|O_CREAT|O_APPEND|O_NOFOLLOW|O_NONBLOCK,0600);
        struct stat st;
        if (null<0 || log<0 || fstat(log,&st) || !S_ISREG(st.st_mode) || st.st_uid!=getuid()) _exit(1);
        if (dup2(null,0)<0 || dup2(null,1)<0 || dup2(log,2)<0) _exit(1);
        closefrom(3);
        execl(binary,binary,"--daemon","--state-dir",state,"--download-dir",downloads,(char *)NULL); _exit(127);
    }
    int status; while (waitpid(child,&status,0)<0) if (errno!=EINTR) return false;
    return WIFEXITED(status) && WEXITSTATUS(status)==0;
}
Client *client_open(const char *state,const char *downloads,bool start,char *error,size_t cap) {
    if (cap) *error=0;
    struct sockaddr_un addr;
    if (start && !private_directory(state,error,cap)) return NULL;
    char resolved[PATH_MAX];
    if (!realpath(state,resolved) || !address(resolved,&addr,error,cap)) { if (cap && !*error) snprintf(error,cap,"Cannot open state directory"); return NULL; }
    int fd=connect_to(&addr);
    if (fd<0 && start) {
        if (!spawn_worker(resolved,downloads,error,cap)) return NULL;
        double deadline=seconds()+15;
        do { struct timespec wait={0,50000000}; nanosleep(&wait,NULL); fd=connect_to(&addr); } while (fd<0 && seconds()<deadline);
    }
    if (fd<0) { snprintf(error,cap,"No background process. See %s/worker.log",resolved); return NULL; }
    Client *c=calloc(1,sizeof(*c));
    if (!c) { close(fd); snprintf(error,cap,"Out of memory"); return NULL; }
    c->fd=fd;
    if (client_poll(c,error,cap)<0 && c->fd<0) { client_close(c); return NULL; }
    return c;
}
void client_close(Client *c) { if (c) { if (c->fd>=0) close(c->fd); free(c); } }
int client_poll(Client *c,char *error,size_t cap) {
    void *data; size_t size;
    if (!request(c,POLL_STATE,NULL,&data,&size,error,cap)) return -1;
    if (size!=sizeof(Status)) { free(data); close(c->fd); c->fd=-1; snprintf(error,cap,"Invalid worker status"); return -1; }
    Status status; memcpy(&status,data,sizeof(status)); free(data);
    c->down=status.down; c->up=status.up; c->slow=status.slow; c->busy=status.busy; c->pid=status.pid;
    if (status.revision==c->revision) return 0;
    c->revision=status.revision;
    if (status.event<0) snprintf(error,cap,"%.*s",(int)sizeof(status.error)-1,status.error);
    return status.event;
}
bool client_stop(Client *c,char *error,size_t cap) { return request(c,STOP,NULL,NULL,NULL,error,cap); }
bool client_busy(Client *c) { return c && c->busy; }
void client_get_limits(Client *c,int *down,int *up,bool *slow) { *down=c->down; *up=c->up; *slow=c->slow; }
bool client_add(Client *c,const char *source,const char *directory,bool paused,char *error,size_t cap) {
    Request r={0};
    if (strlen(source)>=sizeof(r.source) || strlen(directory)>=sizeof(r.directory)) { snprintf(error,cap,"Path or URL is too long"); return false; }
    bool remote=!strncmp(source,"magnet:?",8) || !strncmp(source,"http://",7) || !strncmp(source,"https://",8);
    if (!remote && *source!='/' && strncmp(source,"~/",2)) {
        char cwd[PATH_MAX]; if (!getcwd(cwd,sizeof(cwd)) || snprintf(r.source,sizeof(r.source),"%s/%s",cwd,source)>=(int)sizeof(r.source)) { snprintf(error,cap,"Source path is too long"); return false; }
    } else strcpy(r.source,source);
    strcpy(r.directory,directory); r.paused=paused;
    return request(c,ADD_TORRENT,&r,NULL,NULL,error,cap);
}
bool client_action(Client *c,const char *hash,BtAction action,char *error,size_t cap) {
    Request r={.action=action}; if (strlen(hash)>=sizeof(r.hash)) return false; strcpy(r.hash,hash);
    return request(c,ACTION,&r,NULL,NULL,error,cap);
}
bool client_file(Client *c,const char *hash,int index,bool wanted,char *error,size_t cap) {
    Request r={.index=index,.wanted=wanted}; if (strlen(hash)>=sizeof(r.hash)) return false; strcpy(r.hash,hash);
    return request(c,FILE_PRIORITY,&r,NULL,NULL,error,cap);
}
bool client_limits(Client *c,int down,int up,bool slow,char *error,size_t cap) {
    Request r={.down=down,.up=up,.slow=slow};
    if (!request(c,LIMITS,&r,NULL,NULL,error,cap)) return false;
    c->down=down; c->up=up; c->slow=slow; return true;
}
bool client_snapshot(Client *c,Torrent **items,int *count,char *error,size_t cap) {
    void *data; size_t size;
    if (!request(c,SNAPSHOT,NULL,&data,&size,error,cap)) return false;
    if (size%sizeof(Torrent) || size/sizeof(Torrent)>INT_MAX) { free(data); snprintf(error,cap,"Invalid torrent list"); return false; }
    *items=data; *count=(int)(size/sizeof(Torrent)); return true;
}
bool client_details(Client *c,const char *hash,int tab,BtDetails *d,char *error,size_t cap) {
    *d=(BtDetails){0}; Request r={.tab=tab}; if (strlen(hash)>=sizeof(r.hash)) return false; strcpy(r.hash,hash);
    void *data; size_t size;
    if (!request(c,DETAILS,&r,&data,&size,error,cap)) return false;
    int n[3];
    if (size<sizeof(n)) goto invalid;
    memcpy(n,data,sizeof(n));
    if (n[0]<0 || n[1]<0 || n[2]<0 || sizeof(n)+(uint64_t)n[0]*sizeof(BtFile)+(uint64_t)n[1]*sizeof(BtPeer)+(uint64_t)n[2]*sizeof(BtTracker)!=size) goto invalid;
    d->files=calloc((size_t)n[0]+1,sizeof(BtFile)); d->peers=calloc((size_t)n[1]+1,sizeof(BtPeer)); d->trackers=calloc((size_t)n[2]+1,sizeof(BtTracker));
    if (!d->files || !d->peers || !d->trackers) { bt_details_free(d); free(data); snprintf(error,cap,"Out of memory"); return false; }
    d->file_count=n[0]; d->peer_count=n[1]; d->tracker_count=n[2];
    char *p=(char *)data+sizeof(n);
    memcpy(d->files,p,(size_t)n[0]*sizeof(BtFile)); p+=(size_t)n[0]*sizeof(BtFile);
    memcpy(d->peers,p,(size_t)n[1]*sizeof(BtPeer)); p+=(size_t)n[1]*sizeof(BtPeer);
    memcpy(d->trackers,p,(size_t)n[2]*sizeof(BtTracker)); free(data); return true;
invalid:
    free(data); snprintf(error,cap,"Invalid torrent details"); return false;
}
static bool serve(int fd,Bt *b,Status *status,bool *stop) {
    Header h; double deadline=seconds()+2;
    if (!transfer(fd,&h,sizeof(h),false,deadline) || h.magic!=MAGIC || h.version!=VERSION) return false;
    Request r={0};
    bool needs=h.op==DETAILS || h.op==ADD_TORRENT || h.op==ACTION || h.op==FILE_PRIORITY || h.op==LIMITS;
    if (h.length!=(needs?sizeof(r):0) || (needs && !transfer(fd,&r,sizeof(r),false,deadline))) return false;
    if (!memchr(r.source,0,sizeof(r.source)) || !memchr(r.directory,0,sizeof(r.directory)) || !memchr(r.hash,0,sizeof(r.hash)) || r.paused>1 || r.wanted>1 || r.slow>1) return false;
    char error[1024]={0}; bool ok=false;
    switch (h.op) {
        case POLL_STATE:
            bt_get_limits(b,&status->down,&status->up,&status->slow); status->busy=bt_busy(b);
            return reply(fd,h.op,status,sizeof(*status),NULL);
        case SNAPSHOT: {
            Torrent *items=NULL; int count;
            if (!bt_snapshot(b,&items,&count,error,sizeof(error))) break;
            bool sent=reply(fd,h.op,items,(size_t)count*sizeof(Torrent),NULL); free(items); return sent;
        }
        case DETAILS: {
            BtDetails d={0}; if (r.tab<1 || r.tab>3 || !bt_details(b,r.hash,r.tab,&d,error,sizeof(error))) break;
            size_t length=3*sizeof(int)+(size_t)d.file_count*sizeof(BtFile)+(size_t)d.peer_count*sizeof(BtPeer)+(size_t)d.tracker_count*sizeof(BtTracker);
            if (length>MAX_FRAME) { bt_details_free(&d); return reply(fd,h.op,NULL,0,"Too many files for one view"); }
            char *data=malloc(length); if (!data) { bt_details_free(&d); return reply(fd,h.op,NULL,0,"Out of memory"); }
            int counts[]={d.file_count,d.peer_count,d.tracker_count}; memcpy(data,counts,sizeof(counts)); char *p=data+sizeof(counts);
            if (d.file_count) memcpy(p,d.files,(size_t)d.file_count*sizeof(BtFile));
            p+=(size_t)d.file_count*sizeof(BtFile);
            if (d.peer_count) memcpy(p,d.peers,(size_t)d.peer_count*sizeof(BtPeer));
            p+=(size_t)d.peer_count*sizeof(BtPeer);
            if (d.tracker_count) memcpy(p,d.trackers,(size_t)d.tracker_count*sizeof(BtTracker));
            bool sent=reply(fd,h.op,data,length,NULL); free(data); bt_details_free(&d); return sent;
        }
        case ADD_TORRENT: ok=bt_add(b,r.source,r.directory,r.paused,error,sizeof(error)); break;
        case ACTION:
            if (r.action>=BT_PAUSE && r.action<=BT_DELETE) ok=bt_action(b,r.hash,(BtAction)r.action,error,sizeof(error));
            break;
        case FILE_PRIORITY: ok=bt_file(b,r.hash,r.index,r.wanted,error,sizeof(error)); break;
        case LIMITS: ok=bt_limits(b,r.down,r.up,r.slow,error,sizeof(error)); break;
        case STOP: *stop=true; return true;
        default: return reply(fd,h.op,NULL,0,"Unknown command");
    }
    return reply(fd,h.op,NULL,0,ok?NULL:*error?error:"Invalid request");
}
int service_run(const char *state,const char *downloads,bool local_only) {
    char error[1024]={0}; Bt *b=bt_open(state,downloads,local_only,error,sizeof(error));
    if (!b) { fprintf(stderr,"potok: %s\n",error); return 1; }
    char resolved[PATH_MAX]; struct sockaddr_un addr;
    if (!realpath(state,resolved) || !address(resolved,&addr,error,sizeof(error))) { bt_close(b,error,sizeof(error)); return 1; }
    int server=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0);
    struct stat st;
    if (!lstat(addr.sun_path,&st) && (!S_ISSOCK(st.st_mode) || st.st_uid!=getuid())) { fprintf(stderr,"potok: unsafe socket path\n"); if (server>=0) close(server); bt_close(b,error,sizeof(error)); return 1; }
    unlink(addr.sun_path);
    mode_t mask=umask(0077);
    bool ready=server>=0 && !bind(server,(struct sockaddr *)&addr,sizeof(addr)) && !listen(server,16);
    umask(mask);
    if (!ready) { fprintf(stderr,"potok: socket: %s\n",strerror(errno)); if (server>=0) close(server); bt_close(b,error,sizeof(error)); return 1; }
    signal(SIGTERM,stop_signal); signal(SIGINT,stop_signal); signal(SIGHUP,SIG_IGN); signal(SIGPIPE,SIG_IGN); stopped=0;
    struct pollfd fds[17]={{.fd=server,.events=POLLIN}}; for (int i=1;i<17;i++) fds[i].fd=-1;
    Status status={.pid=getpid()}; bool stop=false; int stop_fd=-1;
    while (!stopped && !stop) {
        int event=bt_poll(b,error,sizeof(error));
        if (event) {
            status.revision++; status.event=event;
            if (event<0) { snprintf(status.error,sizeof(status.error),"%s",error); fprintf(stderr,"%s\n",error); }
        }
        int count=poll(fds,17,20);
        if (count<0) { if (errno==EINTR) continue; break; }
        if (fds[0].revents&POLLIN) {
            int fd=accept4(server,NULL,NULL,SOCK_CLOEXEC);
            if (fd>=0) {
                if (!same_user(fd)) close(fd);
                else { int slot; for (slot=1;slot<17 && fds[slot].fd>=0;slot++); if (slot==17) close(fd); else fds[slot]=(struct pollfd){.fd=fd,.events=POLLIN}; }
            }
        }
        for (int i=1;i<17 && !stop;i++) if (fds[i].fd>=0 && fds[i].revents) {
            if (!(fds[i].revents&POLLIN) || !serve(fds[i].fd,b,&status,&stop)) { close(fds[i].fd); fds[i].fd=-1; }
            if (stop) stop_fd=fds[i].fd;
        }
    }
    /* Remove our socket while the engine still holds the state lock. */
    close(server); fds[0].fd=-1; unlink(addr.sun_path);
    bool ok=bt_close(b,error,sizeof(error));
    if (stop_fd>=0) reply(stop_fd,STOP,NULL,0,ok?NULL:error);
    for (int i=0;i<17;i++) if (fds[i].fd>=0) close(fds[i].fd);
    if (!ok) fprintf(stderr,"potok: %s\n",error);
    return ok?0:1;
}
