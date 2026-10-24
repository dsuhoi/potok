CC ?= cc
CXX ?= c++
PKG_CONFIG ?= pkg-config
PREFIX ?= /usr/local
VERSION = 0.3.3
CPPFLAGS += $(shell $(PKG_CONFIG) --cflags ncursesw)
CFLAGS ?= -Os
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic
CXXFLAGS ?= -Os
CXXFLAGS += -std=c++17 -Wall -Wextra -Wpedantic -pthread

ifneq ($(wildcard .deps/usr/lib/pkgconfig/libtorrent-rasterbar.pc),)
TORRENT_PC = PKG_CONFIG_PATH=$(CURDIR)/.deps/usr/lib/pkgconfig $(PKG_CONFIG) --define-variable=prefix=$(CURDIR)/.deps/usr
TORRENT_INCLUDES = -I$(CURDIR)/.deps/usr/include
RPATH_FLAGS = -Wl,-rpath,'$$ORIGIN/.deps/usr/lib:$$ORIGIN/../.deps/usr/lib'
else
TORRENT_PC = $(PKG_CONFIG)
endif
TORRENT_FLAGS = $(shell $(TORRENT_PC) --cflags libtorrent-rasterbar) $(TORRENT_INCLUDES)
LDLIBS += $(shell $(PKG_CONFIG) --libs ncursesw) $(shell $(TORRENT_PC) --libs libtorrent-rasterbar) -lm -pthread
OBJECTS = potok.o engine.o service.o settings.o picker.o
SOURCE_FILES = Makefile README.md README.ru.md LICENSE LICENSE.libtorrent potok.1 \
	potok.c engine.cpp engine.h lang.h service.c service.h settings.c settings.h \
	picker.c picker.h tests/test.c tests/engine_test.cpp tests/swarm_probe.c

.PHONY: all clean test sanitize bundle install source
all: potok
potok: $(OBJECTS)
	$(CXX) $^ $(LDFLAGS) $(RPATH_FLAGS) $(LDLIBS) -s -o $@
potok.o: potok.c engine.h service.h settings.h picker.h lang.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
engine.o: engine.cpp engine.h
	$(CXX) $(TORRENT_FLAGS) $(CXXFLAGS) -c $< -o $@
service.o: service.c service.h settings.h engine.h
settings.o: settings.c settings.h
picker.o: picker.c picker.h
tests/test.o: tests/test.c potok.c engine.h service.h settings.h picker.h lang.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
tests/test: tests/test.o engine.o service.o settings.o picker.o
	$(CXX) $^ $(LDFLAGS) $(RPATH_FLAGS) $(LDLIBS) -o $@
tests/swarm_probe.o: tests/swarm_probe.c service.h engine.h
tests/swarm_probe: tests/swarm_probe.o engine.o service.o settings.o
	$(CXX) $^ $(LDFLAGS) $(RPATH_FLAGS) $(LDLIBS) -o $@
tests/engine_test: tests/engine_test.cpp engine.cpp engine.h service.h service.o settings.o
	$(CXX) $(TORRENT_FLAGS) $(CXXFLAGS) -O0 $< service.o settings.o $(LDFLAGS) $(RPATH_FLAGS) $(LDLIBS) -o $@
test: potok tests/test tests/engine_test
	./tests/test
	./tests/engine_test
tests/engine-san.o: engine.cpp engine.h
	$(CXX) $(TORRENT_FLAGS) -std=c++17 -g -O1 -pthread -fsanitize=address,undefined -fno-omit-frame-pointer -c engine.cpp -o tests/engine-san.o
tests/test-san.o: tests/test.c potok.c engine.h service.h settings.h picker.h lang.h
	$(CC) $(CPPFLAGS) -std=c11 -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -c tests/test.c -o tests/test-san.o
tests/%-san.o: %.c service.h settings.h picker.h engine.h
	$(CC) $(CPPFLAGS) -std=c11 -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -c $< -o $@
tests/test-sanitize: tests/test-san.o tests/engine-san.o tests/service-san.o tests/settings-san.o tests/picker-san.o
	$(CXX) $^ $(LDFLAGS) $(RPATH_FLAGS) $(LDLIBS) -fsanitize=address,undefined -o $@
tests/engine-sanitize: tests/engine_test.cpp engine.cpp engine.h service.h tests/service-san.o tests/settings-san.o
	$(CXX) $(TORRENT_FLAGS) -std=c++17 -g -O1 -pthread -fsanitize=address,undefined -fno-omit-frame-pointer tests/engine_test.cpp tests/service-san.o tests/settings-san.o $(LDFLAGS) $(RPATH_FLAGS) $(LDLIBS) -o $@
sanitize: potok tests/test-sanitize tests/engine-sanitize
	ASAN_OPTIONS=detect_leaks=1 ./tests/test-sanitize
	ASAN_OPTIONS=detect_leaks=1 ./tests/engine-sanitize
bundle: potok
	mkdir -p dist/potok/lib
	cp README.md README.ru.md LICENSE LICENSE.libtorrent potok.1 dist/potok/
	$(CXX) $(OBJECTS) $(LDFLAGS) -Wl,-rpath,'$$ORIGIN/lib' $(LDLIBS) -s -o dist/potok/potok
	cp -a $(shell $(TORRENT_PC) --variable=libdir libtorrent-rasterbar)/libtorrent-rasterbar.so* dist/potok/lib/
	strip dist/potok/potok
install: potok
	install -Dm755 potok "$(DESTDIR)$(PREFIX)/bin/potok"
	install -Dm644 LICENSE "$(DESTDIR)$(PREFIX)/share/licenses/potok/LICENSE"
	install -Dm644 README.md "$(DESTDIR)$(PREFIX)/share/doc/potok/README.md"
	install -Dm644 README.ru.md "$(DESTDIR)$(PREFIX)/share/doc/potok/README.ru.md"
	install -Dm644 potok.1 "$(DESTDIR)$(PREFIX)/share/man/man1/potok.1"
source:
	mkdir -p dist
	tar -czf dist/potok-$(VERSION).tar.gz --transform 's,^,potok-$(VERSION)/,' $(SOURCE_FILES)
clean:
	rm -f potok *.o tests/*.o tests/test tests/engine_test tests/test-sanitize tests/engine-sanitize tests/swarm_probe
