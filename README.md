# NDISFIX

A DOS tool that fixes the startup freeze of **Windows for Workgroups 3.11**
on fast CPUs and in virtual machines, tuned to the computer it runs on.

## The problem

With networking enabled, WfW 3.11 can freeze on a black screen right after
the startup logo. It doesn't happen every time: often a reset or two gets it
going, so it looks like flaky hardware or a VM that needs to "warm up".
Starting Windows with `win /n` (no network) avoids it.

The cause is in Microsoft's network driver, `NDIS.386`. At startup it
measures how fast the CPU runs an empty loop, by timing 1,048,576 loop
passes against a millisecond clock and dividing by the result. On a modern
CPU the loop finishes in under a millisecond, the measured time is 0, the
divide faults, and Windows halts without a message.

You need this fix if you run WfW 3.11 **with networking** on a fast PC or
in a VM (VirtualBox, VMware, QEMU and so on). Plain Windows 3.1, or WfW
without networking, never loads `NDIS.386` and isn't affected.

## Usage

Exit Windows (or press F8 at boot and skip `WIN`), then run NDISFIX from
the DOS prompt:

```
NDISFIX
```

It finds `NDIS.386`, measures the computer, shows what it would do and asks
before changing anything.

```
NDISFIX [/C | /R | /K:n] [/Y] [path\NDIS.386]

  (no option)  measure this computer and set NDIS.386 to suit it
  /C           check only, change nothing
  /R           restore the original timing
  /K:n         use multiplier n instead of measuring (n divides 1000)
  /Y           don't ask before changing the file
```

The default file is `%WINDIR%\SYSTEM\NDIS.386`, or
`C:\WINDOWS\SYSTEM\NDIS.386` when `WINDIR` isn't set. NDISFIX refuses to
change anything while Windows is running.

Before its first change, NDISFIX saves the original driver as
`\BAK\NDISFIX\NDIS.386` on the same drive, with a `README.TXT` explaining
what it is and how to restore it. An existing backup is never overwritten.
If the driver was already patched by another tool, the backup still gets
Microsoft's original code.

### Moving to different hardware

Run NDISFIX again after moving an installation to another computer. On a
faster machine it raises the setting if needed. On a genuine 386, 486 or
early Pentium it puts back Microsoft's original timing, because there the
patched driver still works but makes Windows take several seconds longer
to start.

### Exit codes

| Code | Meaning |
| --- | --- |
| 0 | Done, or no change needed |
| 1 | Check only (`/C`) found a change to make, or the user answered N |
| 2 | Unknown `NDIS.386` build, file not found, or a write error |
| 3 | Bad options, or Windows is running |

## How it works

NDISFIX finds the calibration routine by its instruction sequence rather
than by fixed offsets, and changes nothing unless it finds exactly one copy.

It multiplies the loop count by **k** and divides the routine's "× 1000"
step by k. The loop then runs long enough to measure, and both values
NDIS stores keep their exact meaning: the measured microseconds for
1,048,576 loops, which NDIS's delay routine divides by for waits of 17 µs
to 10 ms, and the loops per microsecond. k is chosen from the numbers that
divide 1000 (1, 2, 4, 5, 8, 10, 20, 25, 40, 50, 100, 125, 200, 250, 500,
1000). NDISFIX times the same kind of loop on the current computer and
picks the smallest k that gives the loop at least 20 ms. k = 1 is
Microsoft's original code.

From k = 8 up, the multiplier fits a shorter instruction, and the bytes
saved hold a check that turns an elapsed time of 0 into 1, so the divide
can never fault, however fast computers get.

Only the first 21 bytes of the 39-byte routine change. The instruction that
holds a relocated address is never moved.

## patcher9x

[patcher9x](https://github.com/JHRobotics/patcher9x) fixes this bug too,
along with several Windows 95/98/Me bugs, and can patch the compressed
`NDIS.38_` on install disks. Its WfW patch uses a fixed 100× multiplier
with the same zero check; the zero check in NDISFIX follows its example.

NDISFIX adds tuning to the machine and restoring the original timing on
slow hardware. It recognises a driver patched by patcher9x, so you can
re-tune or restore one with NDISFIX.

## Building

NDISFIX is a single C file for [OpenWatcom](https://github.com/open-watcom/open-watcom-v2).

From Linux (or Windows), with OpenWatcom set up:

```
make
```

In DOS, after running OpenWatcom's `OWSETENV.BAT`:

```
BUILD
```

Either way it runs:

```
wcl -bt=dos -ms -os -wx -fe=NDISFIX.EXE NDISFIX.C
```

Prebuilt `NDISFIX.EXE` files are attached to the
[releases](../../releases).

## Testing and limitations

Tested on the `NDIS.386` from a WfW 3.11 VirtualBox VM (27,846 bytes, the
size of Microsoft's 1994 and 1995 network updates), which froze at startup
before the fix and boots reliably after it. Patching, re-tuning, restoring,
backups, and files patched by patcher9x were also tested in DOSBox at
various speeds.

Other builds of `NDIS.386`, such as the original 1993 release or other
language editions, haven't been tested. If their calibration routine
differs, NDISFIX reports an unknown build and leaves the file alone.

The "Windows is running" check hasn't been tested inside a real Windows
DOS box; don't rely on it as the only safeguard.

## Notes

NDISFIX contains no Microsoft code. You patch your own copy of
`NDIS.386`; please don't redistribute patched copies.

Running WfW directly on a modern PC through its CSM (legacy BIOS) mode
rather than in a VM? You may also need
[CREGFIX](https://github.com/mintsuki/cregfix), which cleans up CPU
registers that some modern firmware leaves set.

The code was written with Claude (Anthropic) and reviewed and tested by
the author.

## Licence

GNU General Public License, version 3 or later. See [LICENSE](LICENSE).
