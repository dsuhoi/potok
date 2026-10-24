#include "../engine.cpp"
#include "../service.h"
#include <libtorrent/create_torrent.hpp>
#include <libtorrent/bencode.hpp>
#include <libtorrent/entry.hpp>
#include <cassert>
#include <atomic>
#include <iostream>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/prctl.h>

static void check(bool condition,const char *message) {
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; std::abort(); }
}
static void tick(Bt *b) {
    char error[1024]; int result=bt_poll(b,error,sizeof(error));
    if (result<0) { std::cerr << error << '\n'; std::abort(); }
}
template<typename Fn> static void wait_for(Bt *a,Bt *b,Fn condition) {
    auto deadline=std::chrono::steady_clock::now()+20s;
    do {
        if (a) tick(a);
        if (b) tick(b);
        if (condition()) return;
        std::this_thread::sleep_for(20ms);
    } while (std::chrono::steady_clock::now()<deadline);
    for (auto *engine:{a,b}) if (engine) for (auto const &[key,record]:engine->records) {
        auto s=record.handle.status();
        std::cerr << key << " state=" << s.state << " progress=" << s.progress << " metadata=" << s.has_metadata << " flags=" << static_cast<std::uint64_t>(s.flags)
                  << " peers=" << s.num_peers << " error=" << s.errc.message() << '\n';
    }
    check(false,"timed out");
}
static Torrent row(Bt *b) {
    char error[1024]; Torrent *rows=nullptr; int n=0;
    check(bt_snapshot(b,&rows,&n,error,sizeof(error)),error); check(n==1,"one torrent");
    Torrent result=rows[0]; std::free(rows); return result;
}
static Bt *open_engine(const fs::path &state,const fs::path &data) {
    char error[1024]; Bt *b=bt_open(state.c_str(),data.c_str(),true,error,sizeof(error)); check(b,error); return b;
}
static void close_engine(Bt *b) { char error[1024]; check(bt_close(b,error,sizeof(error)),error); }
static Torrent remote_row(Client *client) {
    char error[1024]; Torrent *rows=nullptr; int count=0;
    check(client_snapshot(client,&rows,&count,error,sizeof(error)),error); check(count==1,"one background torrent");
    Torrent result=rows[0]; std::free(rows); return result;
}
static void test_background(const fs::path &root,Bt *seed,const std::string &payload,pid_t worker) {
    char error[1024]={0}; auto state=(root/"background-state").string(),data=(root/"background-data").string();
    Client *client=client_open(state.c_str(),data.c_str(),false,error,sizeof(error)); check(client,error);
    check(client->pid==worker,"connected to expected background process");
    check(client_limits(client,256,64,false,error,sizeof(error)),error);
    std::string hash=seed->records.begin()->first;
    find(*seed,hash.c_str()).set_upload_limit(128*1024);
    std::string magnet="magnet:?xt=urn:btih:"+hash+"&x.pe=127.0.0.1:"+std::to_string(seed->session->listen_port());
    check(lt::parse_magnet_uri(magnet).peers.size()==1,"loopback peer in magnet");
    auto source=(root/"payload.torrent").string();
    check(client_add(client,source.c_str(),data.c_str(),true,error,sizeof(error)),error);
    check(client_action(client,hash.c_str(),BT_PAUSE,error,sizeof(error)),error);
    wait_for(seed,nullptr,[&] { return remote_row(client).status==0; });
    check(client_file(client,hash.c_str(),0,false,error,sizeof(error)),error);
    wait_for(seed,nullptr,[&] {
        BtDetails details{}; check(client_details(client,hash.c_str(),1,&details,error,sizeof(error)),error);
        bool skipped=details.file_count==1 && !details.files[0].wanted; bt_details_free(&details); return skipped;
    });
    check(client_file(client,hash.c_str(),0,true,error,sizeof(error)),error);
    check(client_action(client,hash.c_str(),BT_RESUME,error,sizeof(error)),error);
    wait_for(seed,nullptr,[&] { return remote_row(client).status!=0; });
    check(client_action(client,hash.c_str(),BT_REMOVE,error,sizeof(error)),error);
    std::this_thread::sleep_for(200ms);
    check(client_add(client,magnet.c_str(),data.c_str(),false,error,sizeof(error)),error);
    wait_for(seed,nullptr,[&] { check(client_poll(client,error,sizeof(error))>=0,error); return remote_row(client).metadata==1; });
    wait_for(seed,nullptr,[&] { auto t=remote_row(client); return t.done>0 && t.done<1; });
    pid_t pid=client->pid;
    client_close(client); client=nullptr;
    bool complete=false; auto until=std::chrono::steady_clock::now()+40s;
    while (std::chrono::steady_clock::now()<until && !complete) {
        tick(seed); std::error_code ec;
        auto path=root/"background-data"/"payload.bin";
        if (fs::file_size(path,ec)==payload.size() && !ec) { std::ifstream f(path,std::ios::binary); std::string bytes(payload.size(),'\0'); f.read(bytes.data(),static_cast<std::streamsize>(bytes.size())); complete=f.gcount()==static_cast<std::streamsize>(bytes.size()) && bytes==payload; }
        if (!complete) std::this_thread::sleep_for(50ms);
    }
    if (!complete) {
        Client *probe=client_open(state.c_str(),data.c_str(),false,error,sizeof(error));
        if (probe) { auto t=remote_row(probe); std::cerr << "background timeout: done=" << t.done << " peers=" << t.peers << " state=" << t.status << " error=" << t.error << '\n'; client_close(probe); }
        auto s=seed->records.begin()->second.handle.status(); std::cerr << "seed peers=" << s.num_peers << " rate=" << s.upload_payload_rate << " uploaded=" << s.all_time_upload << '\n';
    }
    check(complete,"entire payload arrived with no UI connection");
    client=client_open(state.c_str(),data.c_str(),false,error,sizeof(error)); check(client,error);
    check(client->pid==pid,"same background process after reconnect");
    check(client_limits(client,0,0,false,error,sizeof(error)),error);
    wait_for(seed,nullptr,[&] { return remote_row(client).done==1; });
    auto actual=read_file(root/"background-data"/"payload.bin"); check(std::string(actual.begin(),actual.end())==payload,"background payload matches");
    check(client_stop(client,error,sizeof(error)),error); client_close(client);
    int status; check(waitpid(worker,&status,0)==worker && WIFEXITED(status) && WEXITSTATUS(status)==0,"background stopped cleanly");
    check(fs::exists(root/"background-state"/(hash+".resume")),"background saved resume data");
    check(!client_open(state.c_str(),data.c_str(),false,error,sizeof(error)),"stop does not restart worker");
    std::cout << "ok · background P2P continues after disconnect, reconnect, controls, checkpoint and stop\n";
}
static void test_http(const fs::path &root,const std::vector<char> &metadata) {
    int sock=socket(AF_INET,SOCK_STREAM,0); check(sock>=0,"HTTP socket");
    struct sockaddr_in addr{}; addr.sin_family=AF_INET; addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    check(!bind(sock,reinterpret_cast<sockaddr *>(&addr),sizeof(addr)),"bind"); check(!listen(sock,8),"listen");
    socklen_t size=sizeof(addr); check(!getsockname(sock,reinterpret_cast<sockaddr *>(&addr),&size),"HTTP port");
    std::atomic<bool> stop=false;
    std::thread server([&] {
        while (!stop) {
            int client=accept(sock,nullptr,nullptr); if (client<0) return;
            std::string request; char data[4096];
            while (request.find("\r\n\r\n")==std::string::npos && request.size()<16384) {
                ssize_t n=read(client,data,sizeof(data)); if (n<=0) break; request.append(data,static_cast<size_t>(n));
            }
            bool redirect=request.find("GET /redirect ")!=std::string::npos;
            std::string response=redirect ? "HTTP/1.1 302 Found\r\nLocation: /torrent\r\nContent-Length: 0\r\nConnection: close\r\n\r\n" :
                "HTTP/1.1 200 OK\r\nContent-Length: "+std::to_string(metadata.size())+"\r\nConnection: close\r\n\r\n"+std::string(metadata.begin(),metadata.end());
            size_t written=0;
            while (written<response.size()) {
                ssize_t n=send(client,response.data()+written,response.size()-written,MSG_NOSIGNAL); if (n<=0) break; written+=static_cast<size_t>(n);
            }
            ::close(client);
        }
    });
    char error[1024]; Bt *b=open_engine(root/"http-state",root/"http-data");
    std::string url="http://127.0.0.1:"+std::to_string(ntohs(addr.sin_port))+"/redirect";
    check(bt_add(b,url.c_str(),"",true,error,sizeof(error)),error);
    check(bt_busy(b),"URL import asynchronous");
    wait_for(b,nullptr,[&] { return !bt_busy(b); });
    check(row(b).status==0,"URL imports paused"); close_engine(b);
    stop=true; shutdown(sock,SHUT_RDWR); ::close(sock); server.join();
    check(parse_url("https://[::1]:8443/torrent?pass=1").host=="::1","IPv6 URL");
    for (auto const &bad:{"file:///etc/passwd","https://user:pass@example.com/file","https://x/\r\nBad: header","http://x:99999/a","http://x:/a"}) {
        bool rejected=false; try { parse_url(bad); } catch (...) { rejected=true; } check(rejected,"reject invalid URL");
    }
    std::cout << "ok · asynchronous HTTP import, redirect, URL validation\n";
}
int main() {
    char temp[]="/tmp/potok-engine-XXXXXX"; check(mkdtemp(temp),"test directory"); fs::path root=temp;
    fs::create_directories(root/"seed-data");
    std::string payload(2*1024*1024,'\0');
    for (size_t i=0;i<payload.size();i++) payload[i]=static_cast<char>((i*17+i/251)%256);
    { std::ofstream f(root/"seed-data"/"payload.bin",std::ios::binary); f.write(payload.data(),static_cast<std::streamsize>(payload.size())); check(f.good(),"fixture"); }
#if LIBTORRENT_VERSION_NUM >= 20100
    std::vector<lt::create_file_entry> files; files.emplace_back("payload.bin",static_cast<std::int64_t>(payload.size()));
#else
    lt::file_storage files; files.add_file("payload.bin",static_cast<std::int64_t>(payload.size()));
#endif
    lt::create_torrent torrent(files,16*1024,lt::create_torrent::v1_only);
    lt::set_piece_hashes(torrent,(root/"seed-data").string());
    auto metadata=lt::bencode(torrent.generate());
    fs::path metainfo=root/"payload.torrent"; atomic_write(metainfo,metadata.data(),metadata.size());
    auto background_state=(root/"background-state").string(),background_data=(root/"background-data").string();
    pid_t worker=fork(); check(worker>=0,"background fork");
    if (!worker) { prctl(PR_SET_PDEATHSIG,SIGTERM); _exit(service_run(background_state.c_str(),background_data.c_str(),true)); }
    char error[1024];
    Bt *seed=open_engine(root/"seed-state",root/"seed-data");
    Bt *down=open_engine(root/"down-state",root/"down-data");
    check(!bt_open((root/"down-state").c_str(),(root/"down-data").c_str(),true,error,sizeof(error)),"exclusive state lock");
    check(bt_add(seed,metainfo.c_str(),"",false,error,sizeof(error)),error);
    wait_for(seed,down,[&] { return row(seed).done==1; });
    std::string magnet="magnet:?xt=urn:btih:"+seed->records.begin()->first;
    check(bt_add(down,magnet.c_str(),"",false,error,sizeof(error)),error);
    std::string hash=down->records.begin()->first;
    auto downloader=find(*down,hash.c_str());
    auto port=seed->session->listen_port(); check(port>0,"seeder listening");
    check(bt_limits(down,256,64,false,error,sizeof(error)),error);
    downloader.connect_peer(lt::tcp::endpoint(asio::ip::make_address("127.0.0.1"),port));
    wait_for(seed,down,[&] { return row(down).metadata==1; });
    std::cout << "ok · real BitTorrent handshake and magnet metadata exchange\n";
    check(bt_action(down,hash.c_str(),BT_PAUSE,error,sizeof(error)),error);
    wait_for(seed,down,[&] { return row(down).status==0; });
    check(bt_file(down,hash.c_str(),0,false,error,sizeof(error)),error);
    wait_for(seed,down,[&] {
        BtDetails d{}; check(bt_details(down,hash.c_str(),1,&d,error,sizeof(error)),error);
        bool skipped=d.file_count==1 && !d.files[0].wanted; bt_details_free(&d); return skipped;
    });
    check(bt_file(down,hash.c_str(),0,true,error,sizeof(error)),error);
    check(!bt_file(down,hash.c_str(),999,true,error,sizeof(error)),"file bounds");
    check(!bt_limits(down,INT_MAX,0,false,error,sizeof(error)),"rate overflow");
    check(bt_limits(down,256,64,true,error,sizeof(error)),error);
    int d,u; bool slow; bt_get_limits(down,&d,&u,&slow); check(d==256 && u==64 && slow,"limits");
    close_engine(down);
    down=open_engine(root/"down-state",root/"down-data");
    check(row(down).status==0,"pause persisted"); bt_get_limits(down,&d,&u,&slow); check(d==256 && u==64 && slow,"limits persisted");
    check(!bt_add(down,metainfo.c_str(),"",false,error,sizeof(error)),"duplicate torrent");
    check(bt_limits(down,0,0,false,error,sizeof(error)),error);
    check(bt_action(down,hash.c_str(),BT_RESUME,error,sizeof(error)),error);
    downloader=find(*down,hash.c_str()); downloader.connect_peer(lt::tcp::endpoint(asio::ip::make_address("127.0.0.1"),port));
    wait_for(seed,down,[&] { return row(down).done==1; });
    auto actual=read_file(root/"down-data"/"payload.bin"); check(std::string(actual.begin(),actual.end())==payload,"downloaded bytes match");
    std::cout << "ok · pause/resume, file selection, persistence, complete peer transfer\n";
    check(bt_action(down,hash.c_str(),BT_VERIFY,error,sizeof(error)),error);
    wait_for(seed,down,[&] { auto t=row(down); return t.done==1 && t.status==6; });
    check(bt_action(down,hash.c_str(),BT_REMOVE,error,sizeof(error)),error);
    check(fs::exists(root/"down-data"/"payload.bin"),"remove keeps payload");
    close_engine(down); down=open_engine(root/"down-state",root/"down-data"); check(down->records.empty(),"removed torrent stays removed");
    check(bt_add(down,metainfo.c_str(),"",true,error,sizeof(error)),error);
    check(bt_action(down,down->records.begin()->first.c_str(),BT_DELETE,error,sizeof(error)),error);
    wait_for(seed,down,[&] { return !fs::exists(root/"down-data"/"payload.bin"); });
    close_engine(down);
    test_background(root,seed,payload,worker); close_engine(seed);
    std::cout << "ok · verify, remove/keep, delete payload\n";
    fs::path fifo=root/"fifo"; check(!mkfifo(fifo.c_str(),0600),"fifo");
    bool rejected=false; try { read_file(fifo); } catch (...) { rejected=true; } check(rejected,"nonblocking regular-file validation");
    fs::path unsafe=root/"unsafe"; fs::create_directory(unsafe); ::chmod(unsafe.c_str(),0777);
    check(!bt_open(unsafe.c_str(),(root/"unused-data").c_str(),true,error,sizeof(error)),"private state directory required");
    std::cout << "ok · state permissions, duplicate lock, file validation\n";
    test_http(root,metadata);
    fs::remove_all(root); return 0;
}
