#include "engine.h"
#include <libtorrent/session.hpp>
#include <libtorrent/session_params.hpp>
#include <libtorrent/add_torrent_params.hpp>
#include <libtorrent/alert_types.hpp>
#include <libtorrent/load_torrent.hpp>
#include <libtorrent/magnet_uri.hpp>
#include <libtorrent/torrent_status.hpp>
#include <libtorrent/peer_info.hpp>
#include <libtorrent/read_resume_data.hpp>
#include <libtorrent/write_resume_data.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>
#include <algorithm>
#include <chrono>
#include <climits>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace lt = libtorrent;
namespace fs = std::filesystem;
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using namespace std::chrono_literals;
static constexpr size_t max_metainfo = 4 * 1024 * 1024;

struct Record { lt::torrent_handle handle; bool saving = false, dirty = false; };
struct Bt {
    std::unique_ptr<lt::session> session;
    std::map<std::string, Record> records;
    fs::path state, downloads;
    int lock = -1, down = 0, up = 0;
    bool slow = false;
    std::future<lt::add_torrent_params> import;
    std::chrono::steady_clock::time_point checkpoint = std::chrono::steady_clock::now();
    ~Bt() { session.reset(); if (lock >= 0) ::close(lock); }
};

static void fail(const std::string &message) { throw std::runtime_error(message); }
static bool report(char *out, size_t cap, const std::exception &e) {
    if (cap) std::snprintf(out, cap, "%s", e.what());
    return false;
}
template<size_t N> static void copy(char (&out)[N], const std::string &s) { std::snprintf(out, N, "%s", s.c_str()); }
static std::string hex_hash(const char *data,size_t length) {
    std::string out; out.reserve(length * 2);
    for (size_t i = 0; i < length; i++) { auto c = static_cast<unsigned char>(data[i]); out += "0123456789abcdef"[c >> 4]; out += "0123456789abcdef"[c & 15]; }
    return out;
}
static std::string id(const lt::info_hash_t &hash) { return hash.has_v1() ? hex_hash(hash.v1.data(),20) : hex_hash(hash.v2.data(),32); }
static bool matches_id(const std::string &key,const lt::info_hash_t &hash) {
    return (hash.has_v1() && key==hex_hash(hash.v1.data(),20)) || (hash.has_v2() && key==hex_hash(hash.v2.data(),32));
}
static bool valid_id(const std::string &s) {
    return (s.size() == 40 || s.size() == 64) && std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isxdigit(c); });
}
static std::vector<char> read_file(const fs::path &path) {
    int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW);
    if (fd < 0) fail(path.string() + ": " + std::strerror(errno));
    struct stat st{};
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0 || st.st_size > static_cast<off_t>(max_metainfo)) {
        ::close(fd); fail("Expected a regular file, 1 byte–4 MiB");
    }
    std::vector<char> data(static_cast<size_t>(st.st_size)); size_t n = 0;
    while (n < data.size()) {
        ssize_t got = ::read(fd, data.data() + n, data.size() - n);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) { ::close(fd); fail("Could not read " + path.string()); }
        n += static_cast<size_t>(got);
    }
    ::close(fd); return data;
}
static void atomic_write(const fs::path &path, const char *data, size_t n) {
    std::string temp = path.string() + ".tmp.XXXXXX";
    int fd = mkstemp(temp.data());
    if (fd < 0) fail("Cannot save " + path.string() + ": " + std::strerror(errno));
    bool ok = true; size_t written = 0;
    while (written < n) {
        ssize_t count = ::write(fd, data + written, n - written);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { ok = false; break; }
        written += static_cast<size_t>(count);
    }
    if (ok && fsync(fd)) ok = false;
    if (::close(fd)) ok = false;
    if (ok && ::rename(temp.c_str(), path.c_str())) ok = false;
    if (!ok) { int saved = errno; ::unlink(temp.c_str()); fail("Cannot save " + path.string() + ": " + std::strerror(saved)); }
    int parent = ::open(path.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (parent < 0) fail("Cannot sync " + path.parent_path().string());
    int result = fsync(parent); ::close(parent);
    if (result) fail("Cannot sync " + path.parent_path().string());
}
static void save(Bt &b, const std::string &key, const lt::add_torrent_params &params) {
    auto data = lt::write_resume_data_buf(params);
    if (data.size() > max_metainfo) fail("Resume data exceeds 4 MiB");
    atomic_write(b.state / (key + ".resume"), data.data(), data.size());
}
static lt::torrent_handle find(Bt &b, const char *hash) {
    auto it = b.records.find(hash);
    if (it == b.records.end() || !it->second.handle.is_valid()) fail("Torrent not found");
    return it->second.handle;
}
static void apply_limits(Bt &b) {
    lt::settings_pack settings;
    settings.set_int(lt::settings_pack::download_rate_limit, (b.slow ? std::min(b.down ? b.down : 128, 128) : b.down) * 1024);
    settings.set_int(lt::settings_pack::upload_rate_limit, (b.slow ? std::min(b.up ? b.up : 64, 64) : b.up) * 1024);
    b.session->apply_settings(settings);
}
static void request_save(Record &record) {
    if (!record.handle.is_valid()) return;
    if (record.saving) { record.dirty=true; return; }
    record.handle.save_resume_data(lt::torrent_handle::save_info_dict | lt::torrent_handle::flush_disk_cache);
    record.saving = true; record.dirty=false;
}
static void insert(Bt &b, lt::add_torrent_params params, bool persist, const std::string &restored_key={}) {
    auto hashes = params.ti ? params.ti->info_hashes() : params.info_hashes;
    std::string key = restored_key.empty()?id(hashes):restored_key;
    for (auto const &[name, record] : b.records) {
        auto other = record.handle.info_hashes();
        if ((hashes.has_v1() && other.has_v1() && hashes.v1 == other.v1) ||
            (hashes.has_v2() && other.has_v2() && hashes.v2 == other.v2)) fail("Already in the queue");
    }
    params.flags |= lt::torrent_flags::duplicate_is_error;
    auto handle = b.session->add_torrent(params);
    try { if (persist) save(b, key, params); }
    catch (...) { b.session->remove_torrent(handle); throw; }
    b.records.emplace(key, Record{handle});
}

struct Url { std::string host, port, target, authority; bool tls; };
static Url parse_url(const std::string &url) {
    Url u{}; size_t pos;
    if (url.rfind("https://", 0) == 0) { u.tls = true; pos = 8; }
    else if (url.rfind("http://", 0) == 0) pos = 7;
    else fail("Expected an HTTP(S) URL");
    if (url.size() > 16384 || std::any_of(url.begin(), url.end(), [](unsigned char c) { return c <= 32 || c == 127; })) fail("Invalid URL");
    size_t end = url.find_first_of("/?#", pos);
    u.authority = url.substr(pos, end == std::string::npos ? end : end - pos);
    if (u.authority.empty() || u.authority.find('@') != std::string::npos) fail("Invalid URL host");
    u.port = u.tls ? "443" : "80";
    if (u.authority.front() == '[') {
        size_t bracket = u.authority.find(']');
        if (bracket == std::string::npos) fail("Invalid IPv6 URL");
        u.host = u.authority.substr(1, bracket - 1);
        if (bracket + 1 < u.authority.size()) {
            if (u.authority[bracket + 1] != ':') fail("Invalid URL port");
            u.port = u.authority.substr(bracket + 2);
        }
    } else {
        size_t colon = u.authority.find(':'); u.host = u.authority.substr(0, colon);
        if (colon != std::string::npos) u.port = u.authority.substr(colon + 1);
    }
    if (u.host.empty() || u.port.empty() || u.port.size() > 5 || !std::all_of(u.port.begin(),u.port.end(),[](unsigned char c) { return std::isdigit(c); }) || std::stoul(u.port) > 65535 || std::stoul(u.port) == 0) fail("Invalid URL host or port");
    u.target = end == std::string::npos ? "/" : url.substr(end);
    if (u.target.front() != '/') u.target.insert(0, "/");
    size_t fragment = u.target.find('#'); if (fragment != std::string::npos) u.target.resize(fragment);
    return u;
}
struct HttpResult { unsigned status; std::string body, location; };
template<typename Stream> static HttpResult fetch(Stream &stream, asio::io_context &io, const Url &url) {
    asio::ip::tcp::resolver resolver(io); asio::steady_timer timer(io);
    beast::flat_buffer buffer;
    http::request<http::empty_body> request{http::verb::get, url.target, 11};
    request.set(http::field::host, url.authority); request.set(http::field::user_agent, "Potok/0.2");
    request.set(http::field::connection, "close");
    http::response_parser<http::string_body> parser; parser.body_limit(max_metainfo);
    bool done = false; boost::system::error_code error;
    auto finish = [&](boost::system::error_code ec) {
        if (done) return;
        done = true; error = ec; resolver.cancel(); timer.cancel();
        boost::system::error_code ignored; beast::get_lowest_layer(stream).socket().close(ignored);
    };
    auto send = [&] {
        http::async_write(stream, request, [&](auto ec, size_t) {
            if (ec) return finish(ec);
            http::async_read(stream, buffer, parser, [&](auto result, size_t) { finish(result); });
        });
    };
    timer.expires_after(15s); timer.async_wait([&](auto ec) { if (!ec) finish(asio::error::timed_out); });
    resolver.async_resolve(url.host, url.port, [&](auto ec, auto endpoints) {
        if (done) return;
        if (ec) return finish(ec);
        beast::get_lowest_layer(stream).async_connect(endpoints, [&](auto result, auto) {
            if (done) return;
            if (result) return finish(result);
            if constexpr (std::is_same_v<Stream, beast::ssl_stream<beast::tcp_stream>>)
                stream.async_handshake(asio::ssl::stream_base::client, [&](auto ssl_error) { if (ssl_error) finish(ssl_error); else if (!done) send(); });
            else send();
        });
    });
    io.run();
    if (error) fail("Download: " + error.message());
    auto response = parser.release();
    return {response.result_int(), std::move(response.body()), std::string(response[http::field::location])};
}
static std::vector<char> download(std::string url) {
    for (int redirects = 0; redirects <= 3; redirects++) {
        Url u = parse_url(url); asio::io_context io; HttpResult response;
        if (u.tls) {
            asio::ssl::context ctx(asio::ssl::context::tls_client); ctx.set_default_verify_paths();
            beast::ssl_stream<beast::tcp_stream> stream(io, ctx);
            stream.set_verify_mode(asio::ssl::verify_peer); stream.set_verify_callback(asio::ssl::host_name_verification(u.host));
            if (!SSL_set_tlsext_host_name(stream.native_handle(), u.host.c_str())) fail("Cannot set TLS host");
            response = fetch(stream, io, u);
        } else { beast::tcp_stream stream(io); response = fetch(stream, io, u); }
        if (response.status == 200) return {response.body.begin(), response.body.end()};
        if (response.status == 301 || response.status == 302 || response.status == 303 || response.status == 307 || response.status == 308) {
            std::string next = response.location;
            if (!next.empty() && next.front() == '/') next = (u.tls ? "https://" : "http://") + u.authority + next;
            Url destination = parse_url(next);
            if (u.tls && !destination.tls) fail("Refused an HTTPS to HTTP redirect");
            url = next; continue;
        }
        fail("HTTP " + std::to_string(response.status));
    }
    fail("Too many redirects"); return {};
}
static lt::add_torrent_params prepare(const std::string &source, const std::string &directory, bool paused) {
    lt::add_torrent_params params;
    if (source.rfind("magnet:?", 0) == 0) params = lt::parse_magnet_uri(source);
    else {
        auto data = source.rfind("http://",0) == 0 || source.rfind("https://",0) == 0 ? download(source) : read_file(source);
        params = lt::load_torrent_buffer(lt::span<char const>(data.data(), data.size()));
    }
    params.save_path = directory;
    if (paused) { params.flags &= ~lt::torrent_flags::auto_managed; params.flags |= lt::torrent_flags::paused; }
    else { params.flags |= lt::torrent_flags::auto_managed; params.flags &= ~lt::torrent_flags::paused; }
    return params;
}

extern "C" Bt *bt_open(const char *state, const char *downloads, bool local_only, char *error, size_t cap) {
    try {
        auto b = std::make_unique<Bt>(); b->state = fs::absolute(state); b->downloads = fs::absolute(downloads);
        bool created = fs::create_directories(b->state);
        if (created && ::chmod(b->state.c_str(), 0700)) fail("Cannot protect state directory");
        struct stat st{};
        if (::stat(b->state.c_str(), &st) || st.st_uid != getuid() || (st.st_mode & 0077)) fail("State directory must be owned by you with mode 0700");
        b->lock = ::open((b->state / "lock").c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (b->lock < 0 || flock(b->lock, LOCK_EX | LOCK_NB)) fail("Another Potok instance is using this state directory");
        fs::create_directories(b->downloads);
        lt::settings_pack settings;
        settings.set_str(lt::settings_pack::user_agent, "Potok/0.2");
        settings.set_str(lt::settings_pack::listen_interfaces, local_only ? "127.0.0.1:0" : "0.0.0.0:0,[::]:0");
        settings.set_bool(lt::settings_pack::enable_dht, !local_only);
        settings.set_bool(lt::settings_pack::enable_lsd, !local_only);
        settings.set_bool(lt::settings_pack::enable_upnp, !local_only);
        settings.set_bool(lt::settings_pack::enable_natpmp, !local_only);
        settings.set_bool(lt::settings_pack::announce_to_all_tiers, true);
        settings.set_int(lt::settings_pack::alert_mask, static_cast<int>(static_cast<std::uint32_t>(lt::alert_category::error | lt::alert_category::storage | lt::alert_category::status)));
        b->session = std::make_unique<lt::session>(lt::session_params(settings));
        std::ifstream options(b->state / "limits");
        if (options) {
            int slow; if (!(options >> b->down >> b->up >> slow) || b->down < 0 || b->down > INT_MAX/1024 || b->up < 0 || b->up > INT_MAX/1024 || (slow != 0 && slow != 1)) fail("Invalid saved speed limits");
            b->slow = slow != 0;
        }
        apply_limits(*b);
        for (auto const &file : fs::directory_iterator(b->state)) {
            if (file.path().extension() != ".resume") continue;
            auto name = file.path().stem().string(); if (!valid_id(name)) continue;
            auto data = read_file(file.path());
            auto params = lt::read_resume_data(lt::span<char const>(data.data(), data.size()));
            if (!matches_id(name,params.ti ? params.ti->info_hashes() : params.info_hashes)) fail("Resume hash mismatch: " + name);
            insert(*b, std::move(params), false, name);
        }
        return b.release();
    } catch (const std::exception &e) { report(error, cap, e); return nullptr; }
}
extern "C" bool bt_add(Bt *b, const char *source, const char *directory, bool paused, char *error, size_t cap) {
    try {
        if (b->import.valid()) fail("An import is already running");
        fs::path dir = directory && *directory ? fs::path(directory) : b->downloads;
        if (!dir.is_absolute()) fail("Use an absolute download path");
        fs::create_directories(dir);
        std::string src(source);
        if (src.rfind("~/",0) == 0) {
            const char *home = std::getenv("HOME"); if (!home) fail("HOME is not set");
            src = std::string(home) + src.substr(1);
        }
        if (src.rfind("http://",0) == 0 || src.rfind("https://",0) == 0) {
            b->import = std::async(std::launch::async, [src, dir, paused] { return prepare(src, dir.string(), paused); });
        } else insert(*b, prepare(src, dir.string(), paused), true);
        return true;
    } catch (const std::exception &e) { return report(error, cap, e); }
}
extern "C" bool bt_action(Bt *b, const char *hash, BtAction action, char *error, size_t cap) {
    try {
        auto h = find(*b,hash);
        switch (action) {
            case BT_PAUSE: h.unset_flags(lt::torrent_flags::auto_managed); h.pause(); break;
            case BT_RESUME: h.clear_error(); h.set_flags(lt::torrent_flags::auto_managed); h.resume(); break;
            case BT_VERIFY: h.force_recheck(); break;
            case BT_UP: h.queue_position_up(); break;
            case BT_DOWN: h.queue_position_down(); break;
            case BT_REMOVE: case BT_DELETE:
                if (::unlink((b->state / (std::string(hash) + ".resume")).c_str()) && errno != ENOENT) fail("Cannot remove saved torrent");
                b->session->remove_torrent(h, action == BT_DELETE ? lt::session::delete_files : lt::remove_flags_t{});
                b->records.erase(hash); return true;
        }
        request_save(b->records.at(hash)); return true;
    } catch (const std::exception &e) { return report(error, cap, e); }
}
extern "C" bool bt_file(Bt *b, const char *hash, int index, bool wanted, char *error, size_t cap) {
    try {
        auto h = find(*b,hash); auto ti = h.torrent_file();
        if (!ti || index < 0 || index >= ti->num_files()) fail("File not found");
        h.file_priority(lt::file_index_t(index), wanted ? lt::default_priority : lt::dont_download);
        request_save(b->records.at(hash)); return true;
    } catch (const std::exception &e) { return report(error, cap, e); }
}
extern "C" bool bt_limits(Bt *b, int down, int up, bool slow, char *error, size_t cap) {
    try {
        if (down < 0 || down > INT_MAX/1024 || up < 0 || up > INT_MAX/1024) fail("Speed limit is out of range");
        std::string data = std::to_string(down) + " " + std::to_string(up) + " " + (slow ? "1\n" : "0\n");
        atomic_write(b->state / "limits", data.data(), data.size());
        b->down = down; b->up = up; b->slow = slow; apply_limits(*b); return true;
    } catch (const std::exception &e) { return report(error, cap, e); }
}
extern "C" void bt_get_limits(Bt *b, int *down, int *up, bool *slow) { *down = b->down; *up = b->up; *slow = b->slow; }
extern "C" bool bt_snapshot(Bt *b, Torrent **items, int *count, char *error, size_t cap) {
    try {
        std::vector<Torrent> rows; rows.reserve(b->records.size());
        for (auto const &[key, record] : b->records) {
            auto s = record.handle.status(lt::torrent_handle::query_name | lt::torrent_handle::query_save_path);
            Torrent t{}; copy(t.hash,key); copy(t.name,s.name); copy(t.dir,s.save_path); if (s.errc) copy(t.error,s.errc.message());
            bool paused = bool(s.flags & lt::torrent_flags::paused);
            bool managed = bool(s.flags & lt::torrent_flags::auto_managed);
            t.status = paused && !managed ? 0 : s.state == lt::torrent_status::checking_files || s.state == lt::torrent_status::checking_resume_data ? 2 :
                paused ? (s.is_finished ? 5 : 3) : s.is_finished ? 6 : 4;
            t.peers = s.num_peers; t.queue = std::max(0,static_cast<int>(s.queue_position));
            t.done = t.shown = s.progress; t.size = static_cast<double>(s.total_wanted);
            t.down = s.download_payload_rate; t.up = s.upload_payload_rate;
            t.eta = s.download_payload_rate > 0 ? static_cast<double>(s.total_wanted-s.total_wanted_done)/s.download_payload_rate : s.is_finished ? 0 : -1;
            t.ratio = s.all_time_download > 0 ? static_cast<double>(s.all_time_upload)/s.all_time_download : 0;
            t.metadata = s.has_metadata ? 1 : 0; t.checked = s.progress;
            rows.push_back(t);
        }
        std::stable_sort(rows.begin(), rows.end(), [](auto const &a, auto const &c) { return a.queue < c.queue; });
        *items = static_cast<Torrent *>(std::calloc(std::max<size_t>(1,rows.size()),sizeof(Torrent)));
        if (!*items) throw std::bad_alloc();
        if (!rows.empty()) std::memcpy(*items,rows.data(),rows.size()*sizeof(Torrent));
        *count = static_cast<int>(rows.size()); return true;
    } catch (const std::exception &e) { return report(error, cap, e); }
}
extern "C" void bt_details_free(BtDetails *d) { std::free(d->files); std::free(d->peers); std::free(d->trackers); *d = {}; }
extern "C" bool bt_details(Bt *b, const char *hash, int tab, BtDetails *d, char *error, size_t cap) {
    *d = {};
    try {
        auto h = find(*b,hash);
        if (tab == 1) {
            auto ti = h.torrent_file(); if (!ti) return true;
            auto progress = h.file_progress(); auto priorities = h.get_file_priorities();
            d->file_count = ti->num_files(); d->files = static_cast<BtFile *>(std::calloc(std::max(1,d->file_count),sizeof(BtFile)));
            if (!d->files) throw std::bad_alloc();
            for (int i = 0; i < d->file_count; i++) {
                auto idx = lt::file_index_t(i); auto &f = d->files[i];
#if LIBTORRENT_VERSION_NUM >= 20100
                auto const &layout=ti->layout();
#else
                auto const &layout=ti->files();
#endif
                copy(f.name,layout.file_path(idx));
                f.size = static_cast<double>(layout.file_size(idx)); f.done = static_cast<double>(progress.at(static_cast<size_t>(i)));
                f.wanted = priorities.at(static_cast<size_t>(i)) != lt::dont_download;
            }
        } else if (tab == 2) {
            std::vector<lt::peer_info> peers; h.get_peer_info(peers); d->peer_count = static_cast<int>(peers.size());
            d->peers = static_cast<BtPeer *>(std::calloc(std::max(1,d->peer_count),sizeof(BtPeer))); if (!d->peers) throw std::bad_alloc();
            for (size_t i = 0; i < peers.size(); i++) {
#if LIBTORRENT_VERSION_NUM >= 20100
                copy(d->peers[i].address,peers[i].remote_endpoint().address().to_string());
#else
                copy(d->peers[i].address,peers[i].ip.address().to_string());
#endif
                copy(d->peers[i].client,peers[i].client);
                d->peers[i].down = peers[i].payload_down_speed; d->peers[i].up = peers[i].payload_up_speed;
            }
        } else if (tab == 3) {
            auto trackers = h.trackers(); d->tracker_count = static_cast<int>(trackers.size());
            d->trackers = static_cast<BtTracker *>(std::calloc(std::max(1,d->tracker_count),sizeof(BtTracker))); if (!d->trackers) throw std::bad_alloc();
            for (size_t i = 0; i < trackers.size(); i++) {
                copy(d->trackers[i].url,trackers[i].url);
                for (auto const &ep : trackers[i].endpoints) for (auto const &info : ep.info_hashes)
                    if (info.last_error) copy(d->trackers[i].error,info.last_error.message());
            }
        }
        return true;
    } catch (const std::exception &e) { bt_details_free(d); return report(error,cap,e); }
}
extern "C" bool bt_busy(Bt *b) { return b->import.valid(); }
extern "C" int bt_poll(Bt *b, char *error, size_t cap) {
    try {
        int result = 0;
        if (b->import.valid() && b->import.wait_for(0s) == std::future_status::ready) { insert(*b,b->import.get(),true); result = 1; }
        std::vector<lt::alert *> alerts; b->session->pop_alerts(&alerts);
        std::string failure;
        for (auto *alert : alerts) {
            if (auto *saved = lt::alert_cast<lt::save_resume_data_alert>(alert)) {
                for (auto &[key, record] : b->records) if (record.handle == saved->handle) {
                    record.saving = false;
                    try { save(*b,key,saved->params); } catch (const std::exception &e) { failure=e.what(); }
                    if (record.dirty) request_save(record);
                    break;
                }
            } else if (auto *failed = lt::alert_cast<lt::save_resume_data_failed_alert>(alert)) {
                for (auto &[key, record] : b->records) if (record.handle == failed->handle) record.saving = false;
                failure = failed->message();
            } else if (auto *torrent = lt::alert_cast<lt::torrent_error_alert>(alert)) failure = torrent->error.message();
            else if (auto *deleted = lt::alert_cast<lt::torrent_delete_failed_alert>(alert)) failure = deleted->error.message();
            else if (auto *metadata = lt::alert_cast<lt::metadata_received_alert>(alert)) {
                for (auto &[key, record] : b->records) if (record.handle == metadata->handle) request_save(record);
            }
        }
        auto time = std::chrono::steady_clock::now();
        if (time - b->checkpoint >= 30s) { for (auto &[key, record] : b->records) request_save(record); b->checkpoint = time; }
        if (!failure.empty()) fail(failure);
        return result;
    } catch (const std::exception &e) { report(error,cap,e); return -1; }
}
extern "C" bool bt_close(Bt *ptr, char *error, size_t cap) {
    std::unique_ptr<Bt> b(ptr);
    try {
        if (!b) return true;
        if (b->import.valid()) insert(*b,b->import.get(),true);
        for (auto &[key, record] : b->records) request_save(record);
        auto deadline = std::chrono::steady_clock::now() + 10s;
        for (;;) {
            if (bt_poll(b.get(),error,cap) < 0) return false;
            bool waiting = false; for (auto const &[key, record] : b->records) waiting |= record.saving;
            if (!waiting) return true;
            if (std::chrono::steady_clock::now() >= deadline) fail("Timed out saving torrent state");
            std::this_thread::sleep_for(10ms);
        }
    } catch (const std::exception &e) { return report(error,cap,e); }
}
