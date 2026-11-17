# Build NDISFIX.EXE with OpenWatcom (cross-compiling from Linux or Windows).
# Needs OpenWatcom's wcl on the PATH and WATCOM set; INCLUDE defaults to
# $(WATCOM)/h. In DOS itself, use BUILD.BAT instead.

INCLUDE ?= $(WATCOM)/h
export INCLUDE

NDISFIX.EXE: NDISFIX.C
	wcl -bt=dos -ms -os -wx -fe=NDISFIX.EXE NDISFIX.C

clean:
	rm -f NDISFIX.EXE NDISFIX.o NDISFIX.obj *.err

.PHONY: clean
