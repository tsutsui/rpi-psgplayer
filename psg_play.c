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
    /* assume active-low reset */
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
    /* XXX: Address setup time: 300 ns */
    ctrl_latch_addr();
    ctrl_latch_addr();
    ctrl_latch_addr();
    ctrl_latch_addr();
    /* timing is extremely relaxed compared to chip requirements */
    //sleep_us(1);
    mmio_barrier();
    ctrl_inactive();
    mmio_barrier();
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
    /* XXX: Write signal time: 300 ns */
    ctrl_write_data();
    ctrl_write_data();
    ctrl_write_data();
    ctrl_write_data();
    mmio_barrier();
    ctrl_inactive();
    mmio_barrier();
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

    /* Basic: enable tones (0..2 = 0), disable noise (3..5 = 1) => 0x38 */
    ym_write_reg(AY_ENABLE, 0x38);
    ym_write_reg(AY_NOISEPER, 0x00);
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

static PSGDriver driver_private;

static const uint8_t test_data_ch0[] = {
    /* o4 v15 */
    0x85,       /* 8x: octave */
    0x9f,       /* 9x: volume */

    0x21, 96,   /* C  (1) len_flag=2 (0x2<<4 | 0x1 = 0x21) */
    0x84,       /* 8x: octave */
    0x2c, 96,   /* B  (11) */
    0x85,       /* 8x: octave */
    0x21, 96,   /* C  (1) */

    0xff        /* end mark */
};

static const uint8_t test_data_ch1[] = {
    /* o5 v15 */
    0x85,       /* 8x: octave */
    0x9f,       /* 9x: volume */

    0x25, 96,   /* E  (5) */
    0x23, 96,   /* D  (3) */
    0x25, 96,   /* E  (5) */

    0xff        /* end mark */
};

static const uint8_t test_data_ch2[] = {
    /* o5 v15 */
    0x85,       /* 8x: octave */
    0x9f,       /* 9x: volume */

    0x28, 96,   /* G  (8) */
    0x28, 96,   /* G  (8) */
    0x28, 96,   /* G  (8) */

    0xff        /* end mark */
};

static void
psg_write_reg(void *user, uint8_t reg, uint8_t val)
{
    (void)user;
    ym_write_reg(reg, val);
    printf("%s: reg: %2x, val: %2x\n", __func__, reg, val);
}

#if 0
static void
usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s\n", prog);

    exit(EXIT_FAILURE);
}
#endif

int
main(int argc, char **argv)
{
    const char *dev = "/dev/mem";

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

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

    PSGDriver *drv = &driver_private;
    psg_driver_init(drv, psg_write_reg, (void *)gpio);
    psg_driver_set_channel_data(drv, 0, test_data_ch0);
    psg_driver_set_channel_data(drv, 1, test_data_ch1);
    psg_driver_set_channel_data(drv, 2, test_data_ch2);
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
        /*
         * Use select(2) as a 2ms-ish sleep. On HZ=500 kernels, it aligns well.
         * We still correct drift with monotonic time below.
         */
        struct timeval tv;
        tv.tv_sec  = 0;
        tv.tv_usec = 2000; /* 2ms */
        (void)select(0, NULL, NULL, NULL, &tv);

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
    }

    psg_hw_reset();

    munmap((void*)gpio, GPIO_SIZE);
    close(fd);
    return 0;
}
