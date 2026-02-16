/*
 * psg_gpio_mmap_test.c
 *  Minimal YM2149 (AY-3-8910 compatible) write-only test
 *  using GPIO via /dev/mem mmap(2) on Raspberry Pi 3B
 *
 * Wiring (BC2=H fixed, A8=H A9=L fixed):
 *   GPIO4..11 -> DA0..7 (LSB=GPIO4)
 *   GPIO12    -> BDIR
 *   GPIO13    -> BC1
 *   GPIO16    -> RESET (active-high)
 *
 * Build:
 *   cc -O2 -Wall -Wextra -o psg_gpio_mmap_test psg_gpio_mmap_test.c
 *
 * Run (root recommended for /dev/mem accesses):
 *   ./psg_gpio_test            (default clock 2MHz, play C/E/G for 5s)
 *   ./psg_gpio_test -c 2000000 -t 10
 */

#include <sys/types.h>
#include <sys/mman.h>

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

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

/* AY/YM2149 register numbers */
enum {
    AY_AFINE    = 0,
    AY_ACOARSE  = 1,
    AY_BFINE    = 2,
    AY_BCOARSE  = 3,
    AY_CFINE    = 4,
    AY_CCOARSE  = 5,
    AY_NOISEPER = 6,
    AY_ENABLE   = 7,
    AY_AVOL     = 8,
    AY_BVOL     = 9,
    AY_CVOL     = 10,
    AY_EFINE    = 11,
    AY_ECOARSE  = 12,
    AY_ESHAPE   = 13,
    AY_PORTA    = 14,
    AY_PORTB    = 15
};

/* Masks */
#define MASK_DATABUS   (0xFFu << PIN_D0)              /* GPIO4..11 */
#define MASK_BDIR      (1u << PIN_BDIR)
#define MASK_BC1       (1u << PIN_BC1)
#define MASK_CTRL      (MASK_BDIR | MASK_BC1)
#define MASK_RESET     (1u << PIN_RESET)

static volatile uint32_t *gpio;

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
    /* assume active-high reset */
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
    /* timing is extremely relaxed compared to chip requirements */
    sleep_us(1);
    ctrl_inactive();
}

static void
ym_write_data(uint8_t data)
{
    bus_write8(data);
    ctrl_inactive();
    /* recommended: always start from inactive so only BDIR needs to be raised */
    sleep_us(1);
    ctrl_write_data();
    sleep_us(1);
    ctrl_inactive();
}

static void
ym_write_reg(uint8_t reg, uint8_t val)
{
    ym_latch_addr(reg);
    ym_write_data(val);
}

static uint16_t
tone_period_from_freq(uint32_t clock_hz, double freq_hz)
{
    /*
     * AY/YM tone frequency:
     *   f_out = clock / (16 * period)
     * => period = clock / (16 * f_out)
     *
     * period is 12-bit (0..4095). period=0 is treated as 1 on many implementations.
     */
    if (freq_hz <= 0.0) 
        return 1;

    double p = (double)clock_hz / (16.0 * freq_hz);
    if (p < 1.0)
        p = 1.0;
    if (p > 4095.0)
        p = 4095.0;

    return (uint16_t)(p + 0.5);
}

static void
set_tone_A(int fd, uint16_t period)
{
    ym_write_reg(AY_AFINE,   (uint8_t)(period & 0xff));
    ym_write_reg(AY_ACOARSE, (uint8_t)((period >> 8) & 0x0f));
}

static void
set_tone_B(int fd, uint16_t period)
{
    ym_write_reg(AY_BFINE,   (uint8_t)(period & 0xff));
    ym_write_reg(AY_BCOARSE, (uint8_t)((period >> 8) & 0x0f));
}

static void
set_tone_C(int fd, uint16_t period)
{
    ym_write_reg(AY_CFINE,   (uint8_t)(period & 0xff));
    ym_write_reg(AY_CCOARSE, (uint8_t)((period >> 8) & 0x0f));
}

static void
usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [-c clock_hz] [-t seconds]\n"
        "  default: clock=2000000Hz, seconds=5\n", prog);
    exit(2);
}

int
main(int argc, char **argv)
{
    const char *dev = "/dev/mem";
    uint32_t clock_hz = 2000000;  /* adjust to your oscillator */
    int play_seconds = 5;

    int ch;
    while ((ch = getopt(argc, argv, "c:t:h")) != -1) {
        switch (ch) {
        case 'c':
            clock_hz = (uint32_t)strtoul(optarg, NULL, 0);
            break;
        case 't':
            play_seconds = atoi(optarg);
            break;
        default:
            usage(argv[0]);
        }
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

    /* idle and reset */
    ctrl_inactive();
    bus_write8(0x00);
    ym_reset_pulse();

    /* Basic init: disable noise, enable tones, set volumes */
    ym_write_reg(AY_ENABLE, 0x38); /* 0011 1000b: tone enabled (bits0-2=0), noise disabled (bits3-5=1) */
    ym_write_reg(AY_NOISEPER, 0x00);

    /* Set a simple chord (C4/E4/G4) */
    uint16_t pA = tone_period_from_freq(clock_hz, 261.6256);
    uint16_t pB = tone_period_from_freq(clock_hz, 329.6276);
    uint16_t pC = tone_period_from_freq(clock_hz, 391.9954);

    set_tone_A(fd, pA);
    set_tone_B(fd, pB);
    set_tone_C(fd, pC);

    ym_write_reg(AY_AVOL, 0x0f);
    ym_write_reg(AY_BVOL, 0x0f);
    ym_write_reg(AY_CVOL, 0x0f);

    fprintf(stderr, "Playing C/E/G for %d seconds (clock=%" PRIu32 " Hz)\n",
        play_seconds, clock_hz);

    sleep(play_seconds);

    ym_write_reg(AY_AVOL, 0);
    ym_write_reg(AY_BVOL, 0);
    ym_write_reg(AY_CVOL, 0);

    ctrl_inactive();
    bus_write8(0);
    ym_reset_pulse();

    munmap((void*)gpio, GPIO_SIZE);
    close(fd);
    return 0;
}
