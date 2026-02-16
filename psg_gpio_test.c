/*
 * psg_gpio_test.c
 *  Minimal YM2149 (AY-3-8910 compatible) write-only test
 *  via NetBSD gpio(4) /dev/gpio0 on Raspberry Pi 3B.
 *
 * Wiring (BC2=H fixed, A8=H A9=L fixed):
 *   GPIO4..11 -> DA0..7 (LSB=GPIO4)
 *   GPIO12    -> BDIR
 *   GPIO13    -> BC1
 *   GPIO16    -> RESET (active-high)
 *
 * Build:
 *   cc -O2 -Wall -Wextra -o psg_gpio_test psg_gpio_test.c
 *
 * Run (root recommended):
 *   ./psg_gpio_test            (default clock 2MHz, play C/E/G for 5s)
 *   ./psg_gpio_test -c 2000000 -t 10
 */

#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/gpio.h>

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifndef GPIO_PIN_LOW
#define GPIO_PIN_LOW  0
#define GPIO_PIN_HIGH 1
#endif

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

/* Wrapper to configure a pin via GPIOSET */
static void
gpio_config_output(int fd, int pin, const char *name_opt)
{
#if 0    /* GPIO configuration is done by /etc/gpio.conf */
    struct gpio_set gs;
    memset(&gs, 0, sizeof(gs));
    gs.gp_pin = pin;
    gs.gp_flags = GPIO_PIN_OUTPUT;

    if (name_opt != NULL) {
        strlcpy(gs.gp_name2, name_opt, sizeof(gs.gp_name2));
    }

    if (ioctl(fd, GPIOSET, &gs) == -1) {
        /* Common failure modes: not root, securelevel restrictions, pin locked, etc. */
        die("ioctl(GPIOSET)");
    }
#endif
}
static void
gpio_config(int fd)
{
    /* Configure pins as outputs (names are optional but can help with securelevel policy). */
    gpio_config_output(fd, PIN_D0,   "psg_d0");
    gpio_config_output(fd, PIN_D1,   "psg_d1");
    gpio_config_output(fd, PIN_D2,   "psg_d2");
    gpio_config_output(fd, PIN_D3,   "psg_d3");
    gpio_config_output(fd, PIN_D4,   "psg_d4");
    gpio_config_output(fd, PIN_D5,   "psg_d5");
    gpio_config_output(fd, PIN_D6,   "psg_d6");
    gpio_config_output(fd, PIN_D7,   "psg_d7");
    gpio_config_output(fd, PIN_BDIR, "psg_bdir");
    gpio_config_output(fd, PIN_BC1,  "psg_bc1");
    gpio_config_output(fd, PIN_RESET,"psg_reset");
}

/* Wrapper to write a pin via GPIOWRITE */
static void
gpio_write(int fd, int pin, int value)
{
    struct gpio_req gr;
    memset(&gr, 0, sizeof(gr));
    gr.gp_pin = pin;
    gr.gp_value = value ? GPIO_PIN_HIGH : GPIO_PIN_LOW;

    if (ioctl(fd, GPIOWRITE, &gr) == -1) {
        die("ioctl(GPIOWRITE)");
    }
}

/* Data bus: DA0..7 on GPIO4..11, LSB=GPIO4 */
static void
bus_write8(int fd, uint8_t v)
{
    /*
     * This is intentionally simple (8 ioctls),
     * but just for a first hardware smoke test.
     */
    gpio_write(fd, PIN_D0, (v >> 0) & 1);
    gpio_write(fd, PIN_D1, (v >> 1) & 1);
    gpio_write(fd, PIN_D2, (v >> 2) & 1);
    gpio_write(fd, PIN_D3, (v >> 3) & 1);
    gpio_write(fd, PIN_D4, (v >> 4) & 1);
    gpio_write(fd, PIN_D5, (v >> 5) & 1);
    gpio_write(fd, PIN_D6, (v >> 6) & 1);
    gpio_write(fd, PIN_D7, (v >> 7) & 1);
}

/*
 * YM2149/AY-3-8910 bus control (BC2 fixed HIGH):
 *   BDIR BC1  Function
 *    0    0   Inactive
 *    0    1   Read  (unused here)
 *    1    0   Write (data)
 *    1    1   Latch address
 */
static inline void
ctrl_inactive(int fd)
{
    gpio_write(fd, PIN_BC1,  0);
    gpio_write(fd, PIN_BDIR, 0);
}

static inline void
ctrl_latch_addr(int fd)
{
    gpio_write(fd, PIN_BC1,  1);
    gpio_write(fd, PIN_BDIR, 1);
}

static inline void
ctrl_write_data(int fd)
{
    /*
     * Ensure BC1=0 first; then set BDIR=1 (can be two stores but very tight).
     */
    gpio_write(fd, PIN_BC1,  0);
    gpio_write(fd, PIN_BDIR, 1);
}

static void
ym_reset_pulse(int fd)
{
    /* assume active-high reset */
    gpio_write(fd, PIN_RESET, 0);         /* deassert = 0 */
    sleep_us(10);
    gpio_write(fd, PIN_RESET, 1);         /* assert = 1 */
    sleep_us(1000);
    gpio_write(fd, PIN_RESET, 0);         /* deassert = 0*/
    sleep_us(1000);
}

/* Strobes are generous because we are in userspace (ioctl overhead dwarfs chip timing). */
static void
ym_latch_addr(int fd, uint8_t reg)
{
    bus_write8(fd, reg & 0x0f);
    ctrl_latch_addr(fd);
    sleep_us(1);
    ctrl_inactive(fd);
}

static void
ym_write_data(int fd, uint8_t data)
{
    bus_write8(fd, data);
    ctrl_write_data(fd);
    sleep_us(1);
    ctrl_inactive(fd);
}

static void
ym_write_reg(int fd, uint8_t reg, uint8_t val)
{
    ym_latch_addr(fd, reg);
    ym_write_data(fd, val);
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
    ym_write_reg(fd, AY_AFINE,   (uint8_t)(period & 0xff));
    ym_write_reg(fd, AY_ACOARSE, (uint8_t)((period >> 8) & 0x0f));
}

static void
set_tone_B(int fd, uint16_t period)
{
    ym_write_reg(fd, AY_BFINE,   (uint8_t)(period & 0xff));
    ym_write_reg(fd, AY_BCOARSE, (uint8_t)((period >> 8) & 0x0f));
}

static void
set_tone_C(int fd, uint16_t period)
{
    ym_write_reg(fd, AY_CFINE,   (uint8_t)(period & 0xff));
    ym_write_reg(fd, AY_CCOARSE, (uint8_t)((period >> 8) & 0x0f));
}

static void
usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [-d /dev/gpio0] [-c clock_hz] [-t seconds]\n"
        "  default: clock=2000000Hz, seconds=5\n", prog);
    exit(2);
}

int
main(int argc, char **argv)
{
    const char *dev = "/dev/gpio0";
    uint32_t clock_hz = 2000000;  /* adjust to your oscillator */
    int play_seconds = 5;

    int ch;
    while ((ch = getopt(argc, argv, "d:c:t:h")) != -1) {
        switch (ch) {
        case 'd':
            dev = optarg;
            break;
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

    int fd = open(dev, O_RDWR);
    if (fd == -1)
        die("open(/dev/gpio)");

    gpio_config(fd);

    /* idle and reset */
    ctrl_inactive(fd);
    bus_write8(fd, 0x00);
    ym_reset_pulse(fd);

    /* Basic init: disable noise, enable tones, set volumes */
    ym_write_reg(fd, AY_ENABLE, 0x38); /* 0011 1000b: tone enabled (bits0-2=0), noise disabled (bits3-5=1) */
    ym_write_reg(fd, AY_NOISEPER, 0x00);

    /* Set a simple chord (C4/E4/G4) */
    uint16_t pA = tone_period_from_freq(clock_hz, 261.6256);
    uint16_t pB = tone_period_from_freq(clock_hz, 329.6276);
    uint16_t pC = tone_period_from_freq(clock_hz, 391.9954);

    set_tone_A(fd, pA);
    set_tone_B(fd, pB);
    set_tone_C(fd, pC);

    ym_write_reg(fd, AY_AVOL, 0x0f);
    ym_write_reg(fd, AY_BVOL, 0x0f);
    ym_write_reg(fd, AY_CVOL, 0x0f);

    fprintf(stderr, "Playing C/E/G for %d seconds (clock=%" PRIu32 " Hz)\n",
        play_seconds, clock_hz);

    sleep(play_seconds);

    /* Silence */
    ym_write_reg(fd, AY_AVOL, 0x00);
    ym_write_reg(fd, AY_BVOL, 0x00);
    ym_write_reg(fd, AY_CVOL, 0x00);

    ctrl_inactive(fd);
    bus_write8(fd, 0);
    ym_reset_pulse(fd);

    close(fd);
    return 0;
}
