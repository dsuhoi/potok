/* Potok terminal client. */
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#include <curses.h>
#include "service.h"
#include "settings.h"
#include "picker.h"
#include <poll.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>
#include <wctype.h>
#define COUNT(a) (sizeof(a) / sizeof((a)[0]))
#define MIN(a,b) ((a) < (b) ? (a) : (b))
#define MAX(a,b) ((a) > (b) ? (a) : (b))
#include "lang.h"

typedef struct { int x, y, w, h, key; } Hit;
enum { NORMAL, ADD, SEARCH, REMOVE, LIMIT_DOWN, LIMIT_UP, HELP, FILES, STOP_DAEMON, DIRECTORIES };
enum { PASTE_BEGIN=0x6000, PASTE_END, HIT_SOURCE=-1, HIT_DIRECTORY=-2, HIT_QUERY=-3, HIT_BROWSE=-4, HIT_REMOVE_DATA=-5, HIT_TREE=-6, HIT_CHOOSE_DIR=-7, HIT_PARENT=-8, HIT_USE_DIR=-9 };
typedef struct { wchar_t value[4096]; size_t length,cursor,scroll; } Line;
typedef struct { char *path; int parent,depth,file; bool expanded,match; } TreeNode;
typedef struct {
    Client *engine;
    Settings settings;
    double next_update, next_detail;
    Torrent *items;
    int count, *visible, total, selected, scroll, filter, tab, detail_row, detail_scroll;
    bool detail_focus, demo, quit, turtle, russian;
    BtDetails detail;
    int detail_tab;
    char detail_hash[65], query[256], notice[1024], target[65], target_name[1024];
    char downloads[PATH_MAX];
    Line input,destination,file_query;
    int field; bool pasting,remove_data;
    mbstate_t decoder;
    FileList files; size_t *matches; int file_count,file_row,file_scroll;
    TreeNode *tree; size_t tree_count; char *tree_query; bool file_tree;
    int modal, down_limit;
    int openers;
    double notice_until, demo_start;
    int width, height, side, table_y, table_bottom, pane_y;
    Hit hits[64]; int nhits;
} App;

static const char *tr(const App *a,const char *s) { return translate(a->russian,s); }

static volatile sig_atomic_t interrupted;
static void on_signal(int sig) { (void)sig; interrupted = 1; }
static double now(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}
static void *allocate(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) { endwin(); fputs("potok: out of memory\n", stderr); exit(1); }
    return p;
}
static void notice(App *a, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); vsnprintf(a->notice, sizeof(a->notice), tr(a,fmt), ap); va_end(ap);
    a->notice_until = now() + 7;
}
static Torrent *selected(App *a) {
    return a->selected >= 0 && a->selected < a->total ? &a->items[a->visible[a->selected]] : NULL;
}
static bool contains(const char *s,const char *query) {
    size_t len=strlen(query);
    for (;*s;s++) if (!strncasecmp(s,query,len)) return true;
    return !len;
}
static bool completed(const Torrent *t) { return t->metadata>=1 && t->done>=1; }
static bool progress_active(const Torrent *t) { return t->status==2 || (t->status==4 && !completed(t)); }
static bool filter_matches(const Torrent *t,int filter) {
    return !filter || (filter==1 && !completed(t) && (t->status==3||t->status==4)) ||
        (filter==2 && completed(t)) || (filter==3 && !t->status) || (filter==4 && *t->error);
}
static void refilter(App *a, const char *hash) {
    a->total = 0; int keep = -1;
    for (int i = 0; i < a->count; i++) {
        Torrent *t = &a->items[i];
        bool match = filter_matches(t,a->filter);
        if (!match || !contains(t->name,a->query)) continue;
        a->visible[a->total] = i;
        if (hash && !strcmp(t->hash,hash)) keep = a->total;
        a->total++;
    }
    a->selected = keep >= 0 ? keep : MAX(0,MIN(a->selected,a->total-1));
}
static void engine_tick(App *a) {
    char error[1024]; double time=now();
    if (time<a->next_update && (!a->tab || time<a->next_detail)) return;
    int event=client_poll(a->engine,error,sizeof(error));
    if (event<0) { notice(a,"Error: %s",tr(a,error)); if (a->engine->fd<0) { a->quit=true; return; } }
    if (event>0) a->next_update=a->next_detail=0;
    if (time>=a->next_update) {
        Torrent *items=NULL; int count=0;
        if (client_snapshot(a->engine,&items,&count,error,sizeof(error))) {
            char keep[65]=""; Torrent *current=selected(a); if (current) strcpy(keep,current->hash);
            for (int i=0;i<count;i++) if (i<a->count && !strcmp(items[i].hash,a->items[i].hash)) items[i].shown=a->items[i].shown;
            free(a->items); free(a->visible); a->items=items; a->count=count;
            a->visible=allocate((size_t)count*sizeof(*a->visible)); refilter(a,keep);
            int down,up; client_get_limits(a->engine,&down,&up,&a->turtle);
        } else notice(a,"Error: %s",tr(a,error));
        a->next_update=time+.5;
    }
    Torrent *t=selected(a);
    if (a->tab && t && (time>=a->next_detail || a->tab!=a->detail_tab || strcmp(t->hash,a->detail_hash))) {
        BtDetails details={0};
        if (client_details(a->engine,t->hash,a->tab,&details,error,sizeof(error))) {
            bt_details_free(&a->detail); a->detail=details;
            strcpy(a->detail_hash,t->hash); a->detail_tab=a->tab;
        } else notice(a,"Error: %s",tr(a,error));
        a->next_detail=time+1;
    }
}
static bool run_action(App *a,const char *hash,BtAction action) {
    if (a->demo) { notice(a,"Demo"); return false; }
    char error[1024];
    if (!client_action(a->engine,hash,action,error,sizeof(error))) { notice(a,"Error: %s",tr(a,error)); return false; }
    a->next_update=a->next_detail=0; return true;
}

/* Drawing uses terminal cells, not UTF-8 byte counts. Remote control characters are never printed. */
enum { BASE=1, DIM, ACCENT, BLUE, WARM, BAD, SELECT, PANEL, SELECT_ACCENT, WHITE };
static bool drawing_modal;
static void text(int y, int x, int width, int color, const char *s) {
    if (y<0 || y>=LINES || x<0 || x>=COLS || width<=0) return;
    width=MIN(width,COLS-x);
    if (drawing_modal && color!=SELECT && color!=SELECT_ACCENT) color+=10;
    attrset(COLOR_PAIR(color));
    mbstate_t state={0}; const char *p=s; int used=0;
    while (*p && used<width) {
        wchar_t c; size_t n=mbrtowc(&c,p,MB_CUR_MAX,&state);
        if (n==(size_t)-1 || n==(size_t)-2) { memset(&state,0,sizeof(state)); p++; c=L'?'; }
        else { if (!n) break; p+=n; }
        int cells=wcwidth(c);
        if (cells<0 || c==0x202e || c==0x202d || (c>=0x2066 && c<=0x2069)) { c=L' '; cells=1; }
        if (!cells) continue;
        if (used+cells>width) break;
        mvaddnwstr(y,x+used,&c,1); used+=cells;
    }
}
static void label(const App *a,int y,int x,int width,int color,const char *s) {
    text(y,x,width,color,tr(a,s));
}
static int cell_width(const char *s) {
    mbstate_t state={0}; int width=0;
    while (*s) {
        wchar_t c; size_t n=mbrtowc(&c,s,MB_CUR_MAX,&state);
        if (n==(size_t)-1 || n==(size_t)-2) { memset(&state,0,sizeof(state)); s++; width++; }
        else { if (!n) break; s+=n; width+=MAX(0,wcwidth(c)); }
    }
    return width;
}
static void drawf(const App *a,int y,int x,int w,int color,const char *fmt,...) {
    char s[2048]; va_list ap; va_start(ap,fmt); vsnprintf(s,sizeof(s),tr(a,fmt),ap); va_end(ap); text(y,x,w,color,s);
}
static void fill(int y,int x,int w,int color) {
    if (y<0 || y>=LINES || x<0 || x>=COLS || w<=0) return;
    attrset(COLOR_PAIR(color)); mvhline(y,x,' ',MIN(w,COLS-x));
}
static void rule(int y,int x,int w) { for (int i=0;i<w;i++) text(y,x+i,1,DIM,"─"); }
static void hit(App *a,int y,int x,int w,int h,int key) {
    if (a->nhits<(int)COUNT(a->hits)) a->hits[a->nhits++]=(Hit){x,y,w,h,key};
}
static void button(App *a,int y,int x,const char *label,int key,int color) {
    label=tr(a,label); int w=cell_width(label); text(y,x,w,color,label); hit(a,y,x,w,1,key);
}
static const char *bytes(const App *a,double n,char out[32]) {
    const char *en[]={"B","KiB","MiB","GiB","TiB"}, *ru[]={"Б","КиБ","МиБ","ГиБ","ТиБ"};
    const char **units=a->russian?ru:en; int i=0;
    n=MAX(0,n); while (n>=1024 && i<4) { n/=1024; i++; }
    snprintf(out,32,n>=100||!i?"%.0f %s":"%.1f %s",n,units[i]); return out;
}
static const char *eta(const App *a,double n,char out[32]) {
    if (n<0) snprintf(out,32,"—");
    else if (n>=86400) snprintf(out,32,a->russian?"%.0fд %.0fч":"%.0fd %.0fh",floor(n/86400),floor(fmod(n,86400)/3600));
    else if (n>=3600) snprintf(out,32,a->russian?"%.0fч %.0fм":"%.0fh %.0fm",floor(n/3600),floor(fmod(n,3600)/60));
    else snprintf(out,32,a->russian?"%.0fм %.0fс":"%.0fm %.0fs",floor(n/60),floor(fmod(n,60)));
    return out;
}
static const char *status_name(const App *a,const Torrent *t) {
    static const char *names[]={"Paused","Check queued","Verifying","Queued","Downloading","Seed queued","Seeding"};
    return tr(a,*t->error?"Error":t->metadata<1&&t->status==4?"Metadata":names[t->status]);
}
static void progress(int y,int x,int w,double value,bool active,bool chosen,double time) {
    int eighths=(int)(MAX(0,MIN(1,value))*w*8);
    static const char *blocks[]={" ","▏","▎","▍","▌","▋","▊","▉","█"};
    for (int i=0;i<w;i++) {
        int part=MAX(0,MIN(8,eighths-i*8));
        if (value<0) { int head=(int)(time*10)%(w+3); part=i>=head-2 && i<=head?8:0; }
        int c=chosen?SELECT_ACCENT:ACCENT;
        if (active && part && (i+(int)(time*14))%23<3) c=chosen?SELECT:WHITE;
        text(y,x+i,1,part?c:(chosen?SELECT:DIM),part?blocks[part]:"━");
    }
}
static int detail_count(App *a) {
    if (a->tab==1) return a->detail.file_count;
    if (a->tab==2) return a->detail.peer_count;
    return a->tab==3?a->detail.tracker_count:0;
}
static void draw_details(App *a,double time) {
    int x=a->side+3, y=a->pane_y, w=a->width-x-2, bottom=a->height-4;
    rule(y,a->side+1,a->width-a->side-3);
    const char *tabs[]={tr(a,"Overview"),tr(a,"Files"),tr(a,"Peers"),tr(a,"Trackers")}; int tx=x;
    for (int i=0;i<4;i++) { button(a,y+1,tx,tabs[i],200+i,a->tab==i?ACCENT:DIM); tx+=cell_width(tabs[i])+4; }
    label(a,y+1,a->width-20,17,DIM,a->detail_focus?"TAB → torrent list":"TAB → details");
    Torrent *t=selected(a);
    if (!t) { label(a,y+3,x,w,DIM,"Select a torrent"); return; }
    if (a->tab==0) {
        char b[32], e[32];
        text(y+3,x,w,WHITE,t->name);
        drawf(a,y+5,x,w,BASE,"%s   ·   %s   ·   %d peers   ·   ratio %.2f",status_name(a,t),bytes(a,t->size,b),t->peers,MAX(0,t->ratio));
        if (completed(t)) label(a,y+6,x,w,ACCENT,"Ready");
        else drawf(a,y+6,x,w,DIM,"ETA %s   ·   queue #%d   ·   %.1f%% complete",eta(a,t->eta,e),t->queue+1,t->done*100);
        if (y+8<bottom) drawf(a,y+8,x,w,DIM,"SAVE TO  %s",t->dir);
        if (*t->error && y+9<bottom) text(y+9,x,w,BAD,t->error);
        return;
    }
    if (strcmp(t->hash,a->detail_hash) || a->tab!=a->detail_tab) { label(a,y+3,x,w,DIM,"Loading…"); return; }
    int n=detail_count(a), rows=MAX(1,bottom-y-4);
    a->detail_row=MAX(0,MIN(a->detail_row,n-1));
    if (a->detail_row<a->detail_scroll) a->detail_scroll=a->detail_row;
    if (a->detail_row>=a->detail_scroll+rows) a->detail_scroll=a->detail_row-rows+1;
    drawf(a,y+2,x,w,DIM,a->tab==1?"%d files":a->tab==2?"%d peers":"%d trackers",n);
    for (int k=a->detail_scroll;k<n && k<a->detail_scroll+rows;k++) {
        int row=y+3+k-a->detail_scroll;
        char b[32]; bool chosen=a->detail_focus && k==a->detail_row;
        if (chosen) fill(row,x,w,SELECT);
        int c=chosen?SELECT:BASE;
        if (a->tab==1) {
            BtFile *f=&a->detail.files[k];
            text(row,x,3,f->wanted?(chosen?SELECT_ACCENT:ACCENT):c,f->wanted?"[✓]":"[ ]");
            drawf(a,row,x+4,7,c,"%5.1f%%",f->size?f->done/f->size*100:100);
            text(row,x+12,w-25,c,f->name); text(row,x+w-12,12,c,bytes(a,f->size,b));
            hit(a,row,x,3,1,300+k);
        } else if (a->tab==2) {
            BtPeer *p=&a->detail.peers[k]; text(row,x,MIN(40,w/3),c,p->address);
            text(row,x+w/3,w/3,c,p->client); drawf(a,row,x+w*2/3,w/3,c,"↓ %s/s",bytes(a,p->down,b));
        } else {
            BtTracker *tracker=&a->detail.trackers[k];
            text(row,x,w/2,c,tracker->url); text(row,x+w/2,w/2,c,tracker->error);
        }
    }
    if (!n) label(a,y+4,x,w,DIM,"Empty");
    (void)time;
}
static Line *active_line(App *a);
static void choose_file(App *a,bool tree);
static void filter_files(App *a);
static void selected_file(App *a,const char *path);
static bool line_set(Line *line,const char *text);
static bool line_get(const Line *line,char *text,size_t cap);
static bool absolute_path(const char *value,char out[PATH_MAX]);
static void choose_directory(App *a,const char *root);
static bool destination_space(const Line *line,double *available) {
    char value[16384],path[PATH_MAX]; struct statvfs fs;
    if (!line_get(line,value,sizeof(value)) || !*value || !absolute_path(value,path)) return false;
    size_t length=strlen(path); while (length>1 && path[length-1]=='/') path[--length]=0;
    while (statvfs(path,&fs)) {
        if (errno!=ENOENT) return false;
        char *slash=strrchr(path,'/'); if (!slash || !slash[1]) return false;
        if (slash==path) slash[1]=0; else *slash=0;
    }
    *available=(double)fs.f_bavail*fs.f_frsize; return true;
}
static const char *modal_title(int modal) {
    switch (modal) {
        case ADD:return "ADD TORRENT"; case FILES:return "FILES"; case DIRECTORIES:return "SAVE TO"; case STOP_DAEMON:return "STOP DOWNLOADS";
        case SEARCH:return "SEARCH"; case REMOVE:return "REMOVE TORRENT";
        case LIMIT_DOWN:return "DOWNLOAD LIMIT";
        case LIMIT_UP:return "UPLOAD LIMIT"; default:return "KEYBOARD & MOUSE";
    }
}
typedef struct { int y,x; } Caret;
static Caret draw_line(App *a,Line *line,int y,int x,int width,bool active,int which) {
    if (line->scroll>line->cursor) line->scroll=line->cursor;
    int cells=0;
    for (size_t i=line->scroll;i<line->cursor;i++) cells+=MAX(0,wcwidth(line->value[i]));
    while (cells>=width-3 && line->scroll<line->cursor) {
        cells-=MAX(0,wcwidth(line->value[line->scroll])); line->scroll++;
    }
    fill(y,x,width,SELECT);
    char value[16384]; mbstate_t state={0}; const wchar_t *src=line->value+line->scroll;
    size_t n=wcsrtombs(value,&src,sizeof(value)-1,&state); value[n==(size_t)-1?0:n]=0;
    text(y,x+1,width-2,active?SELECT_ACCENT:SELECT,value);
    hit(a,y,x,width,1,which);
    return (Caret){y,x+1+MIN(cells,width-3)};
}
static void draw_modal(App *a) {
    int w=MIN(a->width-6,90), h=a->modal==HELP?24:a->modal==ADD?18:(a->modal==FILES||a->modal==DIRECTORIES)?MIN(a->height-4,24):12;
    if (a->modal!=ADD && a->modal!=FILES && a->modal!=DIRECTORIES) w=MIN(w,76);
    int x=(a->width-w)/2,y=MAX(0,(a->height-h)/2); h=MIN(h,a->height);
    Caret caret={0}; bool editing=false;
    for (int i=0;i<h;i++) fill(y+i,x,w,PANEL);
    rule(y,x,w); rule(y+h-1,x,w);
    for (int i=1;i<h-1;i++) { text(y+i,x,1,DIM,"│"); text(y+i,x+w-1,1,DIM,"│"); }
    text(y,x,1,DIM,"╭"); text(y,x+w-1,1,DIM,"╮"); text(y+h-1,x,1,DIM,"╰"); text(y+h-1,x+w-1,1,DIM,"╯");
    label(a,y+1,x+3,w-6,ACCENT,modal_title(a->modal)); a->nhits=0;
    if (a->modal==HELP) {
        const char *lines[]={"↑ ↓ / j k       Select row","Tab              Switch field / pane","← →              Details tabs","1 … 5            Switch sidebar section","a / f            Add / choose .torrent","F2 / Ctrl-F      Browse active field","Ctrl-T           Tree / folder picker","Enter            Open download folder","Space / p        Pause / resume","/ / l            Search / EN-RU","v                Verify data","K / J            Move in queue","t / s            Slow mode / speed limits","d / D            Remove torrent","q / Ctrl-C       Close TUI; keep downloading","Q                Stop downloads and quit","Mouse / wheel    Select / scroll","Esc              Cancel","Ctrl-U / Ctrl-W  Clear field / delete word"};
        for (size_t i=0;i<COUNT(lines) && y+3+(int)i<y+h-2;i++) label(a,y+3+(int)i,x+3,w-6,BASE,lines[i]);
        button(a,y+h-2,x+3,"[ Close ]",27,ACCENT); return;
    }
    if (a->modal==ADD) {
        const char *browse=tr(a,"[F2 Browse]"); int bx=x+w-3-cell_width(browse);
        label(a,y+3,x+3,bx-x-4,a->field==0?ACCENT:DIM,"Magnet / URL / .torrent");
        button(a,y+3,bx,browse,HIT_BROWSE,a->field==0?ACCENT:DIM);
        Caret source=draw_line(a,&a->input,y+4,x+3,w-6,a->field==0,HIT_SOURCE);
        button(a,y+6,x+3,"[Ctrl-T Tree]",HIT_TREE,DIM);
        label(a,y+8,x+3,bx-x-4,a->field==1?ACCENT:DIM,"Save to");
        button(a,y+8,bx,browse,HIT_CHOOSE_DIR,a->field==1?ACCENT:DIM);
        Caret destination=draw_line(a,&a->destination,y+9,x+3,w-6,a->field==1,HIT_DIRECTORY);
        double available; char b[32];
        if (destination_space(&a->destination,&available)) drawf(a,y+10,x+3,w-6,DIM,"Free: %s",bytes(a,available,b));
        else label(a,y+10,x+3,w-6,DIM,"Free: unavailable");
        label(a,y+12,x+3,w-6,DIM,"Tab: switch field · F2: browse");
        caret=a->field?destination:source; editing=true;
    } else if (a->modal==FILES || a->modal==DIRECTORIES) {
        text(y+2,x+3,w-6,DIM,a->files.root);
        caret=draw_line(a,&a->file_query,y+3,x+3,w-6,true,HIT_QUERY); editing=true;
        if (a->modal==DIRECTORIES) button(a,y+4,x+3,"[← Parent]",HIT_PARENT,DIM);
        int rows=h-8;
        if (a->file_row<a->file_scroll) a->file_scroll=a->file_row;
        if (a->file_row>=a->file_scroll+rows) a->file_scroll=a->file_row-rows+1;
        size_t skip=strlen(a->files.root); if (skip>1) skip++;
        for (int i=a->file_scroll;i<a->file_count && i<a->file_scroll+rows;i++) {
            int row=y+5+i-a->file_scroll; bool selected=i==a->file_row;
            if (selected) fill(row,x+3,w-6,SELECT);
            if (a->file_tree && a->modal==FILES) {
                TreeNode *node=&a->tree[a->matches[i]];
                const char *name=strrchr(node->path,'/'); name=name?name+1:node->path;
                int indent=MIN(node->depth*2,w-16);
                text(row,x+4+indent,2,selected?SELECT_ACCENT:ACCENT,node->file<0?(node->expanded?"▾":"▸"):"·");
                text(row,x+6+indent,w-10-indent,selected?SELECT_ACCENT:BASE,name);
            } else text(row,x+4,w-8,selected?SELECT_ACCENT:BASE,a->files.paths[a->matches[i]]+skip);
            hit(a,row,x+3,w-6,1,-100-i);
        }
        if (!a->file_count) label(a,y+5,x+4,w-8,DIM,a->modal==DIRECTORIES?"No folders":"No .torrent files");
        if (a->file_tree && a->modal==FILES) label(a,y+h-3,x+3,w-6,DIM,"← →: fold / unfold · Enter: select");
        if (a->modal==DIRECTORIES) label(a,y+h-3,x+3,w-6,DIM,"Enter: open folder · ←: parent");
        if (a->files.limited) label(a,y+h-3,x+3,w-6,WARM,"Search limit reached");
    } else if (a->modal==REMOVE) {
        text(y+3,x+3,w-6,WARM,a->target_name);
        button(a,y+5,x+3,a->remove_data?"[✓] Delete downloaded files":"[ ] Delete downloaded files",HIT_REMOVE_DATA,a->remove_data?BAD:BASE);
        label(a,y+7,x+3,w-6,DIM,a->remove_data?"Space: toggle · No recycle bin":"Space: toggle");
    } else {
        const char *description=a->modal==SEARCH?"Name":a->modal==STOP_DAEMON?"Stop background downloads?":"Limit in KiB/s; 0 = unlimited";
        label(a,y+3,x+3,w-6,BASE,description);
        if (a->modal!=STOP_DAEMON) { caret=draw_line(a,&a->input,y+6,x+3,w-6,true,HIT_SOURCE); editing=true; }
    }
    button(a,y+h-2,x+3,a->modal==ADD?"[ Enter: add ]":a->modal==DIRECTORIES?"[F2 Select this folder]":a->modal==FILES?"[ Enter: select ]":a->modal==REMOVE?"[ Enter: remove ]":"[ Enter: confirm ]",a->modal==DIRECTORIES?HIT_USE_DIR:'\n',a->modal==REMOVE && a->remove_data?BAD:ACCENT);
    button(a,y+h-2,x+w-18,"[ Esc: cancel ]",27,DIM);
    if (editing) { curs_set(1); move(caret.y,caret.x); }
}
static void draw(App *a,double time) {
    a->height=LINES; a->width=COLS; a->nhits=0; erase(); curs_set(0);
    if (COLS<80 || LINES<24) {
        label(a,1,2,COLS-4,ACCENT,"POTOK");
        label(a,3,2,COLS-4,BASE,"Resize terminal to at least 80 × 24. Press q to quit."); refresh(); return;
    }
    a->side=COLS>=108?21:0; int x=a->side+3,w=COLS-x-2;
    label(a,1,3,20,ACCENT,"≈  POTOK");
    const char *spin[]={"⠋","⠙","⠹","⠸","⠼","⠴","⠦","⠧","⠇","⠏"};
    if (a->demo) label(a,1,COLS-12,10,WARM,"Demo");
    else if (client_busy(a->engine)) drawf(a,1,COLS-20,18,ACCENT,"%s %s",spin[(int)(time*10)%10],tr(a,"Adding…"));
    rule(3,2,COLS-4);
    const char *filters[]={tr(a,"All torrents"),tr(a,"Downloads"),tr(a,"Completed"),tr(a,"Paused"),tr(a,"Errors")};
    int counts[5]={a->count,0,0,0,0}; double down=0,up=0,peers=0;
    for (int i=0;i<a->count;i++) { Torrent *t=&a->items[i]; down+=t->down; up+=t->up; peers+=t->peers;
        for (int k=1;k<5;k++) counts[k]+=filter_matches(t,k);
    }
    if (a->side) {
        label(a,5,3,17,DIM,"LIBRARY");
        for (int i=0;i<5;i++) {
            if (a->filter==i) fill(7+i*2,2,18,SELECT);
            drawf(a,7+i*2,3,17,a->filter==i?SELECT_ACCENT:BASE,"%s %s",a->filter==i?"▎":" ",filters[i]);
            drawf(a,8+i*2,5,14,DIM,"%d total",counts[i]); hit(a,7+i*2,2,18,2,'1'+i);
        }
        for (int y=4;y<LINES-3;y++) label(a,y,a->side,1,DIM,"│");
    } else {
        for (int i=0;i<5;i++) { text(4,3+i*15,14,a->filter==i?ACCENT:DIM,filters[i]); hit(a,4,3+i*15,14,1,'1'+i); }
    }
    char b[32]; int third=w/3;
    label(a,5,x,third,DIM,"↓  DOWNLOAD"); label(a,5,x+third,third,DIM,"↑  UPLOAD"); label(a,5,x+third*2,third,DIM,"◎  SWARM");
    drawf(a,6,x,third,ACCENT,"%s/s",bytes(a,down,b)); drawf(a,6,x+third,third,BLUE,"%s/s",bytes(a,up,b));
    drawf(a,6,x+third*2,third,BASE,"%.0f peers   %s",peers,a->turtle?tr(a,"[slow]"):"");
    if (*a->query) drawf(a,8,x,w,WARM,"Search: %s  ·  %d results",a->query,a->total);
    else drawf(a,8,x,w,WHITE,"%s  /  %02d",filters[a->filter],a->total);
    label(a,9,x,w,DIM,"  NAME"); label(a,9,x+w-35,12,DIM,"PROGRESS"); label(a,9,x+w-21,10,DIM,"DOWN"); label(a,9,x+w-10,10,DIM,"UP");
    a->table_y=11; a->pane_y=LINES>=32?MAX(19,LINES-15):a->detail_focus?9:LINES-4; a->table_bottom=a->pane_y-1;
    int rows=MAX(1,(a->table_bottom-a->table_y)/3);
    a->scroll=MAX(0,MIN(a->scroll,MAX(0,a->total-rows)));
    if (a->selected<a->scroll) a->scroll=a->selected;
    if (a->selected>=a->scroll+rows) a->scroll=a->selected-rows+1;
    for (int k=a->scroll;k<a->total && k<a->scroll+rows;k++) {
        Torrent *t=&a->items[a->visible[k]]; int y=a->table_y+(k-a->scroll)*3;
        bool chosen=k==a->selected; int c=chosen?SELECT:BASE;
        if (chosen) { fill(y,x-1,w+1,SELECT); fill(y+1,x-1,w+1,SELECT); label(a,y,x-1,1,SELECT_ACCENT,"▎"); }
        drawf(a,y,x+1,w-38,c,"%s",t->name);
        double value=t->status==2?t->checked:t->metadata<1&&t->status>0?-1:completed(t)?1:t->shown;
        progress(y,x+w-35,9,value,progress_active(t),chosen,time);
        if (value<0) text(y,x+w-25,5,c,"  …"); else drawf(a,y,x+w-25,5,c,"%3.0f%%",floor(value*100));
        drawf(a,y,x+w-19,9,c,"%s/s",bytes(a,t->down,b)); drawf(a,y,x+w-9,9,c,"%s/s",bytes(a,t->up,b));
        char e[32];
        if (completed(t)) drawf(a,y+1,x+1,w-2,chosen?SELECT:*t->error?BAD:DIM,"%s  ·  %s  ·  %d peers  ·  %s",status_name(a,t),bytes(a,t->size,b),t->peers,tr(a,"Ready"));
        else drawf(a,y+1,x+1,w-2,chosen?SELECT:*t->error?BAD:DIM,"%s  ·  %s  ·  %d peers  ·  ETA %s",status_name(a,t),bytes(a,t->size,b),t->peers,eta(a,t->eta,e));
    }
    if (!a->total) label(a,11,x+2,w-4,DIM,"No torrents");
    if (LINES>=32 || a->detail_focus) {
        if (LINES<32) for (int y=a->pane_y;y<LINES-3;y++) fill(y,x-1,w+1,BASE);
        draw_details(a,time);
    }
    rule(LINES-3,2,COLS-4);
    if (a->notice_until>time) text(LINES-2,3,COLS-6,WARM,a->notice);
    int bx=a->side+3;
    const char *labels[]={"[a Add]","[p Pause]","[d Remove]","[t Slow]","[s Limits]","[? Help]","[q Close]"};
    const int keys[]={'a','p','d','t','s','?','q'};
    for (size_t i=0;i<COUNT(keys);i++) { button(a,LINES-1,bx,labels[i],keys[i],i==0?ACCENT:DIM); bx+=cell_width(tr(a,labels[i]))+1; }
    button(a,LINES-1,bx,a->russian?"[l RU]":"[l EN]",'l',ACCENT);
    if (a->modal) { drawing_modal=true; draw_modal(a); drawing_modal=false; }
    refresh();
}

static Line *active_line(App *a) { return a->modal==FILES||a->modal==DIRECTORIES?&a->file_query:a->modal==ADD && a->field?&a->destination:&a->input; }
static bool line_set(Line *line,const char *text) {
    Line next={0}; mbstate_t state={0}; const char *p=text;
    size_t n=mbsrtowcs(next.value,&p,COUNT(next.value)-1,&state);
    if (n==(size_t)-1 || p) return false;
    next.length=next.cursor=n; *line=next; return true;
}
static bool line_get(const Line *line,char *text,size_t cap) {
    mbstate_t state={0}; const wchar_t *p=line->value; size_t n=wcsrtombs(text,&p,cap-1,&state);
    if (n==(size_t)-1 || p) return false;
    text[n]=0; return true;
}
static void open_modal(App *a,int modal) {
    a->modal=modal; a->field=0; a->input=(Line){0}; a->pasting=false; a->remove_data=false;
    if (modal==SEARCH) line_set(&a->input,a->query);
    if (modal==ADD) line_set(&a->destination,*a->downloads?a->downloads:"~/Downloads");
}
static void save_settings(App *a) {
    char error[1024]; if (!settings_save(&a->settings,error,sizeof(error))) notice(a,"Error: %s",tr(a,error));
}
static void clear_files(App *a) {
    for (size_t i=0;i<a->tree_count;i++) free(a->tree[i].path);
    free(a->tree); a->tree=NULL; a->tree_count=0; a->file_tree=false;
    free(a->tree_query); a->tree_query=NULL;
    files_free(&a->files); free(a->matches); a->matches=NULL;
}
static int path_compare(const void *a,const void *b) { return strcmp(*(char *const *)a,*(char *const *)b); }
static void build_tree(App *a) {
    if (a->files.count) qsort(a->files.paths,a->files.count,sizeof(*a->files.paths),path_compare);
    int parents[33]; for (int i=0;i<33;i++) parents[i]=-1;
    size_t capacity=0,skip=strlen(a->files.root); if (skip>1) skip++;
    for (size_t file=0;file<a->files.count;file++) {
        const char *path=a->files.paths[file],*part=path+skip; int depth=0,parent=-1;
        while (*part) {
            const char *slash=strchr(part,'/'); size_t len=slash?(size_t)(slash-path):strlen(path);
            if (slash && depth<33 && file && parents[depth]>=0) {
                TreeNode *previous=&a->tree[parents[depth]];
                if (strlen(previous->path)==len && !strncmp(previous->path,path,len)) { parent=parents[depth++]; part=slash+1; continue; }
            }
            if (a->tree_count==capacity) {
                capacity=capacity?capacity*2:64; TreeNode *next=realloc(a->tree,capacity*sizeof(*next));
                if (!next) { endwin(); fputs("potok: out of memory\n",stderr); exit(1); } a->tree=next;
            }
            char *name=allocate(len+1); memcpy(name,path,len); name[len]=0;
            int index=(int)a->tree_count++;
            a->tree[index]=(TreeNode){.path=name,.parent=parent,.depth=depth,.file=slash?-1:(int)file};
            if (!slash) break;
            if (depth<33) parents[depth]=index;
            /* A new branch cannot reuse descendants from the previous one. */
            for (int k=depth+1;k<33;k++) parents[k]=-1;
            parent=index; depth++; part=slash+1;
        }
    }
    free(a->matches); a->matches=allocate((a->tree_count+1)*sizeof(*a->matches));
}
static void filter_files(App *a) {
    char query[16384]; if (!line_get(&a->file_query,query,sizeof(query))) *query=0;
    a->file_count=0;
    size_t skip=strlen(a->files.root); if (skip>1) skip++;
    if (a->file_tree) {
        bool changed=!a->tree_query || strcmp(a->tree_query,query);
        if (changed) { free(a->tree_query); a->tree_query=allocate(strlen(query)+1); strcpy(a->tree_query,query); }
        for (size_t i=0;i<a->tree_count;i++) a->tree[i].match=a->tree[i].file>=0 && file_matches(a->tree[i].path+skip,query);
        for (size_t i=a->tree_count;i>0;i--) if (a->tree[i-1].match && a->tree[i-1].parent>=0) a->tree[a->tree[i-1].parent].match=true;
        int hidden_depth=INT_MAX;
        for (size_t i=0;i<a->tree_count;i++) {
            TreeNode *node=&a->tree[i]; if (!node->match) continue;
            if (changed && *query && node->file<0) node->expanded=true;
            if (node->depth<=hidden_depth) hidden_depth=INT_MAX;
            if (node->depth>hidden_depth) continue;
            a->matches[a->file_count++]=i;
            if (node->file<0 && !node->expanded) hidden_depth=node->depth;
        }
    } else for (size_t i=0;i<a->files.count;i++) if (file_matches(a->files.paths[i]+skip,query)) a->matches[a->file_count++]=i;
    a->file_row=MAX(0,MIN(a->file_row,a->file_count-1));
}
static void selected_file(App *a,const char *path) {
    if (!line_set(&a->input,path)) { notice(a,"Invalid input encoding"); return; }
    a->field=0; a->modal=ADD;
}
static void choose_file(App *a,bool tree) {
    char root[PATH_MAX],query[16384],source[16384],selected_path[PATH_MAX],error[1024];
    const char *initial=*a->settings.browse?a->settings.browse:getenv("HOME");
    if (!initial) initial="/tmp";
    snprintf(root,sizeof(root),"%s",initial); *query=0;
    if (line_get(&a->input,source,sizeof(source)) && *source && strncmp(source,"magnet:?",8) && strncmp(source,"http://",7) && strncmp(source,"https://",8)) {
        char expanded[16384];
        if (!strncmp(source,"~/",2) && getenv("HOME")) snprintf(expanded,sizeof(expanded),"%s/%s",getenv("HOME"),source+2);
        else snprintf(expanded,sizeof(expanded),"%s",source);
        struct stat st;
        if (strlen(expanded)<PATH_MAX && !stat(expanded,&st) && S_ISDIR(st.st_mode)) strcpy(root,expanded);
        else { char *slash=strrchr(expanded,'/'); if (slash) { snprintf(query,sizeof(query),"%s",slash+1); if (slash==expanded) slash[1]=0; else *slash=0; if (strlen(expanded)<PATH_MAX && !stat(expanded,&st) && S_ISDIR(st.st_mode)) strcpy(root,expanded); } else snprintf(query,sizeof(query),"%s",expanded); }
    }
    if (access(root,R_OK|X_OK) && *a->downloads && !access(a->downloads,R_OK|X_OK)) snprintf(root,sizeof(root),"%s",a->downloads);
    if (!tree) {
        def_prog_mode(); mousemask(0,NULL); fputs("\033[?2004l",stdout); fflush(stdout); endwin();
        void (*previous)(int)=signal(SIGINT,SIG_IGN);
        int result=picker_fzf(root,query,selected_path,&interrupted,error,sizeof(error));
        signal(SIGINT,previous);
        reset_prog_mode(); keypad(stdscr,TRUE); nodelay(stdscr,TRUE); mousemask(BUTTON1_PRESSED|BUTTON4_PRESSED|BUTTON5_PRESSED,NULL);
        fputs("\033[?2004h",stdout); fflush(stdout); flushinp(); clearok(stdscr,TRUE);
        if (result==1) { selected_file(a,selected_path); return; }
        if (result<0) { notice(a,"Error: %s",tr(a,error)); return; }
        if (result==0) return;
    }
    clear_files(a);
    if (!files_scan(&a->files,root,error,sizeof(error))) { notice(a,"Error: %s",tr(a,error)); return; }
    a->file_tree=true; build_tree(a);
    a->file_row=a->file_scroll=0; line_set(&a->file_query,query); filter_files(a); a->modal=FILES;
}
static void choose_directory(App *a,const char *root) {
    FileList dirs; char error[1024];
    if (!directories_scan(&dirs,root,error,sizeof(error))) { notice(a,"Error: %s",tr(a,error)); return; }
    clear_files(a); a->files=dirs; a->matches=allocate((dirs.count+1)*sizeof(*a->matches));
    a->modal=DIRECTORIES; a->file_row=a->file_scroll=0; line_set(&a->file_query,""); filter_files(a);
}
static void parent_directory(App *a) {
    char path[PATH_MAX]; snprintf(path,sizeof(path),"%s",a->files.root);
    char *slash=strrchr(path,'/'); if (!slash) return;
    if (slash==path) slash[1]=0; else *slash=0;
    choose_directory(a,path);
}
static void start_directory_picker(App *a) {
    char typed[16384],path[PATH_MAX]; struct stat st;
    if (!line_get(&a->destination,typed,sizeof(typed)) || !absolute_path(typed,path)) { notice(a,"Download path is too long"); return; }
    size_t length=strlen(path); while (length>1 && path[length-1]=='/') path[--length]=0;
    while (stat(path,&st)) {
        if (errno!=ENOENT) { notice(a,"Error: %s",strerror(errno)); return; }
        char *slash=strrchr(path,'/'); if (!slash || !slash[1]) return;
        if (slash==path) slash[1]=0; else *slash=0;
    }
    choose_directory(a,path);
}
static void poll_opener(App *a) {
    /* Only folder launchers are asynchronous children; the picker reaps its own. */
    while (a->openers) {
        int status; pid_t result=waitpid(-1,&status,WNOHANG);
        if (!result) return;
        if (result<0 && errno==EINTR) continue;
        if (result<0) a->openers=0; else a->openers--;
        if (result<0 || !WIFEXITED(status) || WEXITSTATUS(status)) notice(a,"Could not open folder (xdg-open)");
    }
}
static void open_folder(App *a,const Torrent *t) {
    if (a->demo) { notice(a,"Demo"); return; }
    char path[PATH_MAX]; struct stat st;
    if (!absolute_path(t->dir,path)) { notice(a,"Download path is too long"); return; }
    if (stat(path,&st)) { notice(a,"Error: %s",strerror(errno)); return; }
    if (!S_ISDIR(st.st_mode)) { notice(a,"Error: %s",strerror(ENOTDIR)); return; }
    pid_t child=fork();
    if (child<0) { notice(a,"Error: %s",strerror(errno)); return; }
    if (!child) {
        if (setsid()<0) _exit(1);
        int fd=open("/dev/null",O_RDWR); if (fd<0) _exit(1);
        for (int i=0;i<3;i++) if (dup2(fd,i)<0) _exit(1);
        closefrom(3); execlp("xdg-open","xdg-open",path,(char *)NULL); _exit(127);
    }
    a->openers++;
}
static bool limit_value(const char *s,int *n) {
    if (!*s) return false;
    for (const char *p=s;*p;p++) if (!isdigit((unsigned char)*p)) return false;
    errno=0; char *end; long v=strtol(s,&end,10);
    if (errno || *end || v<0 || v>2097151) return false;
    *n=(int)v; return true;
}
static void submit(App *a) {
    if (a->modal==FILES) {
        if (a->file_count) {
            if (a->file_tree) {
                TreeNode *node=&a->tree[a->matches[a->file_row]];
                if (node->file<0) { node->expanded=!node->expanded; filter_files(a); return; }
                selected_file(a,node->path);
            } else selected_file(a,a->files.paths[a->matches[a->file_row]]);
        }
        return;
    }
    if (a->modal==DIRECTORIES) { if (a->file_count) choose_directory(a,a->files.paths[a->matches[a->file_row]]); return; }
    char input[16384];
    if (!line_get(&a->input,input,sizeof(input))) { notice(a,"Invalid input encoding"); return; }
    int modal=a->modal;
    if (modal==ADD) {
        if (!*input) { choose_file(a,false); return; }
        char *source=input; while (isspace((unsigned char)*source)) source++;
        size_t length=strlen(source); while (length && isspace((unsigned char)source[length-1])) source[--length]=0;
        if (length>=2 && ((source[0]=='\"' && source[length-1]=='\"') || (source[0]=='\'' && source[length-1]=='\''))) { source[length-1]=0; source++; }
        char typed[16384],directory[PATH_MAX]; if (!line_get(&a->destination,typed,sizeof(typed))) { notice(a,"Invalid input encoding"); return; }
        if (!absolute_path(*typed?typed:a->downloads,directory)) { notice(a,"Download path is too long"); return; }
        if (a->demo) { notice(a,"Demo"); return; }
        char error[1024];
        if (!client_add(a->engine,source,directory,false,error,sizeof(error))) { notice(a,"Error: %s",tr(a,error)); return; }
        if (strcmp(directory,a->downloads)) { strcpy(a->downloads,directory); strcpy(a->settings.downloads,directory); save_settings(a); }
        a->next_update=0;
    } else if (modal==SEARCH) {
        if (strlen(input)>=sizeof(a->query)) { notice(a,"Search is too long"); return; }
        strcpy(a->query,input); a->selected=a->scroll=0; refilter(a,NULL);
    } else if (modal==REMOVE) {
        if (!run_action(a,a->target,a->remove_data?BT_DELETE:BT_REMOVE)) return;
    } else if (modal==STOP_DAEMON) {
        char error[1024];
        if (a->demo) { notice(a,"Demo"); return; }
        if (!client_stop(a->engine,error,sizeof(error))) { notice(a,"Error: %s",tr(a,error)); return; }
        a->quit=true;
    } else if (modal==LIMIT_DOWN || modal==LIMIT_UP) {
        int v; if (!limit_value(input,&v)) { notice(a,"Enter a whole number from 0 to 2097151"); return; }
        if (modal==LIMIT_DOWN) { a->down_limit=v; open_modal(a,LIMIT_UP); return; }
        if (a->demo) { notice(a,"Demo"); return; }
        char error[1024];
        if (!client_limits(a->engine,a->down_limit,v,a->turtle,error,sizeof(error))) { notice(a,"Error: %s",tr(a,error)); return; }
    }
    a->modal=NORMAL;
}
static void toggle_file(App *a,int row) {
    Torrent *t=selected(a);
    if (!t || a->detail_tab!=1 || strcmp(t->hash,a->detail_hash) || row<0 || row>=a->detail.file_count) return;
    if (a->demo) { notice(a,"Demo"); return; }
    char error[1024];
    if (!client_file(a->engine,t->hash,row,!a->detail.files[row].wanted,error,sizeof(error))) notice(a,"Error: %s",tr(a,error));
    a->next_detail=a->next_update=0;
}
static void key(App *a,int k) {
    if (k=='l' && !a->modal) { a->russian=!a->russian; a->settings.russian=a->russian; save_settings(a); notice(a,"Language: English"); return; }
    if (k==3) { a->quit=true; return; }
    if (a->modal) {
        if (k==27) { a->modal=(a->modal==FILES||a->modal==DIRECTORIES)?ADD:NORMAL; a->pasting=false; return; }
        if (a->modal==HELP) { if (k=='?' || k=='q' || k=='\n') a->modal=NORMAL; return; }
        if (k=='\n' || k==KEY_ENTER) { submit(a); return; }
        if (a->modal==REMOVE) { if (k==' ' || k==HIT_REMOVE_DATA) a->remove_data=!a->remove_data; return; }
        if (a->modal==ADD && (k==KEY_F(2) || k==6 || k==20)) { if (a->field) start_directory_picker(a); else choose_file(a,k==20); return; }
        if (a->modal==DIRECTORIES && (k==KEY_F(2) || k==HIT_USE_DIR)) {
            if (!line_set(&a->destination,a->files.root)) { notice(a,"Invalid input encoding"); return; }
            a->modal=ADD; a->field=1; return;
        }
        if (a->modal==DIRECTORIES && (k==KEY_LEFT || k==HIT_PARENT)) { parent_directory(a); return; }
        if (a->modal==DIRECTORIES && k==KEY_RIGHT) { submit(a); return; }
        if (a->modal==FILES && a->file_tree && a->file_count && (k==KEY_LEFT||k==KEY_RIGHT)) {
            TreeNode *node=&a->tree[a->matches[a->file_row]];
            if (k==KEY_RIGHT && node->file<0) node->expanded=true;
            else if (k==KEY_LEFT) {
                if (node->file<0 && node->expanded) node->expanded=false;
                else if (node->parent>=0) for (int i=0;i<a->file_count;i++) if (a->matches[i]==(size_t)node->parent) { a->file_row=i; break; }
            }
            filter_files(a); return;
        }
        if (a->modal==ADD && (k=='\t' || k==KEY_BTAB)) { a->field=!a->field; return; }
        if ((a->modal==FILES||a->modal==DIRECTORIES) && (k==KEY_UP || k==KEY_DOWN || k==KEY_NPAGE || k==KEY_PPAGE)) {
            int d=k==KEY_UP?-1:k==KEY_DOWN?1:k==KEY_NPAGE?5:-5; a->file_row=MAX(0,MIN(a->file_count-1,a->file_row+d)); return;
        }
        Line *line=active_line(a);
        if ((k==KEY_LEFT) && line->cursor) line->cursor--;
        else if (k==KEY_RIGHT && line->cursor<line->length) line->cursor++;
        else if (k==KEY_HOME || k==1) line->cursor=0;
        else if (k==KEY_END || k==5) line->cursor=line->length;
        else if ((k==KEY_BACKSPACE || k==127 || k==8) && line->cursor) {
            memmove(line->value+line->cursor-1,line->value+line->cursor,(line->length-line->cursor+1)*sizeof(wchar_t));
            line->length--; line->cursor--;
        } else if ((k==KEY_DC || k==4) && line->cursor<line->length) {
            memmove(line->value+line->cursor,line->value+line->cursor+1,(line->length-line->cursor)*sizeof(wchar_t)); line->length--;
        } else if (k==21) *line=(Line){0};
        else if (k==11) { line->length=line->cursor; line->value[line->length]=0; }
        else if (k==23) {
            size_t end=line->cursor;
            while (line->cursor && iswspace(line->value[line->cursor-1])) line->cursor--;
            while (line->cursor && !iswspace(line->value[line->cursor-1])) line->cursor--;
            memmove(line->value+line->cursor,line->value+end,(line->length-end+1)*sizeof(wchar_t)); line->length-=end-line->cursor;
        }
        if (a->modal==FILES||a->modal==DIRECTORIES) filter_files(a);
        return;
    }
    Torrent *t=selected(a);
    if (k=='q') a->quit=true;
    else if (k=='?') open_modal(a,HELP);
    else if (k=='a') open_modal(a,ADD);
    else if (k=='f') { open_modal(a,ADD); choose_file(a,false); }
    else if (k=='Q') open_modal(a,STOP_DAEMON);
    else if (k=='/') open_modal(a,SEARCH);
    else if (k=='s') open_modal(a,LIMIT_DOWN);
    else if (k=='\t') a->detail_focus=!a->detail_focus;
    else if (k==KEY_LEFT || k==KEY_RIGHT || (k>=200 && k<204)) {
        a->tab=k>=200&&k<204?k-200:(a->tab+(k==KEY_RIGHT?1:3))%4;
        if (a->height<32) a->detail_focus=true;
        a->detail_row=a->detail_scroll=0; a->next_detail=0;
    } else if (k>='1' && k<='5') { a->filter=k-'1'; a->selected=a->scroll=0; refilter(a,NULL); }
    else if (k==KEY_DOWN || k=='j' || k==KEY_UP || k=='k' || k==KEY_NPAGE || k==KEY_PPAGE) {
        int delta=(k==KEY_DOWN||k=='j')?1:(k==KEY_UP||k=='k')?-1:k==KEY_NPAGE?5:-5;
        if (a->detail_focus) a->detail_row=MAX(0,a->detail_row+delta);
        else { a->selected=MAX(0,MIN(a->total-1,a->selected+delta)); a->detail_row=a->detail_scroll=0; }
    } else if (k=='t') {
        if (a->demo) { notice(a,"Demo"); return; }
        char error[1024]; int down,up; bool slow; client_get_limits(a->engine,&down,&up,&slow);
        if (!client_limits(a->engine,down,up,!slow,error,sizeof(error))) notice(a,"Error: %s",tr(a,error));
        else a->turtle=!slow;    } else if (t) {
        if (k=='\n' || k==KEY_ENTER) open_folder(a,t);
        else if (k==' ' && a->detail_focus && a->tab==1) toggle_file(a,a->detail_row);
        else if (k==' ' || k=='p') run_action(a,t->hash,t->status?BT_PAUSE:BT_RESUME);
        else if (k=='v') run_action(a,t->hash,BT_VERIFY);
        else if (k=='K' || k=='J') run_action(a,t->hash,k=='K'?BT_UP:BT_DOWN);
        else if (k=='d' || k=='D') { strcpy(a->target,t->hash); strcpy(a->target_name,t->name); open_modal(a,REMOVE); }
    }
}
static void mouse(App *a,MEVENT e) {
    bool click=e.bstate & (BUTTON1_CLICKED|BUTTON1_PRESSED|BUTTON1_DOUBLE_CLICKED);
    if (click) for (int i=a->nhits-1;i>=0;i--) {
        Hit *h=&a->hits[i];
        if (e.x>=h->x && e.x<h->x+h->w && e.y>=h->y && e.y<h->y+h->h) {
            if (h->key==HIT_BROWSE || h->key==HIT_CHOOSE_DIR) { a->field=h->key==HIT_CHOOSE_DIR; key(a,KEY_F(2)); }
            else if (h->key==HIT_TREE) { a->field=0; key(a,20); }
            else if (h->key==HIT_SOURCE || h->key==HIT_DIRECTORY || h->key==HIT_QUERY) {
                if (a->modal==ADD) a->field=h->key==HIT_DIRECTORY;
                Line *line=active_line(a); line->cursor=line->scroll;
                int target=MAX(0,e.x-h->x-1),cells=0;
                while (line->cursor<line->length) { int width=MAX(0,wcwidth(line->value[line->cursor])); if (cells+width>target) break; cells+=width; line->cursor++; }
            } else if (h->key<=-100) {
                a->file_row=-h->key-100;
                if ((a->modal==FILES && a->file_tree && a->tree[a->matches[a->file_row]].file<0) || (e.bstate&BUTTON1_DOUBLE_CLICKED)) submit(a);
            }
            else if (h->key>=300) { a->detail_row=h->key-300; a->detail_focus=true; toggle_file(a,a->detail_row); }
            else key(a,h->key);
            return;
        }
    }
    if ((a->modal==FILES||a->modal==DIRECTORIES) && (e.bstate&(BUTTON4_PRESSED|BUTTON5_PRESSED))) { a->file_row=MAX(0,MIN(a->file_count-1,a->file_row+(e.bstate&BUTTON4_PRESSED?-1:1))); return; }
    if (a->modal || a->width<80 || a->height<24) return;
    bool wheel_up=e.bstate&BUTTON4_PRESSED, wheel_down=e.bstate&BUTTON5_PRESSED;
    if (wheel_up || wheel_down) {
        a->detail_focus=(a->height>=32 || a->detail_focus) && e.y>a->pane_y;
        key(a,wheel_up?KEY_UP:KEY_DOWN); return;
    }
    if (click && e.x>a->side && e.y>=a->table_y && e.y<a->table_bottom) {
        int row=a->scroll+(e.y-a->table_y)/3;
        if (row<a->total) { a->selected=row; a->detail_focus=false; a->detail_row=a->detail_scroll=0; }
    }
}
static void input_text(App *a,wchar_t c) {
    if (!a->modal || a->modal==REMOVE || a->modal==HELP || a->modal==STOP_DAEMON || !iswprint(c)) return;
    Line *line=active_line(a); if (line->length+1>=COUNT(line->value)) return;
    memmove(line->value+line->cursor+1,line->value+line->cursor,(line->length-line->cursor+1)*sizeof(wchar_t));
    line->value[line->cursor++]=c; line->length++;
    if (a->modal==FILES||a->modal==DIRECTORIES) filter_files(a);
}
static void input_event(App *a,wint_t c,bool special) {
    if (special && c==PASTE_BEGIN) { if (!a->modal) open_modal(a,ADD); a->pasting=true; return; }
    if (special && c==PASTE_END) { a->pasting=false; return; }
    if (a->pasting) { if (!special) input_text(a,(wchar_t)c); return; }
    if (special && c==KEY_MOUSE) { MEVENT e; if (getmouse(&e)==OK) mouse(a,e); }
    else if (special) key(a,(int)c);
    else if (a->modal && a->modal!=REMOVE && c>=32 && c!=127) input_text(a,(wchar_t)c);
    else key(a,(int)c);
}
static void input_byte(App *a,int c) {
    if (c>=KEY_MIN) { memset(&a->decoder,0,sizeof(a->decoder)); input_event(a,(wint_t)c,true); return; }
    if (c<128) { memset(&a->decoder,0,sizeof(a->decoder)); input_event(a,(wint_t)c,false); return; }
    char byte=(char)c; wchar_t value;
    size_t n=mbrtowc(&value,&byte,1,&a->decoder);
    if (n==(size_t)-1) memset(&a->decoder,0,sizeof(a->decoder));
    else if (n!=(size_t)-2) input_event(a,(wint_t)value,false);
}

static void demo_init(App *a) {
    static const char *names[]={"debian-13.1.0-amd64-netinst.iso","Big Buck Bunny · Open Movie","Fedora-Workstation-Live-x86_64.iso","Arch Linux · 2026.10.01","Blender Open Data — Classroom","FreeBSD-14.3-RELEASE-amd64.iso"};
    a->count=(int)COUNT(names); a->items=allocate(sizeof(Torrent)*COUNT(names)); a->visible=allocate(sizeof(int)*COUNT(names));
    memset(a->items,0,sizeof(Torrent)*COUNT(names)); a->demo_start=now();
    for (int i=0;i<a->count;i++) {
        Torrent *t=&a->items[i]; snprintf(t->hash,sizeof(t->hash),"%040x",i+1); snprintf(t->name,sizeof(t->name),"%s",names[i]);
        strcpy(t->dir,"~/Downloads/Torrents"); t->status=i==3?0:i==1||i==4?6:4; t->metadata=1;
        t->done=t->shown=t->status==6?1:i==3?.18:.42+i*.07; t->size=(i+1)*734003200.0; t->ratio=.42+i*.33;
        t->queue=i; t->peers=i==3?0:12+i*7; t->eta=320+i*60;
    }
    refilter(a,NULL);
}
static void demo_tick(App *a,double time) {
    for (int i=0;i<a->count;i++) {
        Torrent *t=&a->items[i]; double elapsed=time-a->demo_start;
        if (t->status==4) { t->done=fmod(.42+i*.07+elapsed*.0004,.98); t->down=(2.3+sin(elapsed*.7+i)*.6)*1048576; }
        t->up=t->status?(170+80*sin(elapsed*.5+i))*1024:0;
    }
    Torrent *t=selected(a);
    if (t && (strcmp(a->detail_hash,t->hash) || a->tab!=a->detail_tab)) {
        bt_details_free(&a->detail); a->detail_tab=a->tab;
        if (a->tab==1) {
            a->detail.file_count=2; a->detail.files=calloc(2,sizeof(BtFile));
            strcpy(a->detail.files[0].name,"image.iso"); a->detail.files[0].size=734003200; a->detail.files[0].done=332000000; a->detail.files[0].wanted=true;
            strcpy(a->detail.files[1].name,"SHA256SUMS"); a->detail.files[1].size=a->detail.files[1].done=1842; a->detail.files[1].wanted=true;
        } else if (a->tab==2) {
            a->detail.peer_count=1; a->detail.peers=calloc(1,sizeof(BtPeer));
            strcpy(a->detail.peers[0].address,"192.0.2.14"); strcpy(a->detail.peers[0].client,"qBittorrent"); a->detail.peers[0].down=1452032;
        } else if (a->tab==3) {
            a->detail.tracker_count=1; a->detail.trackers=calloc(1,sizeof(BtTracker));
            strcpy(a->detail.trackers[0].url,"https://tracker.example.org/announce");
        }
        strcpy(a->detail_hash,t->hash);
    }
}
static bool cleanup(App *a) {
    client_close(a->engine);
    bt_details_free(&a->detail); clear_files(a); free(a->items); free(a->visible); return true;
}
static void palette(void) {
    if (!has_colors()) return;
    start_color(); bool defaults=use_default_colors()==OK;
    short bg=defaults?-1:COLOR_BLACK,fg=COLOR_WHITE,dim=COLOR_WHITE,mint=COLOR_GREEN,blue=COLOR_CYAN,warm=COLOR_YELLOW,red=COLOR_RED,sel=COLOR_BLUE,panel=bg;
    if (COLORS>=256) { fg=252; dim=244; mint=85; blue=117; warm=221; red=210; sel=238; }
    init_pair(BASE,fg,bg); init_pair(DIM,dim,bg); init_pair(ACCENT,mint,bg); init_pair(BLUE,blue,bg);
    init_pair(WARM,warm,bg); init_pair(BAD,red,bg); init_pair(SELECT,fg,sel); init_pair(PANEL,fg,panel);
    init_pair(SELECT_ACCENT,mint,sel); init_pair(WHITE,COLOR_WHITE,bg); bkgd(COLOR_PAIR(BASE));
    const short foreground[]={0,fg,dim,mint,blue,warm,red,fg,fg,mint,COLOR_WHITE};
    for (short i=1;i<=10;i++) init_pair(i+10,foreground[i],panel);
}
static void usage(const App *a) {
    puts(tr(a,"Potok"));
    puts(tr(a,"Usage: potok [--lang en|ru] [--download-dir DIR] [--state-dir DIR] [--config-dir DIR] [SOURCE]"));
    puts(tr(a,"Commands: --background, --status, --stop, --demo"));
    puts(tr(a,"Keys: a add, f files, q close TUI, Q stop, ? help"));
}
static bool absolute_path(const char *value,char out[PATH_MAX]) {
    int n;
    if (!strncmp(value,"~/",2) && getenv("HOME")) n=snprintf(out,PATH_MAX,"%s/%s",getenv("HOME"),value+2);
    else if (*value=='/') n=snprintf(out,PATH_MAX,"%s",value);
    else { char cwd[PATH_MAX]; if (!getcwd(cwd,sizeof(cwd))) return false; n=snprintf(out,PATH_MAX,"%s/%s",cwd,value); }
    return n>=0 && n<PATH_MAX;
}
int main(int argc,char **argv) {
    App a={0}; const char *state=NULL,*downloads=NULL,*config=NULL,*source=NULL;
    const char *language=getenv("LC_ALL");
    if (!language || !*language) language=getenv("LC_MESSAGES");
    if (!language || !*language) language=getenv("LANG");
    a.russian=language && !strncmp(language,"ru",2);
    bool help=false,daemon=false,background=false,status=false,stop=false,lang_override=false,positional=false;
    for (int i=1;i<argc;i++) {
        if (positional || argv[i][0]!='-') {
            if (source) { fputs("potok: specify one torrent source\n",stderr); return 2; }
            source=argv[i]; continue;
        }
        if (!strcmp(argv[i],"--")) { positional=true; continue; }
        if (!strcmp(argv[i],"--help") || !strcmp(argv[i],"-h")) { help=true; continue; }
        if (!strcmp(argv[i],"--version")) { puts("potok 0.4.1"); return 0; }
        if (!strcmp(argv[i],"--demo")) a.demo=true;
        else if (!strcmp(argv[i],"--daemon")) daemon=true;
        else if (!strcmp(argv[i],"--background")) background=true;
        else if (!strcmp(argv[i],"--status")) status=true;
        else if (!strcmp(argv[i],"--stop")) stop=true;
        else if (!strcmp(argv[i],"--lang") && i+1<argc) {
            const char *lang=argv[++i];
            if (strcmp(lang,"en") && strcmp(lang,"ru")) { fputs("potok: --lang must be en or ru\n",stderr); return 2; }
            a.russian=!strcmp(lang,"ru"); lang_override=true;
        } else if ((!strcmp(argv[i],"--download-dir") || !strcmp(argv[i],"--state-dir") || !strcmp(argv[i],"--config-dir")) && i+1<argc) {
            const char *option=argv[i++]; if (!strcmp(option,"--state-dir")) state=argv[i]; else if (!strcmp(option,"--config-dir")) config=argv[i]; else downloads=argv[i];
        } else { fprintf(stderr,"potok: unknown or incomplete option: %s\n",argv[i]); return 2; }
    }
    if (help) { usage(&a); return 0; }
    if (daemon) { if (!state || !downloads || a.demo || background || stop || status || source) return 2; return service_run(state,downloads,false); }
    if ((background+status+stop)>1 || (a.demo && (background || status || stop)) || (source && (a.demo || status || stop))) {
        fputs("potok: incompatible command options\n",stderr); return 2;
    }
    if (!(background || status || stop) && (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO))) { fputs("potok: run inside a terminal\n",stderr); return 2; }
    setlocale(LC_ALL,""); setlocale(LC_NUMERIC,"C");
    if (!a.demo) {
        char error[1024]={0},config_path[PATH_MAX],state_path[PATH_MAX];
        if (config && !absolute_path(config,config_path)) { fputs("potok: config path is too long\n",stderr); return 2; }
        if (!settings_load(&a.settings,config?config_path:NULL,a.russian,error,sizeof(error))) { fprintf(stderr,"potok: %s\n",error); return 1; }
        if (!lang_override) a.russian=a.settings.russian;
        if (!absolute_path(state?state:a.settings.state,state_path) || !absolute_path(downloads?downloads:a.settings.downloads,a.downloads)) { fputs("potok: path is too long\n",stderr); return 2; }
        a.engine=client_open(state_path,a.downloads,!(stop || status),error,sizeof(error));
        if (!a.engine) { if (stop || status) { puts(tr(&a,"Not running")); return status?1:0; } fprintf(stderr,"potok: %s\n",error); return 1; }
        if (source && !client_add(a.engine,source,a.downloads,false,error,sizeof(error))) { fprintf(stderr,"potok: %s\n",error); client_close(a.engine); return 1; }
        if (stop) { bool ok=client_stop(a.engine,error,sizeof(error)); client_close(a.engine); if (!ok) { fprintf(stderr,"potok: %s\n",error); return 1; } puts(tr(&a,"Stopped")); return 0; }
        if (background || status) {
            Torrent *items=NULL; int count=0; bool ok=client_snapshot(a.engine,&items,&count,error,sizeof(error));
            if (ok) printf(tr(&a,"Running · PID %d · %d torrents\n"),(int)a.engine->pid,count); else fprintf(stderr,"potok: %s\n",error);
            free(items); client_close(a.engine); return ok?0:1;
        }
    } else {
        const char *home=getenv("HOME"); snprintf(a.downloads,sizeof(a.downloads),"%s/Downloads",home?home:"/tmp");
    }
    if (!initscr()) { cleanup(&a); return 1; }
    cbreak(); noecho(); keypad(stdscr,TRUE); meta(stdscr,TRUE); nodelay(stdscr,TRUE); set_escdelay(75); curs_set(0);
    define_key("\033[200~",PASTE_BEGIN); define_key("\033[201~",PASTE_END);
    fputs("\033[?2004h",stdout); fflush(stdout);
    palette(); mouseinterval(0); mousemask(BUTTON1_PRESSED|BUTTON4_PRESSED|BUTTON5_PRESSED,NULL);
    signal(SIGINT,on_signal); signal(SIGTERM,on_signal);
    if (a.demo) demo_init(&a);
    double last=now(),next_draw=0;
    while (!a.quit && !interrupted) {
        poll_opener(&a);
        double time=now(),dt=MIN(time-last,.1); last=time;
        if (a.demo) demo_tick(&a,time); else engine_tick(&a);
        for (int i=0;i<a.count;i++) a.items[i].shown+=(a.items[i].done-a.items[i].shown)*(1-exp(-dt*8));
        int c,processed=0;
        while (processed++<256 && (c=getch())!=ERR) input_byte(&a,c);
        if (time>=next_draw) { draw(&a,time); next_draw=time+1.0/30; }
        struct pollfd input={.fd=STDIN_FILENO,.events=POLLIN}; poll(&input,1,16);
    }
    fputs("\033[?2004l",stdout); fflush(stdout); mousemask(0,NULL); curs_set(1); endwin(); cleanup(&a); return 0;
}
