# Builds whistle-dictate as a static PIE with the Whistle model embedded.

ifeq ($(origin CXX),default)
CXX := clang++
endif
CXXFLAGS ?= -O2

override CXXFLAGS += -std=c++26 -stdlib=libc++ -fPIE -Wall -Wextra -Wpedantic -Wconversion

# Runs the compile-time tests in boundary.hpp; needs no upstream artifacts.
check:
	printf '#include "boundary.hpp"\n' | $(CXX) $(CXXFLAGS) -I. -fsyntax-only -x c++ -

.PHONY: check
