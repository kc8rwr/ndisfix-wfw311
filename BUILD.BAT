@ECHO OFF
REM Build NDISFIX.EXE in DOS with OpenWatcom.
REM Run OpenWatcom's OWSETENV.BAT (or set WATCOM, PATH and INCLUDE) first.
wcl -bt=dos -ms -os -wx -fe=NDISFIX.EXE NDISFIX.C
