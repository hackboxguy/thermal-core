# tools/thermal-telemetry-tool/Makefile
#
# Stage 19 -- the host tool for the CH32V003 host-command channel.
# Self-contained: pure C++17 + POSIX termios, no external deps. The
# root Makefile's `telemetry-tool` target delegates here; this file
# also lets the tool be built standalone on a bench laptop with a
# plain `make`.
#
#   make         -- build ./thermal-telemetry-tool
#   make clean   -- remove the binary

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra

BIN := thermal-telemetry-tool

all: $(BIN)

$(BIN): main.cpp
	$(CXX) $(CXXFLAGS) -o $@ main.cpp

clean:
	rm -f $(BIN)

.PHONY: all clean
