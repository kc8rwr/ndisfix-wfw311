/*
 * NDISFIX.C - fix the divide-by-zero startup freeze in the Windows for
 * Workgroups 3.11 NDIS.386 driver on fast CPUs and virtual machines,
 * tuned to the machine it runs on.
 *
 * Copyright (C) 2026 Leif Burrow
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * The problem
 * -----------
 * During startup NDIS.386 calibrates its delay loops (39 bytes):
 *
 *     +0  B9 00001000h    mov  ecx, 100000h    ; 1,048,576 loops
 *     +5  90              nop
 *     +6  E2 FE           loop $
 *     +8  CD 20 3F000100  VxDCall Get_System_Time
 *     +14 2B C2           sub  eax, edx        ; elapsed milliseconds
 *     +16 B9 000003E8h    mov  ecx, 1000
 *     +21 F7 E1           mul  ecx             ; elapsed microseconds
 *     +23 A3 xxxxxxxx     mov  [elapsed_us], eax   (address has a fixup)
 *     +28 8B C8           mov  ecx, eax
 *     +30 B8 00001000h    mov  eax, 100000h
 *     +35 2B D2           sub  edx, edx
 *     +37 F7 F1           div  ecx             ; loops per microsecond
 *
 * On a modern CPU the loop can finish within one millisecond, the
 * divide faults, and Windows halts on a black screen after the logo.
 *
 * [elapsed_us] must keep meaning "microseconds for 100000h loops":
 * NDIS's delay routine divides by it for waits of 17 us to 10 ms.
 *
 * The fix
 * -------
 * The loop count is multiplied by k and the 1000 divided by k, where k
 * divides 1000 evenly. [elapsed_us] and the loops-per-microsecond
 * result keep their exact meaning; only the measuring window is longer.
 * NDISFIX times the same kind of loop on this machine and picks the
 * smallest k giving the loop at least TARGET_MS milliseconds. A machine
 * that does not need the fix gets k = 1, the original code.
 *
 * For k >= 8 the multiplier fits a shorter instruction (push imm8 /
 * pop ecx), and the bytes saved, plus the nop, hold a guard that turns
 * an elapsed time of 0 into 1, so the divide can never fault:
 *
 *     +0  B9 nnnnnnnn     mov  ecx, 100000h * k
 *     +5  E2 FE           loop $
 *     +7  CD 20 3F000100  VxDCall Get_System_Time
 *     +13 2B C2           sub  eax, edx
 *     +15 75 01           jnz  +1
 *     +17 40              inc  eax
 *     +18 6A mm           push 1000 / k
 *     +20 59              pop  ecx
 *     +21 ...             unchanged from here on
 *
 * The bytes from +21 on, including the fixup at +24, never move. The
 * guard idea comes from patcher9x (github.com/JHRobotics/patcher9x).
 * patcher9x's own WfW patch is this guard form with k = 100, except
 * that its "sub eax, edx" is encoded 29 D0 instead of 2B C2; both
 * encodings are accepted, so a patcher9x-patched file can be re-tuned
 * or restored too.
 *
 * The routine is found by searching for its instruction sequence, not
 * by fixed offsets, and nothing is changed unless exactly one copy is
 * found. No Microsoft code is included; users patch their own copy.
 *
 * Usage:  NDISFIX [/C | /R | /K:n] [/Y] [path\NDIS.386]
 *
 * Build (OpenWatcom):  wcl -bt=dos -ms -os -fe=NDISFIX.EXE NDISFIX.C
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <conio.h>
#include <dos.h>
#include <i86.h>
#include <io.h>
#include <direct.h>

#define VERSION     "1.2"
#define ORIG_COUNT  0x00100000UL   /* Microsoft's loop count */
#define TARGET_MS   20UL           /* minimum loop time we aim for */
#define RISKY_MS    5UL            /* below this, warn about freezes */
#define GUARD_K     8              /* k from which the guard form is used */
#define SIG_LEN     39
#define BUF_SIZE    4096
#define BAK_SUBDIR  "\\BAK\\NDISFIX"

/* multipliers that divide 1000 evenly */
static const unsigned ks[] = {1, 2, 4, 5, 8, 10, 20, 25, 40, 50, 100,
                              125, 200, 250, 500, 1000};
#define NKS (sizeof ks / sizeof ks[0])

/* What was found in the file */
typedef struct {
    unsigned      k;         /* loop multiplier, 1 = original */
    unsigned char call[6];   /* Get_System_Time call, kept as found */
    unsigned char addr[4];   /* [elapsed_us] address, kept as found */
} routine_t;

/* ------------------------------------------------------------------ */
/* Recognising and building the routine                                */
/* ------------------------------------------------------------------ */

static unsigned long get32(const unsigned char *p)
{
    return  (unsigned long)p[0]        | ((unsigned long)p[1] << 8) |
           ((unsigned long)p[2] << 16) | ((unsigned long)p[3] << 24);
}

static void put32(unsigned char *p, unsigned long v)
{
    p[0] = (unsigned char)v;         p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
}

static int valid_k(unsigned long k)
{
    unsigned i;
    for (i = 0; i < NKS; i++)
        if (ks[i] == k)
            return 1;
    return 0;
}

static int is_call(const unsigned char *p)
{
    static const unsigned char disk[6] = {0xCD,0x20,0x3F,0x00,0x01,0x00};
    return memcmp(p, disk, 6) == 0 ||           /* VxDCall on disk */
           (p[0] == 0xFF && p[1] == 0x15);      /* resolved in memory */
}

/* +21 to +38, the same in every form apart from the address */
static int tail_ok(const unsigned char *p)
{
    return p[21] == 0xF7 && p[22] == 0xE1 &&    /* mul ecx */
           p[23] == 0xA3 &&                     /* mov [elapsed_us], eax */
           p[28] == 0x8B && p[29] == 0xC8 &&    /* mov ecx, eax */
           p[30] == 0xB8 &&                     /* mov eax, n */
           p[35] == 0x2B && p[36] == 0xD2 &&    /* sub edx, edx */
           p[37] == 0xF7 && p[38] == 0xF1;      /* div ecx */
}

/* Does a known form of the routine start at p? */
static int match(const unsigned char *p, routine_t *r)
{
    unsigned long c, m, d;

    if (p[0] != 0xB9 || !tail_ok(p))
        return 0;
    c = get32(p + 1);
    d = get32(p + 31);

    if (p[5] == 0x90) {
        /* plain form: mov ecx,c / nop / loop / call / sub / mov ecx,m */
        if (p[6] != 0xE2 || p[7] != 0xFE || !is_call(p + 8) ||
            p[14] != 0x2B || p[15] != 0xC2 || p[16] != 0xB9)
            return 0;
        m = get32(p + 17);
        if (d != ORIG_COUNT || m == 0 || 1000UL % m != 0 ||
            !valid_k(1000UL / m) || 1000UL / m >= GUARD_K ||
            c != ORIG_COUNT * (1000UL / m))
            return 0;
        r->k = (unsigned)(1000UL / m);          /* 1 = Microsoft's original */
        memcpy(r->call, p + 8, 6);
        memcpy(r->addr, p + 24, 4);
        return 1;
    }

    /* guard form: mov ecx,c / loop / call / sub / jnz / inc / push m / pop */
    if (p[5] != 0xE2 || p[6] != 0xFE || !is_call(p + 7) ||
        !((p[13] == 0x2B && p[14] == 0xC2) ||   /* sub eax, edx (ours) */
          (p[13] == 0x29 && p[14] == 0xD0)) ||  /* sub eax, edx (patcher9x) */
        p[15] != 0x75 || p[16] != 0x01 ||
        p[17] != 0x40 || p[18] != 0x6A || p[20] != 0x59)
        return 0;
    m = p[19];
    if (d != ORIG_COUNT || m == 0 || m > 127 || 1000UL % m != 0 ||
        !valid_k(1000UL / m) || 1000UL / m < GUARD_K ||
        c != ORIG_COUNT * (1000UL / m))
        return 0;
    r->k = (unsigned)(1000UL / m);
    memcpy(r->call, p + 7, 6);
    memcpy(r->addr, p + 24, 4);
    return 1;
}

/* Build the 39 bytes for multiplier k (k = 1 is Microsoft's code). */
static void build(unsigned char *b, unsigned k, const routine_t *r)
{
    unsigned long m = 1000UL / k;

    b[0] = 0xB9;
    put32(b + 1, ORIG_COUNT * k);
    if (k < GUARD_K) {
        b[5] = 0x90;
        b[6] = 0xE2; b[7] = 0xFE;
        memcpy(b + 8, r->call, 6);
        b[14] = 0x2B; b[15] = 0xC2;
        b[16] = 0xB9;
        put32(b + 17, m);
    } else {
        b[5] = 0xE2; b[6] = 0xFE;
        memcpy(b + 7, r->call, 6);
        b[13] = 0x2B; b[14] = 0xC2;
        b[15] = 0x75; b[16] = 0x01;
        b[17] = 0x40;
        b[18] = 0x6A; b[19] = (unsigned char)m;
        b[20] = 0x59;
    }
    b[21] = 0xF7; b[22] = 0xE1;
    b[23] = 0xA3;
    memcpy(b + 24, r->addr, 4);
    b[28] = 0x8B; b[29] = 0xC8;
    b[30] = 0xB8;
    put32(b + 31, ORIG_COUNT);
    b[35] = 0x2B; b[36] = 0xD2;
    b[37] = 0xF7; b[38] = 0xF1;
}

/* Scan a file. Returns number of matches (-1 on read error); fills in
   the offset and details of the first one. */
static int scan(const char *path, long *offset, routine_t *r, long *size)
{
    static unsigned char buf[BUF_SIZE + SIG_LEN];
    FILE *f;
    size_t keep = 0, got, i, avail;
    long base = 0;              /* file offset of buf[0] */
    int found = 0;
    routine_t t;

    f = fopen(path, "rb");
    if (!f)
        return -1;
    for (;;) {
        got = fread(buf + keep, 1, BUF_SIZE, f);
        avail = keep + got;
        if (avail >= SIG_LEN) {
            for (i = 0; i + SIG_LEN <= avail; i++) {
                if (match(buf + i, &t)) {
                    if (found == 0) {
                        *offset = base + (long)i;
                        *r = t;
                    }
                    found++;
                }
            }
            /* keep the last SIG_LEN-1 bytes for matches spanning reads */
            memmove(buf, buf + avail - (SIG_LEN - 1), SIG_LEN - 1);
            base += (long)(avail - (SIG_LEN - 1));
            keep = SIG_LEN - 1;
        } else {
            keep = avail;
        }
        if (got < BUF_SIZE)
            break;
    }
    *size = base + (long)keep;
    if (ferror(f))
        found = -1;
    fclose(f);
    return found;
}

/* Write the routine for multiplier k, then rescan to confirm. */
static int set_k(const char *path, long offset, const routine_t *r,
                 unsigned k)
{
    unsigned char b[SIG_LEN];
    routine_t r2;
    long off2, size;
    FILE *f;
    int ok;

    build(b, k, r);
    f = fopen(path, "r+b");
    if (!f)
        return -1;
    ok = fseek(f, offset, SEEK_SET) == 0 && fwrite(b, 1, SIG_LEN, f) == SIG_LEN;
    if (fclose(f) != 0)
        ok = 0;
    if (!ok)
        return -1;
    if (scan(path, &off2, &r2, &size) != 1 || off2 != offset ||
        r2.k != k)
        return -1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Timing                                                              */
/* ------------------------------------------------------------------ */

/* Spin "loop $" with a 32-bit ECX count, like NDIS.386 does. */
void spin(unsigned long n);
#pragma aux spin =      \
    0x66 0x51           /* push ecx            */ \
    0x52                /* push dx  (high)     */ \
    0x50                /* push ax  (low)      */ \
    0x66 0x59           /* pop  ecx (= dx:ax)  */ \
    0x67 0xE2 0xFD      /* loop $   using ECX  */ \
    0x66 0x59           /* pop  ecx            */ \
    parm [dx ax] modify [];

/* BIOS tick counter at 0040:006C, about 18.2 ticks per second */
static unsigned long ticks(void)
{
    volatile unsigned long __far *t =
        (volatile unsigned long __far *)MK_FP(0x40, 0x6C);
    unsigned long a, b;
    do {                    /* re-read in case the timer fires mid-read */
        a = *t;
        b = *t;
    } while (a != b);
    return a;
}

static unsigned long elapsed_ticks(unsigned long n)
{
    unsigned long t0, t1;

    t0 = ticks();
    while (ticks() == t0)   /* start on a tick edge */
        ;
    t0 = ticks();
    spin(n);
    t1 = ticks();
    if (t1 < t0)            /* passed midnight */
        t1 += 0x1800B0UL;
    return t1 - t0;
}

/* Loop iterations per millisecond on this machine. */
static unsigned long measure_rate(void)
{
    unsigned long n = 0x10000UL, t, ms;

    for (;;) {
        t = elapsed_ticks(n);
        if (t >= 8 || n >= 0x80000000UL)
            break;
        n <<= 1;
    }
    ms = t * 54925UL / 1000UL;      /* one tick is 54.925 ms */
    if (ms == 0)
        ms = 1;
    return n / ms;
}

/* Smallest k giving the loop at least TARGET_MS on this machine. */
static unsigned choose_k(unsigned long rate)
{
    unsigned long need;
    unsigned i;

    if (rate > 0xFFFFFFFFUL / TARGET_MS)
        need = 0xFFFFFFFFUL;
    else
        need = rate * TARGET_MS;
    for (i = 0; i < NKS; i++)
        if (ORIG_COUNT * ks[i] >= need)
            return ks[i];
    return ks[NKS - 1];
}

static unsigned long loop_ms(unsigned k, unsigned long rate)
{
    return rate ? ORIG_COUNT * k / rate : 0;
}

static void describe(const routine_t *r)
{
    if (r->k == 1)
        printf("original");
    else
        printf("patched, %ux", r->k);
}

static void describe_k(unsigned k)
{
    if (k == 1)
        printf("original");
    else
        printf("patched, %ux", k);
}

/* ------------------------------------------------------------------ */
/* Backup and README                                                   */
/* ------------------------------------------------------------------ */

static int file_exists(const char *path)
{
    return access(path, 0) == 0;
}

static const char *base_name(const char *path)
{
    const char *p = path, *s;
    for (s = path; *s; s++)
        if (*s == '\\' || *s == '/' || *s == ':')
            p = s + 1;
    return p;
}

static int copy_file(const char *src, const char *dst)
{
    static unsigned char buf[BUF_SIZE];
    FILE *in, *out;
    size_t n;
    int ok = 1;

    in = fopen(src, "rb");
    if (!in)
        return -1;
    out = fopen(dst, "wb");
    if (!out) {
        fclose(in);
        return -1;
    }
    while ((n = fread(buf, 1, sizeof buf, in)) > 0)
        if (fwrite(buf, 1, n, out) != n) {
            ok = 0;
            break;
        }
    if (ferror(in))
        ok = 0;
    fclose(in);
    if (fclose(out) != 0)
        ok = 0;
    if (!ok)
        remove(dst);
    return ok ? 0 : -1;
}

/* Build X:\BAK\NDISFIX on the drive the driver is on */
static void backup_dir(const char *target, char *dir)
{
    char drive;
    if (target[0] && target[1] == ':')
        drive = (char)toupper((unsigned char)target[0]);
    else {
        unsigned d;
        _dos_getdrive(&d);
        drive = (char)('A' + d - 1);
    }
    sprintf(dir, "%c:%s", drive, BAK_SUBDIR);
}

static int make_dirs(const char *dir)
{
    char tmp[80];
    strcpy(tmp, dir);
    tmp[6] = '\0';                  /* "X:\BAK" */
    mkdir(tmp);                     /* fine if it already exists */
    mkdir(dir);
    return file_exists(dir) ? 0 : -1;
}

static int write_readme(const char *dir, const char *target,
                        const char *backup)
{
    char path[80];
    FILE *f;

    sprintf(path, "%s\\README.TXT", dir);
    f = fopen(path, "w");           /* text mode: CR/LF line endings */
    if (!f)
        return -1;
    fprintf(f,
"NDISFIX backup - what this folder is\n"
"====================================\n"
"\n"
"This folder was made by NDISFIX.EXE. It holds an untouched copy of the\n"
"Windows for Workgroups 3.11 network driver NDIS.386, saved before\n"
"NDISFIX changed it for the first time.\n"
"\n"
"  Original driver:  %s\n"
"  Driver in use:    %s\n"
"\n"
"What the patch is for\n"
"---------------------\n"
"When Windows starts, NDIS.386 does a quick speed test of the computer.\n"
"On fast computers and in virtual machines that test finishes too fast\n"
"to measure, and Windows can freeze on a black screen just after the\n"
"startup logo. It doesn't happen every time, so it can look like a\n"
"flaky machine that needs a few resets before it will boot.\n"
"\n"
"NDISFIX makes the speed test run longer, so it always has something\n"
"to measure. Nothing else about the driver changes.\n"
"\n"
"Why you might undo it\n"
"---------------------\n"
"On genuine older hardware (386, 486, early Pentium) the original test\n"
"works fine. The patched one still works there, but makes Windows take\n"
"longer to start: a few seconds on a 486, longer on a 386. If you moved\n"
"this installation from a fast PC or a virtual machine to an older\n"
"computer, you may want the original timing back.\n"
"\n"
"How to change or undo it\n"
"------------------------\n"
"Easiest: exit Windows to DOS and run NDISFIX again. It measures the\n"
"computer it is running on and sets the driver to suit it, restoring\n"
"the original on a machine that doesn't need the fix. To restore the\n"
"original no matter how fast the computer is, run:  NDISFIX /R\n"
"\n"
"By hand: from DOS (not a DOS box inside Windows), copy the original\n"
"back over the driver in use:\n"
"\n"
"  COPY %s %s\n",
        backup, target, backup, target);
    return fclose(f) == 0 ? 0 : -1;
}

/* Make sure an original copy exists in the backup folder. If there is
   no backup yet and the driver is already patched (for example by
   patcher9x), the copy gets Microsoft's code written back
   into it, so the backup is always the original. */
static int ensure_backup(const char *target, long offset, const routine_t *r,
                         char *dir, char *backup)
{
    backup_dir(target, dir);
    sprintf(backup, "%s\\%s", dir, base_name(target));

    if (file_exists(backup)) {
        printf("Original already saved as %s\n", backup);
        return 0;
    }
    if (make_dirs(dir) != 0) {
        printf("ERROR: could not create %s\n", dir);
        return -1;
    }
    if (copy_file(target, backup) != 0) {
        printf("ERROR: could not copy the driver to %s\n", backup);
        return -1;
    }
    if (r->k != 1 && set_k(backup, offset, r, 1) != 0) {
        printf("ERROR: could not restore the original code in %s\n", backup);
        remove(backup);
        return -1;
    }
    printf("Original saved as %s\n", backup);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

static int windows_running(void)
{
    union REGS r;
    r.x.ax = 0x1600;                /* enhanced-mode Windows check */
    int86(0x2F, &r, &r);
    return r.h.al != 0x00 && r.h.al != 0x80;
}

static void usage(void)
{
    printf(
"Usage: NDISFIX [/C | /R | /K:n] [/Y] [path\\NDIS.386]\n"
"\n"
"  (no option)  measure this computer and set NDIS.386 to suit it\n"
"  /C           check only, change nothing\n"
"  /R           restore the original timing\n"
"  /K:n         use multiplier n instead of measuring (n divides 1000)\n"
"  /Y           don't ask before changing the file\n"
"\n"
"The default file is %%WINDIR%%\\SYSTEM\\NDIS.386, or C:\\WINDOWS\\SYSTEM\\NDIS.386.\n"
"A copy of the original is kept in \\BAK\\NDISFIX on the same drive.\n"
"Run it from DOS, not from a DOS box inside Windows.\n");
}

int main(int argc, char **argv)
{
    char target[80] = "", dir[80], backup[80];
    int check_only = 0, restore = 0, yes = 0, i, n;
    unsigned force_k = 0, want;
    long offset = 0, size = 0;
    unsigned long rate = 0;
    routine_t cur;
    const char *windir;

    printf("NDISFIX " VERSION " - NDIS.386 startup freeze fix for "
           "Windows for Workgroups 3.11\n\n");

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] == '/' || a[0] == '-') {
            switch (toupper((unsigned char)a[1])) {
            case 'C': check_only = 1; break;
            case 'R': restore = 1; break;
            case 'Y': yes = 1; break;
            case 'K':
                if ((a[2] == ':' || a[2] == '=') && isdigit((unsigned char)a[3]))
                    force_k = (unsigned)atoi(a + 3);
                if (!valid_k(force_k)) {
                    printf("/K needs a multiplier that divides 1000: "
                           "1 2 4 5 8 10 20 25 40 50 100 125 200 250 500 1000\n");
                    return 3;
                }
                break;
            default:  usage(); return a[1] == '?' ? 0 : 3;
            }
        } else if (!target[0] && strlen(a) < 64) {
            strcpy(target, a);
        } else {
            usage();
            return 3;
        }
    }
    if (check_only + restore + (force_k != 0) > 1) {
        usage();
        return 3;
    }
    if (!target[0]) {
        windir = getenv("WINDIR");
        if (windir && strlen(windir) < 50)
            sprintf(target, "%s\\SYSTEM\\NDIS.386", windir);
        else
            strcpy(target, "C:\\WINDOWS\\SYSTEM\\NDIS.386");
    }
    strupr(target);

    if (!check_only && windows_running()) {
        printf("Windows is running. Exit Windows and run NDISFIX from the DOS\n"
               "prompt, because Windows has NDIS.386 loaded right now.\n");
        return 3;
    }

    n = scan(target, &offset, &cur, &size);
    if (n < 0) {
        printf("Can't read %s\n", target);
        if (!file_exists(target))
            printf("If NDIS.386 is somewhere else, give its path:  "
                   "NDISFIX path\\NDIS.386\n");
        return 2;
    }
    printf("File:     %s (%ld bytes)\n", target, size);
    if (n != 1) {
        printf("\nFound %d copies of the timing routine; expected exactly one.\n"
               "This isn't a version of NDIS.386 that NDISFIX knows, so it has\n"
               "been left alone.\n", n);
        return 2;
    }
    printf("Routine:  at offset %lXh\n", offset);
    printf("Current:  ");
    describe(&cur);
    printf("\n");

    if (restore) {
        want = 1;
    } else if (force_k) {
        want = force_k;
    } else {
        printf("\nMeasuring this computer's speed...");
        fflush(stdout);
        rate = measure_rate();
        want = choose_k(rate);
        printf(" done\n");
        printf("Startup test would take about %lu ms with the current setting",
               loop_ms(cur.k, rate));
        if (loop_ms(cur.k, rate) < RISKY_MS)
            printf(" -\nshort enough to risk the black-screen freeze");
        printf(".\n");
        printf("Best for this computer: ");
        describe_k(want);
        printf(", about %lu ms.\n", loop_ms(want, rate));
        if (want == ks[NKS - 1] && loop_ms(want, rate) < TARGET_MS)
            printf("Note: this computer is faster than NDISFIX can fully "
                   "allow for,\nbut the patch's zero check still prevents "
                   "the freeze.\n");
    }

    if (want == cur.k) {
        printf("\nNo change needed.\n");
        return 0;
    }
    if (check_only) {
        printf("\nRun NDISFIX without /C to change it.\n");
        return 1;
    }

    printf("\nThis will change NDIS.386 from ");
    describe(&cur);
    printf("\nto ");
    describe_k(want);
    printf(".\n");
    if (!yes) {
        int k;
        printf("Continue? (Y/N) ");
        fflush(stdout);
        k = getch();
        printf("%c\n", isprint(k) ? k : ' ');
        if (toupper(k) != 'Y') {
            printf("Nothing changed.\n");
            return 1;
        }
    }

    if (ensure_backup(target, offset, &cur, dir, backup) != 0) {
        printf("Nothing changed.\n");
        return 2;
    }
    if (write_readme(dir, target, backup) != 0)
        printf("Warning: could not write %s\\README.TXT\n", dir);

    if (set_k(target, offset, &cur, want) != 0) {
        printf("ERROR: writing %s failed. If Windows won't start, restore the\n"
               "original from DOS:  COPY %s %s\n", target, backup, target);
        return 2;
    }
    if (want == 1)
        printf("Done. NDIS.386 has its original timing back.\n");
    else
        printf("Done. NDIS.386 is patched for this computer.\n");
    printf("To undo, run NDISFIX /R, or see %s\\README.TXT\n", dir);
    return 0;
}
