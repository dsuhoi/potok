# Potok

A compact terminal BitTorrent client with mouse support, animated progress,
English/Russian UI and a background worker that keeps downloading when you
close the interface.

Written in C11, with a C++17 adapter to libtorrent-rasterbar. No external
Transmission service is needed. [Русская документация](README.ru.md).

## Build

```sh
# Arch Linux
sudo pacman -S --needed base-devel ncurses libtorrent-rasterbar boost

# Debian / Ubuntu
sudo apt install build-essential pkg-config libncurses-dev libtorrent-rasterbar-dev

make -j2
./potok
```

Runtime libraries: ncursesw, libtorrent-rasterbar, OpenSSL and the system C/C++
runtime. Boost headers are needed to build. `fzf` is optional; the file picker
has a built-in fallback. Python, CMake, a web browser and a separate torrent
service are not required to build or run Potok. Dependencies are supplied by
the distribution and are not vendored in this repository.

The stripped 0.3.1 executable is approximately 1.04 MiB. On an Intel i5-8365U
with GCC 16.2.1, a clean `make -j2` took 36 seconds; rebuilding only the C UI
took 0.94 seconds. Idle TUI + worker used about 27 MiB of combined RSS
(shared library pages are counted in both processes).
These are single-run local observations, not cross-machine benchmarks.

Linux x86_64 with libtorrent 2.1.2 is tested. The terminal must be at least
80×24. The interface uses the terminal's default background, so your terminal's
transparency remains visible.

Install into `/usr/local`, or stage an installation for a package:

```sh
sudo make install
make DESTDIR="$PWD/stage" PREFIX=/usr install
```

`make` builds the application only. Tests, source archives and portable bundles
are separate targets. `make source` creates `dist/potok-0.3.1.tar.gz`;
`make bundle` creates `dist/potok/` with a copy of libtorrent. The portable bundle
still needs compatible system ncurses, OpenSSL, C++ runtime and glibc.

## Use

| Key | Action |
|---|---|
| `a` | Add a magnet, HTTP(S) `.torrent` URL, or local `.torrent` |
| `f` / `Ctrl-F` in Add | Search `.torrent` files using fzf or the built-in picker |
| `Tab`, `Enter`, `Esc` | Switch fields, submit, cancel |
| `↑ ↓` / `j k` | Select a torrent |
| `p` / `Space` | Pause / resume |
| `← →` | Overview, files, peers, trackers |
| `/` | Filter by name |
| `s` / `t` | Speed limits / slow mode |
| `v` | Verify downloaded data |
| `d` / `D` | Remove torrent / delete torrent and data after confirmation |
| `l` | Switch English / Russian; the setting is saved |
| `q` / `Ctrl-C` | Close TUI; continue downloading |
| `Q` | Stop downloads and quit after confirmation |
| `?` | Help |

The mouse selects rows, tabs and buttons; the wheel scrolls. Source fields
support UTF-8, bracketed paste, cursor editing and paths with spaces.
The picker remembers its last directory. To search elsewhere, type a directory
in the source field before pressing `Ctrl-F`.

```sh
potok --background
potok --status
potok --stop
potok --lang ru
potok --demo
```

Settings are created in `$XDG_CONFIG_HOME/potok/config` or
`~/.config/potok/config`. State is stored in `$XDG_DATA_HOME/potok` or
`~/.local/share/potok`; downloads default to `~/Downloads`.
`--config-dir`, `--state-dir` and `--download-dir` select other directories.
One worker owns each state directory. Use the same profile options for status
and stop commands. No boot-time service is installed.

## Arch / AUR packaging

The [potok-git recipe](packaging/aur/potok-git/PKGBUILD) builds from this
repository and uses system libraries. It does not bundle libtorrent.
`SKIP` is used for the Git source, as usual for VCS packages.

```sh
cd packaging/aur/potok-git
makepkg -si
```

The recipe is prepared for submission; inclusion in AUR requires a maintainer
account and an associated SSH key. This README does not imply that it has
already been published to AUR.

## Checks

```sh
make test
make sanitize
./tests/test --snapshots
```

Tests cover the C UI, keyboard and mouse input through a real PTY, languages,
private settings, file search, and real loopback BitTorrent transfers, including
magnet metadata, pause/resume, restart, file selection, deletion and downloads
with no UI attached. Public swarms are excluded from automatic tests.

Manual public-swarm testing on 2026-10-24 downloaded the full 756 MiB Debian
netinst image and matched its official SHA-256; a separate Debian Edu magnet
received metadata and payload from public peers. Worker restart and terminal
closure were also checked.

Known limits: local peers can bypass global rate limits under libtorrent's
default peer classes; paused speed statistics may take a few seconds to decay.
Torrent creation and tracker editing are not implemented. Private trackers,
macOS and long-running soak tests have not been validated.

## License

[MIT](LICENSE). libtorrent-rasterbar is distributed separately under
[BSD-3-Clause](LICENSE.libtorrent).
