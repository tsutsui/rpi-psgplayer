/*
 * psg_play.c
 *  Minimal YM2149 (AY-3-8910 compatible) player
 *  using GPIO via /dev/mem mmap(2) on Raspberry Pi 3B
 *
 * Wiring (BC2=H fixed, A8=H A9=L fixed):
 *   GPIO4..11 -> DA0..7 (LSB=GPIO4)
 *   GPIO12    -> BDIR
 *   GPIO13    -> BC1
 *   GPIO16    -> RESET (active-high)
 *
 */

#include <sys/types.h>
#include <sys/mman.h>
#include <sys/select.h>

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "psg_driver.h"
#include "player_ui.h"

#define PERI_BASE   0x3F000000u
#define GPIO_BASE   (PERI_BASE + 0x200000u)
#define GPIO_SIZE   0x1000u

/* GPIO registers */
#define GPFSEL0     0x00
#define GPFSEL1     0x04
#define GPFSEL2     0x08
#define GPSET0      0x1c
#define GPCLR0      0x28
#define GPLEV0      0x34

/* ---- GPIO pin assignment (Raspberry Pi GPIO numbering) ---- */
enum {
    PIN_D0   = 4,  /* DA0 */
    PIN_D1   = 5,
    PIN_D2   = 6,
    PIN_D3   = 7,
    PIN_D4   = 8,
    PIN_D5   = 9,
    PIN_D6   = 10,
    PIN_D7   = 11, /* DA7 */

    PIN_BDIR = 12,
    PIN_BC1  = 13,

    PIN_RESET = 16
};

/* Masks */
#define MASK_DATABUS   (0xFFu << PIN_D0)              /* GPIO4..11 */
#define MASK_BDIR      (1u << PIN_BDIR)
#define MASK_BC1       (1u << PIN_BC1)
#define MASK_CTRL      (MASK_BDIR | MASK_BC1)
#define MASK_RESET     (1u << PIN_RESET)

static volatile uint32_t *gpio;
static volatile sig_atomic_t g_stop = 0;
static volatile sig_atomic_t g_redraw = 0;

static void
on_signal(int signo)
{
    (void)signo;
    g_stop = 1;
}

static void
die(const char *msg)
{
    perror(msg);
    exit(1);
}

static void
sleep_us(long us)
{
    struct timespec ts;
    ts.tv_sec = us / 1000000;
    ts.tv_nsec = (us % 1000000) * 1000;
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR)
        ;
}

/* Minimal memory barrier (ordering for MMIO) */
static inline void
mmio_barrier(void)
{
#if defined(__arm__) || defined(__aarch64__)
    __asm__ volatile("dmb ish" ::: "memory");
#else
    __sync_synchronize();
#endif
}

/* Set GPIO function to output: fsel=001 */
static void
gpio_config_output(int pin)
{
    uint32_t reg = pin / 10;          /* each GPFSEL covers 10 pins */
    uint32_t shift = (pin % 10) * 3;
    volatile uint32_t *fsel = &gpio[GPFSEL0 / 4 + reg];

    uint32_t v = *fsel;
    v &= ~(7u << shift);
    v |=  (1u << shift);
    *fsel = v;
    mmio_barrier();
}

static void
gpio_config(void)
{
    /* configure pins as output */
    for (int pin = PIN_D0; pin <= PIN_D7; pin++) {
        gpio_config_output(pin);
    }
    gpio_config_output(PIN_BDIR);
    gpio_config_output(PIN_BC1);
    gpio_config_output(PIN_RESET);
}

/* Write multiple pins at once: set_mask bits become 1, clr_mask bits become 0 */
static inline void
gpio_write_masks(uint32_t set_mask, uint32_t clr_mask)
{
    if (clr_mask)
        gpio[GPCLR0 / 4] = clr_mask;
    if (set_mask)
        gpio[GPSET0 / 4] = set_mask;
    mmio_barrier();
}

/* Dummy GPIO register reads for wait by I/O */
static inline void
gpio_wait(void)
{
/* 2 seems enough even on AY-3-8910, but for safety */
#define NREAD   3u

    volatile uint32_t dummy;
    for (unsigned int i = 0; i < NREAD; i++) {
        dummy = gpio[GPCLR0];
        (void)dummy;
        dummy = gpio[GPSET0];
        (void)dummy;
    }
    mmio_barrier();
}

/* Put value on data bus GPIO4..11 in one operation (2 stores: clear then set) */
static inline void
bus_write8(uint8_t v)
{
    uint32_t setm = ((uint32_t)v << PIN_D0) & MASK_DATABUS;
    uint32_t clrm = MASK_DATABUS & ~setm;
    gpio_write_masks(setm, clrm);
}

/*
 * YM2149/AY-3-8910 bus control (BC2 fixed HIGH):
 *   BDIR BC1  Function
 *    0    0   Inactive
 *    0    1   Read  (unused here)
 *    1    0   Write (data)
 *    1    1   Latch address
 *
 * The key: for "latch address", we set both bits HIGH in one GPSET write
 * (no intermediate READ/WRITE state).
 */
static inline void
ctrl_inactive(void)
{
    gpio_write_masks(0, MASK_CTRL);
}

static inline void
ctrl_latch_addr(void)
{
    /* set both in one shot (no intermediate state) */
    gpio_write_masks(MASK_CTRL, 0);
}

static inline void
ctrl_write_data(void)
{
    /*
     * Ensure BC1=0 first; then set BDIR=1 (can be two stores but very tight).
     * If we always go through inactive before write, we can do just "set BDIR".
     */
    gpio_write_masks(MASK_BDIR, MASK_BC1);
}

static void
ym_reset_pulse(void)
{
    /* RESETはアクティブHigh想定 (I/F回路はオープンコレクタTr経由で駆動) */
    gpio_write_masks(0, MASK_RESET);      /* deassert = 0 */
    sleep_us(10);
    gpio_write_masks(MASK_RESET, 0);      /* assert = 1 */
    sleep_us(1000);
    gpio_write_masks(0, MASK_RESET);      /* deassert = 0*/
    sleep_us(1000);
}

static void
ym_latch_addr(uint8_t reg)
{
    bus_write8(reg & 0x0f);
    ctrl_latch_addr();
    /* Wait address setup time: 300 ns on YM2149F, 400 ns on AY-3-8910 */
    gpio_wait();
    ctrl_inactive();
}

static void
ym_write_data(uint8_t data)
{
    bus_write8(data);
    mmio_barrier();
    ctrl_inactive();
    /* recommended: always start from inactive so only BDIR needs to be raised */
    mmio_barrier();
    ctrl_write_data();
    /* Wait write signal time: 300 ns on YM2149F, 500 ns on AY-3-8910 */
    gpio_wait();
    ctrl_inactive();
}

static void
ym_write_reg(uint8_t reg, uint8_t val)
{
    ym_latch_addr(reg);
    ym_write_data(val);
}

/* --- timing helpers --- */
static inline uint64_t
nsec_now_monotonic(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* One-time init to make sure the chip is in a known state. */
static void
psg_hw_reset(void)
{
    ctrl_inactive();
    bus_write8(0x00);
    ym_reset_pulse();
}

static void
psg_hw_init(void)
{
    psg_hw_reset();

    /* Silence */
    ym_write_reg(AY_AVOL, 0);
    ym_write_reg(AY_BVOL, 0);
    ym_write_reg(AY_CVOL, 0);
}

static void
psg_write_reg(void *user, uint8_t reg, uint8_t val)
{
    ym_write_reg(reg, val);
    PSGDriver *drv = user;
    UI_state *ui = drv->note_user;
    ui_on_reg_write(ui, reg, val);
}

static void
ui_note_event_cb(void *user, int ch, uint8_t octave, uint8_t note,
                 uint8_t volume, uint16_t len, uint8_t is_rest,
                 uint16_t bpm_x10)
{
    PSGDriver *drv = user;
    UI_state *ui = drv->note_user;
    uint64_t now = nsec_now_monotonic();
    ui_on_note_event(ui, now, ch, octave, note, volume, len, is_rest, bpm_x10);
}

static void
usage(void)
{
    fprintf(stderr,
        "Usage: %s [-t title] p6psgfile\n", getprogname());

    exit(EXIT_FAILURE);
}

int
main(int argc, char **argv)
{
    const char *ifname;
    const char *dev = "/dev/mem";
    const char *title = NULL;
    PSGDriver psgdriver, *drv;
    UI_state uistate, *ui;

    int ch;
    while ((ch = getopt(argc, argv, "t:")) != -1) {
        switch (ch) {
        case 't':
            title = optarg;
            break;
        default:
            usage();
        }
    }
    argc -= optind;
    argv += optind;

    if (argc != 1)
        usage();
    ifname = argv[0];

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /* read P6 PSG data file */
    FILE *p6psgfile = fopen(ifname, "rb");
    if (p6psgfile == NULL) {
        die("fopen p6psgfile");
    }
    fseek(p6psgfile, 0, SEEK_END);
    size_t p6size = ftell(p6psgfile);
    fseek(p6psgfile, 0, SEEK_SET);
    if (p6size < (8 + 3)) {
        die("p6psgfile too short");
    }
    uint8_t *psgdata = malloc(p6size);
    if (psgdata == NULL) {
        die("malloc p6psgfile");
    }
    if (fread(psgdata, 1, p6size, p6psgfile) != p6size) {
        die("fread p6psgfile");
    }
    (void)fclose(p6psgfile);

    /* parse and split P6 PSG data file per channel */
    uint16_t a_addr = ((uint16_t)psgdata[1] << 8) | psgdata[0];
    uint16_t b_addr = ((uint16_t)psgdata[3] << 8) | psgdata[2];
    uint16_t c_addr = ((uint16_t)psgdata[5] << 8) | psgdata[4];
    if (c_addr > p6size || b_addr >= c_addr || a_addr >= b_addr || a_addr < 8) {
        die("p6psgfile invalid address");
    }
    uint16_t a_size = b_addr - a_addr;
    uint16_t b_size = c_addr - b_addr;
    uint16_t c_size = p6size - c_addr;
    if (psgdata[a_addr + a_size - 1] != 0xff ||
        psgdata[b_addr + b_size - 1] != 0xff ||
        psgdata[c_addr + c_size - 1] != 0xff) {
        die("p6psgfile invalid data");
    }

    int fd = open(dev, O_RDWR | O_SYNC);
    if (fd == -1)
        die("open(/dev/mem)");

    void *p = mmap(NULL, GPIO_SIZE, PROT_READ|PROT_WRITE, MAP_SHARED,
      fd, GPIO_BASE);
    if (p == MAP_FAILED)
        die("mmap(GPIO)");

    gpio = (volatile uint32_t *)p;

    gpio_config();

    psg_hw_init();

    ui = &uistate;
    int ui_active = 0;
    uint64_t now0 = nsec_now_monotonic();
    ui_init(ui, now0);
    ui_active = 1;

    drv = &psgdriver;
    psg_driver_init(drv, psg_write_reg, (void *)gpio, ui_note_event_cb, ui);
    psg_driver_set_channel_data(drv, 0, &psgdata[a_addr]);
    psg_driver_set_channel_data(drv, 1, &psgdata[b_addr]);
    psg_driver_set_channel_data(drv, 2, &psgdata[c_addr]);
    psg_driver_start(drv);

    /*
     * main player loop
     */

    /* call PSG driver every 2ms */
    const uint64_t tick_ns = 2000000ull;
    uint64_t t0 = nsec_now_monotonic();
    uint64_t next_deadline = t0 + tick_ns;

    fprintf(stderr, "Start 2ms tick loop\n");

    while (g_stop == 0) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(STDIN_FILENO, &rfds);
        /*
         * Use select(2) as a 2ms-ish sleep. On HZ=500 kernels, it aligns well.
         * We still correct drift with monotonic time below.
         */
        struct timeval tv;
        tv.tv_sec  = 0;
        tv.tv_usec = 2000; /* 2ms */
        int n = select(STDIN_FILENO + 1, &rfds, NULL, NULL, &tv);

        if (n > 0 && FD_ISSET(STDIN_FILENO, &rfds)) {
            uint8_t buf[64];
            ssize_t r = read(STDIN_FILENO, buf, sizeof(buf));
            if (r > 0) {
                for (ssize_t i = 0; i < r; i++) {
                    if (buf[i] == 0x0c)
                        g_redraw = 1;  /* Ctrl+L */
                    if (buf[i] == 'q' || buf[i] == 'Q')
                        g_stop = 1;    /* quit */
                }
            }
        }
        /*
         * Determine how many ticks are due. This keeps timing stable even if
         * select() returns late.
         */
        uint64_t now = nsec_now_monotonic();

        if (now < next_deadline) {
            /* Early wake; just continue (rare on coarse tick systems). */
            continue;
        }

        /* Catch up for all missed ticks, but cap if you prefer. */
        uint64_t behind = now - next_deadline;
        uint32_t due = (uint32_t)(behind / tick_ns) + 1;

        /* Optional safety cap to avoid spiral if the system is overloaded */
        if (due > 50)
            due = 50;

        for (uint32_t i = 0; i < due; i++) {
            psg_driver_tick(drv);
            next_deadline += tick_ns;
        }

        /* draw AFTER catch-up loop (important) */
        if (g_redraw) {
            ui->have_prev = 0;
            g_redraw = 0;
        }
        ui_maybe_render(ui, now, title != NULL ? title : "OSC demo");
    }

    psg_driver_stop(drv);
    psg_hw_reset();

    if (ui_active)
        ui_shutdown(ui);

    munmap((void*)gpio, GPIO_SIZE);
    close(fd);
    free(psgdata);
    return 0;
}
