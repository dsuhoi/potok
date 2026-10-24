#define _GNU_SOURCE
#include "picker.h"
#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <sys/prctl.h>

typedef struct { size_t found; bool limited; bool (*emit)(const char *,void *); void *context; } Walk;
static bool torrent_name(const char *s) { size_t n=strlen(s); return n>8 && !strcasecmp(s+n-8,".torrent"); }
static bool walk(Walk *w,const char *path,int depth) {
    /* shortcut: cap traversal at 32 levels / 100k torrents; raise for larger archives. */
    if (depth>32 || w->found>=100000) { w->limited=true; return true; }
    DIR *dir=opendir(path); if (!dir) return true;
    struct dirent *entry; bool ok=true;
    while ((entry=readdir(dir))) {
        if (!strcmp(entry->d_name,".") || !strcmp(entry->d_name,"..")) continue;
        if (w->found>=100000) { w->limited=true; break; }
        if (entry->d_type!=DT_UNKNOWN && entry->d_type!=DT_DIR &&
            (entry->d_type!=DT_REG || !torrent_name(entry->d_name))) continue;
        char child[PATH_MAX]; int n=snprintf(child,sizeof(child),"%s%s%s",path,path[strlen(path)-1]=='/'?"":"/",entry->d_name);
        if (n<0 || n>=(int)sizeof(child)) { w->limited=true; continue; }
        struct stat st; if (lstat(child,&st)) continue;
        if (S_ISDIR(st.st_mode)) { if (!walk(w,child,depth+1)) { ok=false; break; } }
        else if (S_ISREG(st.st_mode) && torrent_name(child)) { w->found++; if (!w->emit(child,w->context)) { ok=false; break; } }
    }
    closedir(dir); return ok;
}
static bool root_path(const char *path,char root[PATH_MAX],char *error,size_t cap) {
    if (!realpath(path,root)) { snprintf(error,cap,"%s: %s",path,strerror(errno)); return false; }
    DIR *dir=opendir(root); if (!dir) { snprintf(error,cap,"%s: %s",root,strerror(errno)); return false; } closedir(dir); return true;
}
static bool collect(const char *path,void *context) {
    FileList *list=context; char **p=realloc(list->paths,(list->count+1)*sizeof(*p)); if (!p) return false;
    list->paths=p; p[list->count]=strdup(path); if (!p[list->count]) return false; list->count++; return true;
}
static int compare(const void *a,const void *b) { return strcasecmp(*(char *const *)a,*(char *const *)b); }
bool files_scan(FileList *list,const char *root,char *error,size_t cap) {
    memset(list,0,sizeof(*list)); if (!root_path(root,list->root,error,cap)) return false;
    Walk w={.emit=collect,.context=list};
    if (!walk(&w,list->root,0)) { files_free(list); snprintf(error,cap,"Out of memory"); return false; }
    list->limited=w.limited; if (list->count) qsort(list->paths,list->count,sizeof(*list->paths),compare); return true;
}
bool directories_scan(FileList *list,const char *root,char *error,size_t cap) {
    memset(list,0,sizeof(*list)); if (!root_path(root,list->root,error,cap)) return false;
    DIR *dir=opendir(list->root);
    if (!dir) { snprintf(error,cap,"%s: %s",root,strerror(errno)); return false; }
    struct dirent *entry; bool ok=true;
    while ((entry=readdir(dir))) {
        if (!strcmp(entry->d_name,".") || !strcmp(entry->d_name,"..")) continue;
        char path[PATH_MAX]; struct stat st;
        int n=snprintf(path,sizeof(path),"%s%s%s",list->root,!strcmp(list->root,"/")?"":"/",entry->d_name);
        if (n<0 || n>=(int)sizeof(path)) { list->limited=true; continue; }
        if (!stat(path,&st) && S_ISDIR(st.st_mode) && !collect(path,list)) { ok=false; break; }
    }
    closedir(dir);
    if (!ok) { files_free(list); snprintf(error,cap,"Out of memory"); return false; }
    if (list->count) qsort(list->paths,list->count,sizeof(*list->paths),compare);
    return true;
}
void files_free(FileList *list) { for (size_t i=0;i<list->count;i++) free(list->paths[i]); free(list->paths); memset(list,0,sizeof(*list)); }
bool file_matches(const char *path,const char *query) {
    for (;*query;query++) {
        while (*path && tolower((unsigned char)*path)!=tolower((unsigned char)*query)) path++;
        if (!*path) return false;
        path++;
    }
    return true;
}
static bool executable(const char *name,char result[PATH_MAX]) {
    const char *path=getenv("PATH"); if (!path) path="/usr/bin:/bin";
    char *copy=strdup(path); if (!copy) return false;
    char *part=copy;
    while (part) {
        char *next=strchr(part,':'); if (next) *next++=0;
        int n=snprintf(result,PATH_MAX,"%s/%s",*part?part:".",name);
        if (n>0 && n<PATH_MAX && !access(result,X_OK)) { free(copy); return true; }
        part=next;
    }
    free(copy); return false;
}
static bool emit_file(const char *path,void *context) {
    size_t skip=*(size_t *)context;
    return fwrite(path+skip,1,strlen(path+skip)+1,stdout)==strlen(path+skip)+1 && !fflush(stdout);
}
int picker_fzf(const char *root,const char *query,char selected[PATH_MAX],const volatile sig_atomic_t *cancel,char *error,size_t cap) {
    char binary[PATH_MAX],resolved[PATH_MAX]; if (!executable("fzf",binary)) return 2;
    if (!root_path(root,resolved,error,cap)) return -1;
    int input[2]; FILE *output=tmpfile();
    if (!output || pipe2(input,O_CLOEXEC)) { if (output) fclose(output); snprintf(error,cap,"Cannot open file picker: %s",strerror(errno)); return -1; }
    pid_t producer=fork();
    if (producer<0) { close(input[0]); close(input[1]); fclose(output); snprintf(error,cap,"Cannot start file search"); return -1; }
    if (!producer) {
        prctl(PR_SET_PDEATHSIG,SIGTERM);
        signal(SIGPIPE,SIG_DFL); signal(SIGTERM,SIG_DFL); signal(SIGINT,SIG_DFL);
        if (dup2(input[1],STDOUT_FILENO)<0) _exit(1);
        closefrom(3);
        size_t skip=strlen(resolved)+1; if (!strcmp(resolved,"/")) skip=1;
        Walk w={.emit=emit_file,.context=&skip}; bool ok=walk(&w,resolved,0); if (fflush(stdout)) ok=false; _exit(ok?0:1);
    }
    pid_t child=fork();
    if (!child) {
        prctl(PR_SET_PDEATHSIG,SIGTERM);
        signal(SIGINT,SIG_DFL); signal(SIGTERM,SIG_DFL);
        if (dup2(input[0],0)<0 || dup2(fileno(output),1)<0) _exit(1);
        closefrom(3);
        execl(binary,binary,"--read0","--print0","--no-multi","--no-print-query","--no-expect","--layout=reverse","--border=rounded","--color=bg:-1,bg+:-1,gutter:-1,hl:85,hl+:85,pointer:85","--header",resolved,"--prompt=.torrent > ","--query",query,(char *)NULL);
        _exit(127);
    }
    close(input[0]); close(input[1]);
    int status=0;
    if (child>0) for (;;) {
        pid_t result=waitpid(child,&status,WNOHANG);
        if (result==child) break;
        if (result<0 && errno!=EINTR) { status=-1; break; }
        if (cancel && *cancel) kill(child,SIGTERM);
        struct timespec wait={0,20000000}; nanosleep(&wait,NULL);
    }
    kill(producer,SIGTERM); while (waitpid(producer,NULL,0)<0 && errno==EINTR);
    if (child<0 || status<0 || !WIFEXITED(status)) { fclose(output); snprintf(error,cap,"File picker failed"); return -1; }
    int result=WEXITSTATUS(status);
    if (result==1 || result==130) { fclose(output); return 0; }
    if (result!=0) { fclose(output); snprintf(error,cap,"fzf exited with code %d",result); return -1; }
    if (fseek(output,0,SEEK_SET)) { fclose(output); snprintf(error,cap,"Cannot read file selection"); return -1; }
    char relative[PATH_MAX]; size_t n=fread(relative,1,sizeof(relative),output); bool failed=ferror(output); fclose(output);
    if (failed) { snprintf(error,cap,"Cannot read file selection"); return -1; }
    char *end=memchr(relative,0,n);
    if (!end || !*relative || *relative=='/' || !torrent_name(relative)) { snprintf(error,cap,"Invalid file selection"); return -1; }
    char candidate[PATH_MAX];
    if (snprintf(candidate,sizeof(candidate),"%s/%s",resolved,relative)>=(int)sizeof(candidate) || !realpath(candidate,selected)) { snprintf(error,cap,"Selected path is too long or unavailable"); return -1; }
    size_t prefix=strlen(resolved);
    if (strcmp(resolved,"/") && (strncmp(selected,resolved,prefix) || selected[prefix]!='/')) { snprintf(error,cap,"Selection escaped the search directory"); return -1; }
    struct stat st;
    if (lstat(selected,&st) || !S_ISREG(st.st_mode)) { snprintf(error,cap,"Selected file is unavailable"); return -1; }
    return 1;
}
