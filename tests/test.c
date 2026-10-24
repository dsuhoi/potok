#define main potok_main
#include "../potok.c"
#undef main
#include <assert.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <poll.h>
#include <limits.h>
#include <ftw.h>
#include <sys/syscall.h>
static volatile sig_atomic_t worker_pidfd=-1;
static void failed_test(int sig) { (void)sig; if (worker_pidfd>=0) syscall(SYS_pidfd_send_signal,worker_pidfd,SIGTERM,NULL,0); _exit(134); }

static void set_input(App *a,const wchar_t *s) {
    Line *line=active_line(a); wcscpy(line->value,s); line->length=line->cursor=wcslen(s); line->scroll=0;
}
static void signature(const char *s,char out[256]) {
    size_t n=0;
    while (*s) {
        if (*s++!='%') continue;
        if (*s=='%') { s++; continue; }
        do { assert(*s && n+1<256); out[n++]=*s; } while (!strchr("diuoxXfFeEgGaAcspn",*s++));
    }
    out[n]=0;
}
static void test_languages(void) {
    App a={0}; assert(!strcmp(tr(&a,"Paused"),"Paused")); key(&a,'l'); assert(a.russian);
    assert(!strcmp(tr(&a,"Paused"),"Пауза")); assert(!strcmp(a.notice,"Язык: русский"));
    assert(cell_width(tr(&a,"[p Pause]"))==9 && cell_width(tr(&a,"Files"))==5);
    Torrent t={.status=4,.metadata=1}; assert(!strcmp(status_name(&a,&t),"Загрузка"));
    char time[32]; assert(!strcmp(eta(&a,65,time),"1м 5с"));
    assert(!strcmp(bytes(&a,1048576,time),"1.0 МиБ"));
    a.selected=3; strcpy(a.query,"test"); key(&a,'l'); assert(!a.russian && a.selected==3 && !strcmp(a.query,"test"));
    assert(!strcmp(a.notice,"Language: English"));
    for (size_t i=0;i<COUNT(translations);i++) {
        char en[256],ru[256]; signature(translations[i].en,en); signature(translations[i].ru,ru);
        assert(!strcmp(en,ru) && *translations[i].ru);
        for (size_t k=0;k<i;k++) assert(strcmp(translations[i].en,translations[k].en));
    }
}
static void test_model_and_actions(void) {
    App a={.demo=true}; strcpy(a.downloads,"/tmp"); demo_init(&a);
    assert(a.count==6 && a.total==6);
    a.selected=1; char keep[65]; strcpy(keep,selected(&a)->hash); refilter(&a,keep); assert(a.selected==1);
    a.filter=1; refilter(&a,NULL); assert(a.total==3);
    a.filter=0; strcpy(a.query,"FEDORA"); refilter(&a,NULL); assert(a.total==1 && strstr(selected(&a)->name,"Fedora"));
    strcpy(a.query,"missing"); refilter(&a,NULL); assert(!a.total && !selected(&a));
    *a.query=0; a.selected=0; refilter(&a,NULL);
    key(&a,'d'); assert(a.modal==REMOVE); strcpy(keep,a.target); a.selected=1;
    assert(strcmp(keep,selected(&a)->hash)); submit(&a); assert(!a.modal && !strcmp(keep,a.target));
    key(&a,'D'); set_input(&a,L"delete"); submit(&a); assert(a.modal==DELETE_DATA);
    set_input(&a,L"DELETE"); submit(&a); assert(!a.modal);
    open_modal(&a,SEARCH); input_text(&a,L'п'); input_text(&a,L'т'); key(&a,KEY_LEFT); input_text(&a,L'о');
    assert(!wcscmp(a.input.value,L"пот")); key(&a,KEY_BACKSPACE); assert(!wcscmp(a.input.value,L"пт")); key(&a,27);
    open_modal(&a,ADD); set_input(&a,L"magnet:?xt=urn:btih:0123456789"); key(&a,'\t'); assert(a.field==1);
    set_input(&a,L"/tmp/downloads"); key(&a,'\t'); assert(a.field==0 && wcsstr(a.input.value,L"magnet:")); submit(&a); assert(a.modal==ADD); key(&a,27);
    int v; assert(limit_value("0",&v) && !v); assert(limit_value("2097151",&v));
    const char *invalid[]={"","-1","1.5","nan","1e3","2097152","9999999999999999","12abc"," 12"};
    for (size_t i=0;i<COUNT(invalid);i++) assert(!limit_value(invalid[i],&v));
    assert(cleanup(&a));
}

static void xml(FILE *f,const wchar_t *s) {
    for (;*s;s++) {
        if (*s=='&') fputs("&amp;",f); else if (*s=='<') fputs("&lt;",f); else if (*s=='>') fputs("&gt;",f);
        else { char out[MB_LEN_MAX]; mbstate_t st={0}; size_t n=wcrtomb(out,*s,&st); if (n!=(size_t)-1) fwrite(out,1,n,f); }
    }
}
static void snapshot(const char *path) {
    FILE *f=fopen(path,"w"); assert(f);
    fprintf(f,"<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"%d\" height=\"%d\"><g font-family=\"DejaVu Sans Mono,monospace\" font-size=\"14\">",COLS*9+32,LINES*19+32);
    const char *colors[]={"#d0d0d0","#d0d0d0","#808080","#5fffaf","#87d7ff","#ffd75f","#ffaf87","#d0d0d0","#d0d0d0","#5fffaf","#ffffff"};
    for (int y=0;y<LINES;y++) for (int x=0;x<COLS;x++) {
        cchar_t cell; wchar_t s[CCHARW_MAX]; attr_t attrs; short pair;
        assert(mvin_wch(y,x,&cell)!=ERR); assert(getcchar(&cell,s,&attrs,&pair,NULL)!=ERR);
        if (pair==SELECT || pair==SELECT_ACCENT)
            fprintf(f,"<rect x=\"%d\" y=\"%d\" width=\"9\" height=\"19\" fill=\"%s\"/>",16+x*9,16+y*19,pair==PANEL||pair>10?"#303030":"#444444");
        if (pair>10) pair-=10;
        if (*s && *s!=L' ') { fprintf(f,"<text x=\"%d\" y=\"%d\" fill=\"%s\">",16+x*9,31+y*19,colors[pair>=0&&pair<(short)COUNT(colors)?pair:0]); xml(f,s); fputs("</text>",f); }
    }
    fputs("</g></svg>",f); fclose(f);
}
static void test_ui(bool screenshots) {
    FILE *out=tmpfile(),*in=tmpfile(); assert(out && in);
    SCREEN *screen=newterm("xterm-256color",out,in); assert(screen); set_term(screen); palette();
    App a={.demo=true}; strcpy(a.downloads,"/tmp"); demo_init(&a); resizeterm(40,128); demo_tick(&a,a.demo_start+1); draw(&a,a.demo_start+1);
    assert(a.total==6 && a.side==21);
    wchar_t brand[32]; assert(mvinnwstr(1,3,brand,8)!=ERR && !wcsncmp(brand,L"≈  POTOK",8));
    short fg,bg; assert(pair_content(BASE,&fg,&bg)==OK && bg==-1);
    progress(0,0,9,-1,true,false,0.1); cchar_t frame1; assert(mvin_wch(0,0,&frame1)==OK);
    progress(0,0,9,-1,true,false,0.6); cchar_t frame2; assert(mvin_wch(0,0,&frame2)==OK);
    wchar_t one[CCHARW_MAX],two[CCHARW_MAX]; attr_t attrs; short pair;
    assert(getcchar(&frame1,one,&attrs,&pair,NULL)==OK && getcchar(&frame2,two,&attrs,&pair,NULL)==OK && wcscmp(one,two));
    draw(&a,a.demo_start+1);
    MEVENT e={.x=30,.y=a.table_y+3,.bstate=BUTTON1_PRESSED}; mouse(&a,e); assert(a.selected==1);
    e=(MEVENT){.x=30,.y=a.table_y,.bstate=BUTTON5_PRESSED}; mouse(&a,e); assert(a.selected==2);
    key(&a,KEY_RIGHT); assert(a.tab==1); demo_tick(&a,a.demo_start+1); draw(&a,a.demo_start+1);
    assert(a.detail.file_count==2);
    e=(MEVENT){.x=a.side+4,.y=a.pane_y+3,.bstate=BUTTON1_PRESSED}; mouse(&a,e); assert(a.detail_focus && a.detail_row==0);
    key(&a,'1'); key(&a,200); a.selected=0; a.detail_focus=false; a.notice_until=0; draw(&a,a.demo_start+1);
    if (screenshots) {
        mkdir("artifacts",0755); snapshot("artifacts/potok.svg"); mkdir("artifacts/frames",0755);
        for (int i=0;i<20;i++) {
            double time=a.demo_start+1+i*.1; demo_tick(&a,time);
            for (int k=0;k<a.count;k++) a.items[k].shown=a.items[k].done;
            draw(&a,time); char path[128]; snprintf(path,sizeof(path),"artifacts/frames/%02d.svg",i); snapshot(path);
        }
    }
    key(&a,'a'); draw(&a,a.demo_start+1); assert(a.modal==ADD);
    if (screenshots) snapshot("artifacts/potok-add.svg");
    for (int i=0;i<a.nhits;i++) if (a.hits[i].key==27) { e=(MEVENT){.x=a.hits[i].x,.y=a.hits[i].y,.bstate=BUTTON1_PRESSED}; mouse(&a,e); break; }
    assert(!a.modal);
    draw(&a,a.demo_start+1);
    for (int i=0;i<a.nhits;i++) if (a.hits[i].key=='l') { e=(MEVENT){.x=a.hits[i].x,.y=a.hits[i].y,.bstate=BUTTON1_PRESSED}; mouse(&a,e); break; }
    assert(a.russian); a.notice_until=0; draw(&a,a.demo_start+1);
    assert(mvinnwstr(1,3,brand,8)!=ERR && !wcsncmp(brand,L"≈  ПОТОК",8));
    wchar_t title[32]; assert(mvinnwstr(5,3,title,10)!=ERR && !wcsncmp(title,L"БИБЛИОТЕКА",10));
    if (screenshots) snapshot("artifacts/potok-ru.svg");
    if (screenshots) {
        open_modal(&a,ADD); set_input(&a,L"~/Downloads/ubuntu"); draw(&a,a.demo_start+1); snapshot("artifacts/potok-add-ru.svg");
        a.files.paths=allocate(3*sizeof(*a.files.paths)); a.files.count=3; strcpy(a.files.root,"/home/user/Downloads");
        const char *paths[]={"/home/user/Downloads/debian-13.1.0-amd64-netinst.torrent","/home/user/Downloads/Fedora-Workstation-Live-x86_64.torrent","/home/user/Downloads/ubuntu-24.04.3-desktop-amd64.torrent"};
        for (size_t i=0;i<COUNT(paths);i++) a.files.paths[i]=strdup(paths[i]);
        a.matches=allocate(3*sizeof(*a.matches)); a.modal=FILES; filter_files(&a); draw(&a,a.demo_start+1); snapshot("artifacts/potok-files-ru.svg");
        key(&a,27); key(&a,27); draw(&a,a.demo_start+1);
    }
    for (int i=0;i<a.nhits;i++) if (a.hits[i].key==201) {
        assert(a.hits[i].w==5); e=(MEVENT){.x=a.hits[i].x+4,.y=a.hits[i].y,.bstate=BUTTON1_PRESSED}; mouse(&a,e); break;
    }
    assert(a.tab==1); key(&a,200);
    const int sizes[][2]={{24,80},{32,108},{48,160},{16,60}};
    for (size_t i=0;i<COUNT(sizes);i++) { resizeterm(sizes[i][0],sizes[i][1]); draw(&a,a.demo_start+2); assert(a.height==sizes[i][0] && a.width==sizes[i][1]); }
    resizeterm(24,80); open_modal(&a,SEARCH); set_input(&a,L"Русский тест"); draw(&a,a.demo_start+2);
    assert(mvinnwstr(7,6,title,5)!=ERR && !wcsncmp(title,L"ПОИСК",5));
    if (screenshots) snapshot("artifacts/potok-search.svg");
    key(&a,27); open_modal(&a,HELP); draw(&a,a.demo_start+2);
    if (screenshots) snapshot("artifacts/potok-ru-help.svg");
    key(&a,27);
    resizeterm(16,60); a.russian=false; draw(&a,a.demo_start+2);
    assert(mvinnwstr(1,2,brand,5)!=ERR && !wcsncmp(brand,L"POTOK",5));
    a.russian=true; draw(&a,a.demo_start+2);
    assert(mvinnwstr(1,2,brand,5)!=ERR && !wcsncmp(brand,L"ПОТОК",5));
    cleanup(&a); endwin(); delscreen(screen); fclose(out); fclose(in);
}
static void terminal_read(int fd,char *buffer,size_t cap,int milliseconds) {
    double until=now()+milliseconds/1000.0; size_t n=0;
    while (now()<until && n+1<cap) {
        struct pollfd p={.fd=fd,.events=POLLIN};
        if (poll(&p,1,20)>0) {
            size_t before=n;
            ssize_t got=read(fd,buffer+n,cap-n-1);
            if (got<=0) break;
            n+=(size_t)got;
            buffer[n]=0;
            if (strstr(buffer+(before>3?before-3:0),"\033[6n")) assert(write(fd,"\033[1;1R",6)==6);
        }
    }
    buffer[n]=0;
}
static void test_terminal(bool real,bool cli) {
    const char *binary=getenv("POTOK_TEST_BINARY"); if (!binary || !*binary) binary="./potok";
    char executable[PATH_MAX]; assert(realpath(binary,executable)); binary=executable;
    char state[]="/tmp/potok-ui-XXXXXX",downloads[128],config[128],source[128];
    if (real) {
        assert(mkdtemp(state)); snprintf(downloads,sizeof(downloads),"%s/downloads",state); snprintf(config,sizeof(config),"%s/config",state);
        snprintf(source,sizeof(source),"%s/-русский файл.torrent",state); FILE *f=fopen(source,"wb"); assert(f);
        fputs("d4:infod6:lengthi4e4:name11:payload.bin12:piece lengthi16384e6:pieces20:",f);
        const unsigned char hash[]={0xa9,0x4a,0x8f,0xe5,0xcc,0xb1,0x9b,0xa6,0x1c,0x4c,0x08,0x73,0xd3,0x91,0xe9,0x87,0x98,0x2f,0xbb,0xd3};
        assert(fwrite(hash,1,sizeof(hash),f)==sizeof(hash)); fputs("7:privatei1eee",f); fclose(f);
    }
    int master=posix_openpt(O_RDWR|O_NOCTTY); assert(master>=0 && !grantpt(master) && !unlockpt(master));
    char *name=ptsname(master); assert(name); char slave_name[128]; snprintf(slave_name,sizeof(slave_name),"%s",name);
    struct winsize size={.ws_row=40,.ws_col=128}; assert(!ioctl(master,TIOCSWINSZ,&size));
    pid_t pid=fork(); assert(pid>=0);
    if (!pid) {
        assert(setsid()>=0); int slave=open(slave_name,O_RDWR); assert(slave>=0);
        for (int fd=0;fd<3;fd++) assert(dup2(slave,fd)>=0);
        close(slave); close(master); setenv("TERM","xterm-256color",1);
        unsetenv("FZF_DEFAULT_OPTS"); unsetenv("FZF_DEFAULT_OPTS_FILE");
        if (cli) { assert(!chdir(state)); execl(binary,binary,"--state-dir",state,"--download-dir",downloads,"--config-dir",config,"--lang","en","--","-русский файл.torrent",(char *)NULL); }
        else if (real) execl(binary,binary,"--state-dir",state,"--download-dir",downloads,"--config-dir",config,"--lang","en",(char *)NULL);
        else execl(binary,binary,"--demo","--lang","en",(char *)NULL);
        _exit(127);
    }
    char buffer[262144]; terminal_read(master,buffer,sizeof(buffer),real?1000:350);
    assert(strstr(buffer,"POTOK") && !strstr(buffer,"ПОТОК") && strstr(buffer,"1006"));
    if (real) {
        char startup_error[1024]={0}; Client *owner=client_open(state,downloads,false,startup_error,sizeof(startup_error));
        if (owner) { worker_pidfd=(sig_atomic_t)syscall(SYS_pidfd_open,owner->pid,0); client_close(owner); }
        assert(!strstr(buffer,"Transmission") && !strstr(buffer,"RECONNECT"));
        if (!cli) {
            assert(strstr(buffer,"No torrents"));
            assert(write(master,"a",1)==1); terminal_read(master,buffer,sizeof(buffer),150);
            assert(write(master,"\033[200~",6)==6);
            for (size_t i=0;i<strlen(source);i++) { assert(write(master,source+i,1)==1); struct timespec wait={0,3000000}; nanosleep(&wait,NULL); }
            assert(write(master,"\033[201~",6)==6); terminal_read(master,buffer,sizeof(buffer),200);
            if (!access("/usr/bin/fzf",X_OK)) {
                assert(write(master,"\006",1)==1); terminal_read(master,buffer,sizeof(buffer),1000);
                assert(strstr(buffer,".torrent"));
                assert(write(master,"\r",1)==1); terminal_read(master,buffer,sizeof(buffer),400);
            }
            assert(write(master,"\t\n",2)==2); terminal_read(master,buffer,sizeof(buffer),700);
        }
        assert(strstr(buffer,"payload.bin"));
        assert(write(master,"q",1)==1); terminal_read(master,buffer,sizeof(buffer),250);
        int status; assert(waitpid(pid,&status,0)==pid && WIFEXITED(status) && WEXITSTATUS(status)==0); close(master);
        char error[1024]={0}; Client *client=client_open(state,downloads,false,error,sizeof(error)); assert(client && client->pid>0);
        Torrent *items=NULL; int count=0; assert(client_snapshot(client,&items,&count,error,sizeof(error)) && count==1 && !strcmp(items[0].name,"payload.bin"));
        assert(client_action(client,items[0].hash,BT_REMOVE,error,sizeof(error))); free(items);
        assert(client_stop(client,error,sizeof(error))); client_close(client); if (worker_pidfd>=0) close(worker_pidfd); worker_pidfd=-1;
        char path[256]; snprintf(path,sizeof(path),"%s/lock",state); assert(!unlink(path));
        snprintf(path,sizeof(path),"%s/worker.log",state); assert(!unlink(path));
        snprintf(path,sizeof(path),"%s/config",config); assert(!unlink(path)); assert(!unlink(source)); assert(!rmdir(config)); assert(!rmdir(downloads));
        for (int i=0;i<100;i++) { if (!rmdir(state)) break; struct timespec wait={0,10000000}; nanosleep(&wait,NULL); if (i==99) assert(false); } return;
    }
    assert(write(master,"l",1)==1); terminal_read(master,buffer,sizeof(buffer),150);
    assert(strstr(buffer,"ПОТОК") && strstr(buffer,"БИБЛИОТЕКА") && strstr(buffer,"русский"));
    const char lang_click[]="\033[<0;90;40M\033[<0;90;40m"; assert(write(master,lang_click,sizeof(lang_click)-1)==sizeof(lang_click)-1);
    terminal_read(master,buffer,sizeof(buffer),150); assert(strstr(buffer,"Language: English"));
    /* Real SGR mouse input through ncurses, not a direct call to the handler. */
    const char click[]="\033[<0;26;40M\033[<0;26;40m"; assert(write(master,click,sizeof(click)-1)==sizeof(click)-1);
    terminal_read(master,buffer,sizeof(buffer),150); assert(strstr(buffer,"ADD TORRENT"));
    const char paste[]="\033[200~magnet:?dn=abcdef_abc_abc_abc_abc_abc_abc_abc_abc_abc_abc_abc_abc\n\033[201~";
    assert(write(master,paste,sizeof(paste)-1)==sizeof(paste)-1); terminal_read(master,buffer,sizeof(buffer),150);
    assert(!strstr(buffer,"No torrents"));
    const char edit[]="\001\004"; assert(write(master,edit,sizeof(edit)-1)==sizeof(edit)-1); terminal_read(master,buffer,sizeof(buffer),100);
    assert(write(master,"\025",1)==1); terminal_read(master,buffer,sizeof(buffer),100);
    const unsigned char russian[]={0xd1,0x84,0xd0,0xb0,0xd0,0xb9,0xd0,0xbb};
    for (size_t i=0;i<sizeof(russian);i++) { assert(write(master,russian+i,1)==1); struct timespec wait={0,5000000}; nanosleep(&wait,NULL); }
    terminal_read(master,buffer,sizeof(buffer),150); assert(strstr(buffer,"ф") && strstr(buffer,"а") && strstr(buffer,"й") && strstr(buffer,"л"));
    assert(write(master,"\033",1)==1); terminal_read(master,buffer,sizeof(buffer),100);
    assert(write(master,"/fedora\n",8)==8); terminal_read(master,buffer,sizeof(buffer),150);
    assert(strstr(buffer,"Fedora") || strstr(buffer,"fedora"));
    size.ws_row=24; size.ws_col=80; assert(!ioctl(master,TIOCSWINSZ,&size)); kill(pid,SIGWINCH);
    terminal_read(master,buffer,sizeof(buffer),150); assert(strstr(buffer,"Search"));
    assert(write(master,"q",1)==1); terminal_read(master,buffer,sizeof(buffer),150);
    int status; assert(waitpid(pid,&status,0)==pid && WIFEXITED(status) && WEXITSTATUS(status)==0); close(master);
}
static int delete_test(const char *path,const struct stat *st,int type,struct FTW *walk) { (void)st; (void)type; (void)walk; return remove(path); }
static void test_settings_and_files(void) {
    char root[]="/tmp/potok-settings-XXXXXX",path[PATH_MAX+128],error[1024],config[PATH_MAX]; assert(mkdtemp(root));
    snprintf(config,sizeof(config),"%s/config",root); Settings settings;
    assert(settings_load(&settings,config,true,error,sizeof(error)));
    assert(!strcmp(settings.browse,getenv("HOME")));
    snprintf(settings.downloads,sizeof(settings.downloads),"%s/data with spaces",root);
    snprintf(settings.state,sizeof(settings.state),"%s/state",root); snprintf(settings.browse,sizeof(settings.browse),"%s/search",root);
    assert(settings_save(&settings,error,sizeof(error))); Settings loaded;
    assert(settings_load(&loaded,config,false,error,sizeof(error)) && loaded.russian && !strcmp(loaded.downloads,settings.downloads));
    struct stat st; assert(!stat(config,&st) && (st.st_mode&0777)==0700);
    snprintf(path,sizeof(path),"%s/config",config); assert(!stat(path,&st) && (st.st_mode&0777)==0600);
    App a={.settings=loaded,.russian=true}; key(&a,'l'); assert(settings_load(&loaded,config,true,error,sizeof(error)) && !loaded.russian);
    Settings bad=loaded; strcpy(bad.downloads,"/tmp/a\nb"); assert(!settings_save(&bad,error,sizeof(error)));
    assert(settings_load(&loaded,config,true,error,sizeof(error)) && !strcmp(loaded.downloads,settings.downloads));
    assert(!mkdir(settings.browse,0700));
    const char *names[]={"Fedora live.torrent","evil;touch SENTINEL.torrent","русский\nфайл.TORRENT","ignore.txt"};
    for (size_t i=0;i<COUNT(names);i++) { snprintf(path,sizeof(path),"%s/%s",settings.browse,names[i]); FILE *f=fopen(path,"w"); assert(f); fputs("test",f); fclose(f); }
    snprintf(path,sizeof(path),"%s/loop",settings.browse); assert(!symlink(settings.browse,path));
    snprintf(path,sizeof(path),"%s/linked.torrent",settings.browse); assert(!symlink("Fedora live.torrent",path));
    snprintf(path,sizeof(path),"%s/nested",settings.browse); assert(!mkdir(path,0700));
    snprintf(path,sizeof(path),"%s/nested/second.torrent",settings.browse); FILE *nested=fopen(path,"w"); assert(nested); fclose(nested);
    FileList files; assert(files_scan(&files,settings.browse,error,sizeof(error)) && files.count==4 && !files.limited);
    assert(file_matches("Fedora live.torrent","fdlv") && !file_matches("Fedora live.torrent","zzz"));
    a=(App){.demo=true,.settings=loaded}; strcpy(a.downloads,"/tmp"); open_modal(&a,ADD); a.files=files; a.matches=allocate(files.count*sizeof(*a.matches));
    a.modal=FILES; line_set(&a.file_query,"fdlv"); filter_files(&a); assert(a.file_count==1); submit(&a);
    assert(a.modal==ADD && wcsstr(a.input.value,L"Fedora live.torrent"));
    a.modal=FILES; line_set(&a.file_query,"second"); filter_files(&a); assert(a.file_count==1); submit(&a);
    assert(a.modal==ADD && wcsstr(a.input.value,L"nested/second.torrent") && !strcmp(a.settings.browse,settings.browse));
    assert(settings_load(&loaded,config,true,error,sizeof(error)) && !strcmp(loaded.browse,settings.browse));
    assert(cleanup(&a)); assert(!nftw(root,delete_test,16,FTW_DEPTH|FTW_PHYS));
}
static void test_editor(void) {
    char path[PATH_MAX],expected[PATH_MAX];
    assert(absolute_path("~/Downloads",path)); snprintf(expected,sizeof(expected),"%s/Downloads",getenv("HOME")); assert(!strcmp(path,expected));
    assert(absolute_path("./downloads",path) && *path=='/');
    FILE *out=tmpfile(),*in=tmpfile(); SCREEN *screen=newterm("xterm-256color",out,in); assert(screen); set_term(screen); palette();
    App a={.demo=true}; strcpy(a.downloads,"/tmp"); open_modal(&a,ADD);
    char long_text[200]; memset(long_text,'x',sizeof(long_text)-1); long_text[sizeof(long_text)-1]=0; assert(line_set(&a.input,long_text));
    draw_line(&a,&a.input,2,2,30,true,HIT_SOURCE); size_t scroll=a.input.scroll; assert(scroll>0);
    key(&a,KEY_LEFT); draw_line(&a,&a.input,2,2,30,true,HIT_SOURCE); assert(a.input.scroll==scroll);
    key(&a,1); draw_line(&a,&a.input,2,2,30,true,HIT_SOURCE); assert(!a.input.scroll && !a.input.cursor);
    key(&a,21); input_event(&a,PASTE_BEGIN,true);
    const wchar_t *paste=L"magnet:?dn=файл_qa\n"; for (const wchar_t *p=paste;*p;p++) input_event(&a,*p,false);
    input_event(&a,27,false); input_event(&a,PASTE_END,true);
    assert(a.modal==ADD && !a.quit && !a.pasting && !wcscmp(a.input.value,L"magnet:?dn=файл_qa"));
    key(&a,21); const unsigned char *bytes=(const unsigned char *)"файл";
    for (;*bytes;bytes++) input_byte(&a,*bytes);
    assert(!wcscmp(a.input.value,L"файл")); line_set(&a.input,"magnet:?xt=urn:btih:1234");
    key(&a,'\t'); assert(a.field==1); key(&a,21); input_text(&a,L'/'); input_text(&a,L'x'); key(&a,'\t'); assert(a.field==0 && wcsstr(a.input.value,L"magnet:"));
    cleanup(&a); endwin(); delscreen(screen); fclose(out); fclose(in);
}
int main(int argc,char **argv) {
    signal(SIGABRT,failed_test);
    setlocale(LC_ALL,""); setlocale(LC_NUMERIC,"C");
    test_languages(); puts("ok · EN/RU, Unicode, format arguments");
    test_model_and_actions(); puts("ok · filters, delete confirmation, input, limits");
    test_settings_and_files(); puts("ok · private settings, atomic save, fuzzy file search, symlinks");
    test_editor(); puts("ok · stable input viewport, fields, bracketed paste");
    test_ui(argc>1 && !strcmp(argv[1],"--snapshots")); puts("ok · rendering, transparent background, resize, mouse, modals");
    test_terminal(false,false); test_terminal(true,false); test_terminal(true,true); puts("ok · PTY: paste, keyboard, CLI file with spaces/Unicode, background survives TUI exit, clean stop");
    return 0;
}
