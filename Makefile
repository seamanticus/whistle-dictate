# Builds whistle-dictate as a static PIE with the Whistle model embedded.
#
# VENDOR must hold the upstream artifacts:
#   needle.h, libneedle.a  Cactus-Compute/needle3, linux-x86_64/
#   whistle.cact           Cactus-Compute/whistle
# libneedle.a is built against libc++, so the program is too.

ifeq ($(origin CXX),default)
CXX := clang++
endif
VENDOR ?= vendor
PREFIX ?= /usr
CXXFLAGS ?= -O2

override CXXFLAGS += -std=c++26 -stdlib=libc++ -fPIE -Wall -Wextra -Wpedantic -Wconversion \
	-isystem $(VENDOR) --embed-dir=$(VENDOR)
override LDFLAGS += -static-pie -fuse-ld=lld -stdlib=libc++ -Wl,--gc-sections
# libc++abi carries the C++ ABI that libc++.a leaves out; the clang driver adds
# libgcc's unwinder itself for static links.
LDLIBS = $(VENDOR)/libneedle.a -lc++abi

whistle-dictate: dictate.o
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

dictate.o: dictate.cpp boundary.hpp $(VENDOR)/needle.h $(VENDOR)/whistle.cact
	$(CXX) $(CXXFLAGS) -c -o $@ $<

# Runs the compile-time tests in boundary.hpp; needs no upstream artifacts.
check:
	printf '#include "boundary.hpp"\n' | $(CXX) $(CXXFLAGS) -I. -fsyntax-only -x c++ -

install: whistle-dictate
	install -Dm755 whistle-dictate $(DESTDIR)$(PREFIX)/bin/whistle-dictate

clean:
	rm -f whistle-dictate dictate.o

.PHONY: check install clean
