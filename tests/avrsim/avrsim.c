/*
 * avrsim - a simulated AVR target behind a ppdev parallel port, for testing
 * paravr without hardware. Load it into paravr with LD_PRELOAD.
 *
 * Opening AVRSIM_DEV (default /dev/parport0) yields a fake port wired the way
 * paravr expects: D0 = MOSI, D1 = SCK, D2 = RESET, MISO on a STATUS pin. A
 * standard port inverts BUSY (pin 11) and no other STATUS input; unconnected
 * inputs read high. Behind the port sits an AVR speaking the serial
 * programming protocol.
 *
 * Sleeps never sleep; they advance a virtual clock. Every port access costs
 * 1 us of it. The target checks its timing against that clock: SCK edges in
 * the first 20 ms after RESET are ignored, instructions arriving while a page
 * write (4.5 ms) or chip erase (9 ms) is in progress are dropped, and edges
 * closer together than AVRSIM_MIN_PHASE_US are missed.
 *
 * Environment:
 *   AVRSIM_DEV           device path to simulate (default /dev/parport0)
 *   AVRSIM_MCU           m328p (default), t85 or m2560
 *   AVRSIM_FLASH         raw flash image, loaded on open if it exists, saved on close
 *   AVRSIM_FUSES         low,high,ext,lock in hex (default FF,DE,FD,CF)
 *   AVRSIM_PIN           STATUS pin carrying MISO: 10 (default), 11, 12, 13 or 15
 *   AVRSIM_CARD_INVERT   1 = the card also inverts the MISO pin (non-standard)
 *   AVRSIM_RESET_INVERT  1 = RESET reaches the target through an inverter
 *   AVRSIM_MIN_PHASE_US  shortest SCK phase the target can follow (default 0)
 *   AVRSIM_VERBOSE       1 = print counters to stderr when the port is closed
 */

#define _GNU_SOURCE
#undef _FORTIFY_SOURCE /* fortified headers define open() inline */

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/ppdev.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define US 1000ull
#define MS 1000000ull
#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

struct mcu {
    const char *id;
    uint8_t sig[3];
    uint32_t flash_size, page_size;
};

static const struct mcu MCUS[] = {
    {"m328p", {0x1E, 0x95, 0x0F}, 32768, 128},
    {"t85", {0x1E, 0x93, 0x0B}, 8192, 64},
    {"m2560", {0x1E, 0x98, 0x01}, 262144, 256},
};

static int sim_fd = -1;
static uint64_t now; /* virtual clock, ns */

static struct {
    /* configuration */
    const struct mcu *mcu;
    const char *flash_path;
    int miso_pin, card_invert, reset_invert, verbose;
    uint64_t min_phase;
    uint8_t fuses[4]; /* low, high, ext, lock */

    /* port */
    int host_sck, host_edges;
    uint64_t last_edge;

    /* target */
    uint8_t flash[262144];
    uint8_t page[256];
    int in_reset, enabled;
    uint64_t reset_at, busy_until;
    int avr_sck, nbits, idx;
    uint8_t in, out, frame[4], ext;

    /* counters */
    unsigned long frames, enables, erases, page_writes, busy_drops, early_edges, missed_edges;
} S;

static void *next(const char *name)
{
    void *f = dlsym(RTLD_NEXT, name);
    if (!f) {
        fprintf(stderr, "avrsim: %s not found\n", name);
        abort();
    }
    return f;
}

static long env_long(const char *name, long def)
{
    const char *v = getenv(name);
    return v ? strtol(v, NULL, 0) : def;
}

/* ------------------------------------------------------------------------ */
/* Target                                                                   */

static int busy(void) { return now < S.busy_until; }

static int is_read(const uint8_t *f)
{
    return f[0] == 0x20 || f[0] == 0x28 || f[0] == 0x30 || f[0] == 0x50 || f[0] == 0x58 ||
           f[0] == 0xF0;
}

/* Word address of a flash instruction; wraps like the hardware. */
static uint32_t word_addr(const uint8_t *f)
{
    return ((uint32_t)S.ext << 16 | f[1] << 8 | f[2]) & (S.mcu->flash_size / 2 - 1);
}

static uint8_t read_result(const uint8_t *f)
{
    switch (f[0]) {
    case 0x20:
    case 0x28: return S.flash[word_addr(f) * 2 + (f[0] == 0x28)];
    case 0x30: return (f[2] & 3) < 3 ? S.mcu->sig[f[2] & 3] : 0xFF;
    case 0x50: return f[1] == 0x08 ? S.fuses[2] : S.fuses[0];
    case 0x58: return f[1] == 0x08 ? S.fuses[1] : S.fuses[3];
    case 0xF0: return busy();
    }
    return 0xFF;
}

static void execute(const uint8_t *f)
{
    uint32_t page_words = S.mcu->page_size / 2;
    switch (f[0]) {
    case 0xAC:
        if (f[1] == 0x80) { /* chip erase */
            memset(S.flash, 0xFF, S.mcu->flash_size);
            S.fuses[3] = 0xFF;
            S.busy_until = now + 9 * MS;
            S.erases++;
        }
        break;
    case 0x40: S.page[(f[2] & (page_words - 1)) * 2] = f[3]; break;
    case 0x48: S.page[(f[2] & (page_words - 1)) * 2 + 1] = f[3]; break;
    case 0x4C: { /* write page; flash bits can only be cleared */
        uint8_t *dst = S.flash + (word_addr(f) & ~(page_words - 1)) * 2;
        for (uint32_t i = 0; i < S.mcu->page_size; i++) dst[i] &= S.page[i];
        memset(S.page, 0xFF, sizeof S.page);
        S.busy_until = now + 4500 * US;
        S.page_writes++;
        break;
    }
    case 0x4D: /* load extended address */
        if (S.mcu->flash_size > 0x20000) S.ext = f[2];
        break;
    }
}

/*
 * Instructions are 4-byte frames counted from RESET. While receiving a byte
 * the target shifts out the previous one, except that the 4th byte of a read
 * returns the result. Before Programming Enable (AC 53) nothing else is acted on.
 */
static void byte_received(uint8_t b)
{
    S.frame[S.idx] = b;
    S.out = b;
    if (S.idx == 2 && S.enabled && is_read(S.frame)) {
        if (busy() && S.frame[0] != 0xF0) {
            S.busy_drops++;
            S.out = 0xFF;
        } else {
            S.out = read_result(S.frame);
        }
    }
    if (S.idx == 3) {
        S.frames++;
        if (!S.enabled) {
            if (S.frame[0] == 0xAC && S.frame[1] == 0x53) {
                S.enabled = 1;
                S.enables++;
            }
        } else if (!is_read(S.frame)) {
            if (busy())
                S.busy_drops++;
            else
                execute(S.frame);
        }
    }
    S.idx = (S.idx + 1) & 3;
}

/* ------------------------------------------------------------------------ */
/* Port                                                                     */

static void write_data(uint8_t d)
{
    now += US;

    int in_reset = S.reset_invert ? (d & 4) != 0 : (d & 4) == 0;
    if (in_reset != S.in_reset) {
        S.in_reset = in_reset;
        S.enabled = 0;
        if (in_reset) {
            S.reset_at = now;
            S.nbits = S.idx = 0;
            S.in = S.out = S.ext = 0;
            memset(S.page, 0xFF, sizeof S.page);
        }
    }

    int sck = (d & 2) != 0;
    if (sck == S.host_sck) return;
    int missed = S.min_phase && S.host_edges && now - S.last_edge < S.min_phase;
    S.host_sck = sck;
    S.last_edge = now;
    S.host_edges++;
    if (missed) {
        S.missed_edges++;
        return;
    }
    if (sck == S.avr_sck) return;
    S.avr_sck = sck;
    if (!S.in_reset) return; /* running: the programming interface is off */
    if (now - S.reset_at < 20 * MS) {
        S.early_edges++;
        return;
    }
    if (sck) { /* rising edge: sample MOSI */
        S.in = (uint8_t)(S.in << 1 | (d & 1));
    } else if (++S.nbits == 8) { /* falling edge: next MISO bit */
        S.nbits = 0;
        byte_received(S.in);
        S.in = 0;
    }
}

static uint8_t read_status(void)
{
    static const struct {
        int pin;
        uint8_t mask;
    } PINS[] = {{10, 0x40}, {11, 0x80}, {12, 0x20}, {13, 0x10}, {15, 0x08}};

    now += US;
    int miso = S.in_reset ? (S.out >> (7 - S.nbits)) & 1 : 1; /* undriven: pulled high */
    uint8_t r = 0;
    for (size_t i = 0; i < COUNT(PINS); i++) {
        int is_miso = PINS[i].pin == S.miso_pin;
        int bit = is_miso ? miso : 1;
        if (PINS[i].pin == 11) bit = !bit;
        if (is_miso && S.card_invert) bit = !bit;
        if (bit) r |= PINS[i].mask;
    }
    return r;
}

static void sim_open(void)
{
    memset(&S, 0, sizeof S);
    const char *v = getenv("AVRSIM_MCU");
    S.mcu = &MCUS[0];
    if (v) {
        S.mcu = NULL;
        for (size_t i = 0; i < COUNT(MCUS); i++)
            if (strcmp(v, MCUS[i].id) == 0) S.mcu = &MCUS[i];
        if (!S.mcu) {
            fprintf(stderr, "avrsim: unknown AVRSIM_MCU '%s'\n", v);
            abort();
        }
    }
    S.flash_path = getenv("AVRSIM_FLASH");
    S.miso_pin = (int)env_long("AVRSIM_PIN", 10);
    S.card_invert = (int)env_long("AVRSIM_CARD_INVERT", 0);
    S.reset_invert = (int)env_long("AVRSIM_RESET_INVERT", 0);
    S.min_phase = (uint64_t)env_long("AVRSIM_MIN_PHASE_US", 0) * US;
    S.verbose = (int)env_long("AVRSIM_VERBOSE", 0);

    unsigned f[4] = {0xFF, 0xDE, 0xFD, 0xCF};
    if ((v = getenv("AVRSIM_FUSES"))) sscanf(v, "%x,%x,%x,%x", &f[0], &f[1], &f[2], &f[3]);
    for (int i = 0; i < 4; i++) S.fuses[i] = (uint8_t)f[i];

    memset(S.flash, 0xFF, sizeof S.flash);
    FILE *fp = S.flash_path ? fopen(S.flash_path, "rb") : NULL;
    if (fp) {
        if (fread(S.flash, 1, S.mcu->flash_size, fp) == 0) fprintf(stderr, "avrsim: empty flash file\n");
        fclose(fp);
    }
}

static void sim_close(void)
{
    FILE *fp = S.flash_path ? fopen(S.flash_path, "wb") : NULL;
    if (fp) {
        fwrite(S.flash, 1, S.mcu->flash_size, fp);
        fclose(fp);
    }
    if (S.verbose)
        fprintf(stderr,
                "avrsim: frames=%lu enables=%lu erases=%lu page_writes=%lu busy_drops=%lu "
                "early_edges=%lu missed_edges=%lu time=%.1fms\n",
                S.frames, S.enables, S.erases, S.page_writes, S.busy_drops, S.early_edges,
                S.missed_edges, (double)now / MS);
    sim_fd = -1;
}

__attribute__((destructor)) static void sim_unload(void)
{
    if (sim_fd >= 0) sim_close();
}

/* ------------------------------------------------------------------------ */
/* Interposed libc functions                                                */

static int is_device(const char *path)
{
    const char *dev = getenv("AVRSIM_DEV");
    return path && strcmp(path, dev ? dev : "/dev/parport0") == 0;
}

static int open_device(void)
{
    static int (*real_open)(const char *, int, ...);
    if (!real_open) real_open = next("open");
    int fd = real_open("/dev/null", O_RDWR);
    if (fd >= 0) {
        sim_fd = fd;
        sim_open();
    }
    return fd;
}

#define NEEDS_MODE(flags) (((flags) & O_CREAT) || ((flags) & O_TMPFILE) == O_TMPFILE)

#define WRAP_OPEN(name)                                                                            \
    int name(const char *path, int flags, ...)                                                    \
    {                                                                                              \
        static int (*real)(const char *, int, ...);                                                \
        mode_t mode = 0;                                                                           \
        if (NEEDS_MODE(flags)) {                                                                   \
            va_list ap;                                                                            \
            va_start(ap, flags);                                                                   \
            mode = va_arg(ap, mode_t);                                                             \
            va_end(ap);                                                                            \
        }                                                                                          \
        if (is_device(path)) return open_device();                                                 \
        if (!real) real = next(#name);                                                             \
        return real(path, flags, mode);                                                            \
    }

WRAP_OPEN(open)
WRAP_OPEN(open64)

/* What fortified callers use instead of open() */
#define WRAP_OPEN_2(name)                                                                          \
    int name(const char *path, int flags)                                                         \
    {                                                                                              \
        static int (*real)(const char *, int);                                                     \
        if (is_device(path)) return open_device();                                                 \
        if (!real) real = next(#name);                                                             \
        return real(path, flags);                                                                  \
    }

WRAP_OPEN_2(__open_2)
WRAP_OPEN_2(__open64_2)

int close(int fd)
{
    static int (*real)(int);
    if (!real) real = next("close");
    if (fd >= 0 && fd == sim_fd) sim_close();
    return real(fd);
}

int ioctl(int fd, unsigned long request, ...)
{
    va_list ap;
    va_start(ap, request);
    void *arg = va_arg(ap, void *);
    va_end(ap);

    if (fd < 0 || fd != sim_fd) {
        static int (*real)(int, unsigned long, ...);
        if (!real) real = next("ioctl");
        return real(fd, request, arg);
    }
    switch (request) {
    case PPCLAIM:
    case PPRELEASE:
    case PPDATADIR: return 0;
    case PPWDATA: write_data(*(unsigned char *)arg); return 0;
    case PPRSTATUS: *(unsigned char *)arg = read_status(); return 0;
    }
    fprintf(stderr, "avrsim: unsupported ioctl 0x%lx\n", request);
    errno = ENOTTY;
    return -1;
}

int nanosleep(const struct timespec *req, struct timespec *rem)
{
    (void)rem;
    now += (uint64_t)req->tv_sec * 1000000000ull + (uint64_t)req->tv_nsec;
    return 0;
}

int clock_nanosleep(clockid_t clock, int flags, const struct timespec *req, struct timespec *rem)
{
    (void)rem;
    int64_t ns = (int64_t)req->tv_sec * 1000000000 + req->tv_nsec;
    if (flags & TIMER_ABSTIME) {
        struct timespec t;
        clock_gettime(clock, &t);
        ns -= (int64_t)t.tv_sec * 1000000000 + t.tv_nsec;
    }
    if (ns > 0) now += (uint64_t)ns;
    return 0;
}
