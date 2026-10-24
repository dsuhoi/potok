#define _GNU_SOURCE
#include "settings.h"
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

static bool path_value(char *out,const char *value,char *error,size_t cap) {
    if (*value!='/' || strlen(value)>=PATH_MAX || strpbrk(value,"\r\n")) {
        snprintf(error,cap,"Expected an absolute path shorter than %d bytes",PATH_MAX); return false;
    }
    strcpy(out,value); return true;
}
bool private_directory(const char *path,char *error,size_t cap) {
    if (!path || *path!='/' || strlen(path)>=PATH_MAX) { snprintf(error,cap,"Invalid private directory"); return false; }
    char copy[PATH_MAX]; strcpy(copy,path);
    for (char *p=copy+1;*p;p++) if (*p=='/') { *p=0; if (mkdir(copy,0700) && errno!=EEXIST) goto failed; *p='/'; }
    if (mkdir(copy,0700) && errno!=EEXIST) goto failed;
    struct stat st;
    if (stat(copy,&st) || !S_ISDIR(st.st_mode) || st.st_uid!=getuid() || (st.st_mode&0077)) {
        snprintf(error,cap,"Directory must be owned by you with mode 0700: %s",path); return false;
    }
    return true;
failed:
    snprintf(error,cap,"%s: %s",path,strerror(errno)); return false;
}
bool settings_save(const Settings *s,char *error,size_t cap) {
    if (!*s->directory) return true;
    if (!private_directory(s->directory,error,cap)) return false;
    char valid[PATH_MAX];
    if (!path_value(valid,s->downloads,error,cap) || !path_value(valid,s->state,error,cap) || !path_value(valid,s->browse,error,cap)) return false;
    char *path=NULL,*temp=NULL;
    if (asprintf(&path,"%s/config",s->directory)<0 || asprintf(&temp,"%s/config.tmp.XXXXXX",s->directory)<0) { free(path); free(temp); snprintf(error,cap,"Out of memory"); return false; }
    int fd=mkstemp(temp); bool ok=false;
    if (fd>=0) {
        FILE *f=fdopen(fd,"w");
        if (f) {
            ok=fprintf(f,"language=%s\ndownload_dir=%s\nstate_dir=%s\nbrowse_dir=%s\n",s->russian?"ru":"en",s->downloads,s->state,s->browse)>0;
            if (fflush(f) || fsync(fd)) ok=false;
            if (fclose(f)) ok=false;
        } else close(fd);
        if (ok && rename(temp,path)) ok=false;
        if (ok) {
            int parent=open(s->directory,O_RDONLY|O_DIRECTORY|O_CLOEXEC);
            if (parent<0) ok=false;
            else { if (fsync(parent)) ok=false; close(parent); }
        }
    }
    if (!ok) { snprintf(error,cap,"Cannot save settings: %s",strerror(errno)); unlink(temp); }
    free(path); free(temp); return ok;
}
bool settings_load(Settings *s,const char *directory,bool russian,char *error,size_t cap) {
    memset(s,0,sizeof(*s)); s->russian=russian;
    const char *home=getenv("HOME"),*xdg=getenv("XDG_CONFIG_HOME"),*data=getenv("XDG_DATA_HOME");
    if (!home || *home!='/') { snprintf(error,cap,"HOME must be an absolute path"); return false; }
    int n;
    if (directory) n=snprintf(s->directory,sizeof(s->directory),"%s",directory);
    else if (xdg && *xdg=='/') n=snprintf(s->directory,sizeof(s->directory),"%s/potok",xdg);
    else n=snprintf(s->directory,sizeof(s->directory),"%s/.config/potok",home);
    if (n<0 || n>=PATH_MAX) goto invalid;
    if (!private_directory(s->directory,error,cap)) return false;
    n=snprintf(s->downloads,sizeof(s->downloads),"%s/Downloads",home);
    if (n<0 || n>=PATH_MAX) goto invalid;
    strcpy(s->browse,s->downloads);
    n=data && *data=='/' ? snprintf(s->state,sizeof(s->state),"%s/potok",data) : snprintf(s->state,sizeof(s->state),"%s/.local/share/potok",home);
    if (n<0 || n>=PATH_MAX) goto invalid;
    char path[PATH_MAX+8]; snprintf(path,sizeof(path),"%s/config",s->directory);
    int fd=open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
    if (fd<0) {
        if (errno==ENOENT) return settings_save(s,error,cap);
        snprintf(error,cap,"Cannot read settings: %s",strerror(errno)); return false;
    }
    struct stat st;
    if (fstat(fd,&st) || !S_ISREG(st.st_mode) || st.st_size>65536 || st.st_uid!=getuid() || (st.st_mode&0022)) { close(fd); goto invalid; }
    FILE *f=fdopen(fd,"r"); if (!f) { close(fd); goto invalid; }
    char *line=NULL; size_t size=0; ssize_t length; bool ok=true;
    while ((length=getline(&line,&size,f))>=0) {
        if (memchr(line,0,(size_t)length)) { ok=false; break; }
        while (length && (line[length-1]=='\n' || line[length-1]=='\r')) line[--length]=0;
        char *key=line; while (isspace((unsigned char)*key)) key++;
        if (!*key || *key=='#') continue;
        char *value=strchr(key,'='); if (!value) { ok=false; break; }
        *value++=0; char *end=key+strlen(key); while (end>key && isspace((unsigned char)end[-1])) *--end=0;
        if (!strcmp(key,"language")) {
            if (strcmp(value,"en") && strcmp(value,"ru")) { ok=false; break; }
            s->russian=!strcmp(value,"ru");
        } else {
            char *target=!strcmp(key,"download_dir")?s->downloads:!strcmp(key,"state_dir")?s->state:!strcmp(key,"browse_dir")?s->browse:NULL;
            if (!target || !path_value(target,value,error,cap)) { ok=false; break; }
        }
    }
    if (ferror(f)) ok=false;
    free(line); fclose(f);
    if (!ok) goto invalid;
    return true;
invalid:
    snprintf(error,cap,"Invalid settings in %s/config",s->directory); return false;
}
