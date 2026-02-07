/*
 * psg_backend_rpi_gpio.c
 *
 * This backend assumes:
 * - Raspberry Pi 1/Zero (BCM2835): PERI_BASE = 0x20000000
 * - Raspberry Pi 2/3 (BCM2836/7):  PERI_BASE = 0x3F000000
 * - Raspberry Pi 4 (BCM2711):      PERI_BASE = 0xFE000000
 * - /dev/mem mmap GPIO and CM
 * - Wiring (BC2=H fixed, A8=H A9=L fixed):
 *     GPIO20..27 -> DA0..7 (LSB=GPIO20)
 *     GPIO12     -> BDIR
 *     GPIO13     -> BC1
 *     GPIO17     -> RESET (active-high)
 *     GPIO4      -> 2.000 MHz or 1.9968 MHz clock for YM2149F
 *
 * - Note BOARD_V1 (exhibit at OSC 2026 Osaka) had:
 *     GPIO4..12 -> DA0..7 (LSB=GPIO4)
 *     GPIO12     -> BDIR
 *     GPIO13     -> BC1
 *     GPIO16     -> RESET (active-high)
 *     No GPIO clock (using 2.000 MHz oscillator)
 */

#include <sys/types.h>
#include <sys/mman.h>
#include <sys/sysctl.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "psg_backend_rpi_gpio.h"
#include "ym2149f.h"

/* ---- BCM2835 (Raspberry Pi Zero/1) fixed addresses ---- */
#define PERI_BASE_BCM2835   0x20000000u

/* ---- BCM2836/7 (Raspberry Pi 2/3) fixed addresses ---- */
#define PERI_BASE_BCM2836   0x3F000000u

/* ---- BCM2711 (Raspberry Pi 4) fixed addresses ---- */
#define PERI_BASE_BCM2711   0xFE000000u

#define GPIO_OFFSET 0x00200000u
#define GPIO_SIZE   0x1000u

/* GPIO registers */
#define GPFSEL0     0x00
#define GPFSEL1     0x04
#define GPFSEL2     0x08
#define GPSET0      0x1c
#define GPCLR0      0x28

/* Clock Manager (CM) registers */
#define CM_OFFSET   0x00101000u
#define CM_SIZE     0x1000u

#define CM_GP0CTL   0x70
#define CM_GP0DIV   0x74

#define CM_PASSWD   0x5a000000u

#define CM_CTL_MASH_SHIFT  9
#define CM_CTL_MASH_MASK   (3u << CM_CTL_MASH_SHIFT)
#define CM_CTL_FLIP        (1u << 8u)
#define CM_CTL_BUSY        (1u << 7u)
#define CM_CTL_KILL        (1u << 5u)
#define CM_CTL_ENAB        (1u << 4u)
#define CM_CTL_SRC_MASK    0x0fu

/* clock source values */
#define CM_SRC_OSC         1u
#define CM_SRC_PLLD        6u   /* PLLD (assume 500MHz) */

/* GPIO FSEL to enable Clock Manager */
#define GPIO_FSEL_INPUT  0u
#define GPIO_FSEL_OUTPUT 1u
#define GPIO_FSEL_ALT0   4u  /* 100 */
#define GPIO_FSEL_ALT5   2u  /* 010 */

/* ---- Fixed pin assignment (BCM GPIO numbering) ---- */
enum {
#ifdef BOARD_V1
    PIN_D0    =  4,  /* DA0 */
    PIN_D1    =  5,
    PIN_D2    =  6,
    PIN_D3    =  7,
    PIN_D4    =  8,
    PIN_D5    =  9,
    PIN_D6    = 10,
    PIN_D7    = 11, /* DA7 */
    PIN_BDIR  = 12,
    PIN_BC1   = 13,
    PIN_RESET = 16,
    PIN_CLOCK = 20  /* optional */
#else
    PIN_D0    = 20,  /* DA0 */
    PIN_D1    = 21,
    PIN_D2    = 22,
    PIN_D3    = 23,
    PIN_D4    = 24,
    PIN_D5    = 25,
    PIN_D6    = 26,
    PIN_D7    = 27, /* DA7 */
    PIN_BDIR  = 12,
    PIN_BC1   = 13,
    PIN_RESET = 17,
    PIN_CLOCK =  4
#endif
};

#define MASK_DATABUS   (0xFFu << PIN_D0)     /* DA0..DA7 */
#define MASK_BDIR      (1u << PIN_BDIR)
#define MASK_BC1       (1u << PIN_BC1)
#define MASK_CTRL      (MASK_BDIR | MASK_BC1)
#define MASK_RESET     (1u << PIN_RESET)

/* Wait loop: dummy reads count (tuned for AY-3-8910 worst case) */
#define NREAD_WAIT     3u

typedef struct {
    int fd;
    uint32_t peri_base;
    volatile uint32_t *gpio;
    volatile uint32_t *cm;   /* clock manager */
    int enabled;
} rpi_gpio_t;

/* Minimal memory barrier (ordering for MMIO) */
static inline void
mmio_barrier(void)
{
#if (defined(__arm__) && __ARM_ARCH >= 7) || defined(__aarch64__)
    __asm__ volatile("dmb ish" ::: "memory");
#else
    __sync_synchronize();
#endif
}

static uint32_t
detect_peri_base(void)
{
    char model[256];
    size_t len = sizeof(model);

    /* Check Raspberry Pi model strings */
    if (sysctlbyname("hw.model", model, &len, NULL, 0) != 0) {
        /* TODO: peri_base 判定ではなく、汎用の機種判定にすべき */
        /* assume Pi2/3 as default */
        return PERI_BASE_BCM2836;
    }
    model[len - 1] = '\0';

    /* model strings are taken from dts files */
    if (strstr(model, "raspberrypi,model-a") != NULL ||
        strstr(model, "raspberrypi,model-b") != NULL ||
        strstr(model, "raspberrypi,model-zero") != NULL) {
        return PERI_BASE_BCM2835;
    }
    if (strstr(model, "raspberrypi,2-model") != NULL ||
        strstr(model, "raspberrypi,3-model") != NULL ||
        strstr(model, "raspberrypi,3-compute") != NULL) {
        return PERI_BASE_BCM2836;
    }
    if (strstr(model, "raspberrypi,4-model") != NULL ||
        strstr(model, "raspberrypi,400") != NULL) {
        return PERI_BASE_BCM2711;
    }

    /* default Pi2/3 */
    return PERI_BASE_BCM2836;
}

/* Set GPIO function to output: fsel=001 */
static void
gpio_config_output(rpi_gpio_t *rg, int pin)
{
    uint32_t reg = pin / 10;          /* each GPFSEL covers 10 pins */
    uint32_t shift = (pin % 10) * 3;
    volatile uint32_t *fsel = &rg->gpio[GPFSEL0 / 4 + reg];

    uint32_t v = *fsel;
    v &= ~(7u << shift);
    v |=  (1u << shift); /* 001 = output */
    *fsel = v;
    mmio_barrier();
}

/* Set GPIO function to output: fsel=001 */
static void
gpio_config_alt(rpi_gpio_t *rg, int pin, uint32_t fsel_bits)
{
    uint32_t reg = pin / 10;          /* each GPFSEL covers 10 pins */
    uint32_t shift = (pin % 10) * 3;
    volatile uint32_t *fsel = &rg->gpio[GPFSEL0 / 4 + reg];

    uint32_t v = *fsel;
    v &= ~(7u << shift);
    v |= (fsel_bits << shift);
    *fsel = v;
    mmio_barrier();
}

static inline void
cm_wait_not_busy(volatile uint32_t *cm_ctl)
{
    for (int i = 0; i < 10000; i++) {
        if ((*cm_ctl & CM_CTL_BUSY) == 0)
            return;
    }
}

static int
rpi_gpclk0_set_hz(rpi_gpio_t *rg, uint32_t hz, uint32_t src, uint32_t mash)
{
    volatile uint32_t *ctl = &rg->cm[CM_GP0CTL / 4];
    volatile uint32_t *div = &rg->cm[CM_GP0DIV / 4];

    /* 1) disable */
    *ctl = CM_PASSWD | (*ctl & ~CM_CTL_ENAB);
    mmio_barrier();
    cm_wait_not_busy(ctl);

    /* 2) choose divisor */
    /*    assume PLLD is 500MHz (XXX: Pi4 has 750MHz?) */
    uint32_t divi = 0, divf = 0;

    if (hz == 2000000u) {
        divi = 250;
        divf = 0;
        mash = 0; /* integer divider */
    } else if (hz == 1996800u) {
        divi = 250;
        divf = 1641;
        mash = 1; /* fractional divider */
    } else {
        return 0;
    }
    *div = CM_PASSWD | ((divi & 0x0fffu) << 12u) | (divf & 0x0fffu);
    mmio_barrier();

    /* 3) enable with src+mash */
    uint32_t ctlv = 0;
    ctlv |= (src & CM_CTL_SRC_MASK);
    ctlv |= ((mash & 3u) << CM_CTL_MASH_SHIFT);
    ctlv |= CM_CTL_ENAB;

    *ctl = CM_PASSWD | ctlv;
    mmio_barrier();

    return 1;
}

static int
rpi_gpio_clock_enable(rpi_gpio_t *rg, int clock_pin, uint32_t clock_hz)
{
#ifdef BOARD_V1
    /* GPIO20: ALT5=GPCLK0 (to avoid conflict with DA0 on GPIO4) */
    if (clock_pin == 20) {
        gpio_config_alt(rg, 20, GPIO_FSEL_ALT5);
        return rpi_gpclk0_set_hz(rg, clock_hz, CM_SRC_PLLD, 1);
    }
#else
    /* GPIO4: ALT0=GPCLK0 */
    if (clock_pin == 4) {
        gpio_config_alt(rg, 4, GPIO_FSEL_ALT0);
        return rpi_gpclk0_set_hz(rg, clock_hz, CM_SRC_PLLD, 1);
    }
#endif

    return 0;
}

static void
rpi_gpio_clock_disable(rpi_gpio_t *rg)
{
    volatile uint32_t *ctl = &rg->cm[CM_GP0CTL / 4];

    /* disable */
    *ctl = CM_PASSWD | (*ctl & ~CM_CTL_ENAB);
    mmio_barrier();
    cm_wait_not_busy(ctl);
}

static void
gpio_config(rpi_gpio_t *rg)
{
    /* configure pins as output */
    for (int pin = PIN_D0; pin <= PIN_D7; pin++)
        gpio_config_output(rg, pin);
    gpio_config_output(rg, PIN_BDIR);
    gpio_config_output(rg, PIN_BC1);
    gpio_config_output(rg, PIN_RESET);
}

/* Write multiple pins at once: set_mask bits become 1, clr_mask bits become 0 */
static inline void
gpio_write_masks(rpi_gpio_t *rg, uint32_t set_mask, uint32_t clr_mask)
{
    volatile uint32_t *gpio = rg->gpio;
    if (clr_mask)
        gpio[GPCLR0 / 4] = clr_mask;
    if (set_mask)
        gpio[GPSET0 / 4] = set_mask;
    mmio_barrier();
}

/* Dummy GPIO register reads for wait by I/O */
static inline void
gpio_wait(rpi_gpio_t *rg)
{
    volatile uint32_t *gpio = rg->gpio;
    volatile uint32_t dummy;
    for (unsigned int i = 0; i < NREAD_WAIT; i++) {
        dummy = gpio[GPCLR0 / 4];
        (void)dummy;
        dummy = gpio[GPSET0 / 4];
        (void)dummy;
    }
    mmio_barrier();
}

/* Put value on data bus GPIOs in one operation (2 stores: clear then set) */
static inline void
bus_write8(rpi_gpio_t *rg, uint8_t v)
{
    uint32_t setm = ((uint32_t)v << PIN_D0) & MASK_DATABUS;
    uint32_t clrm = MASK_DATABUS & ~setm;
    gpio_write_masks(rg, setm, clrm);
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
ctrl_inactive(rpi_gpio_t *rg)
{
    gpio_write_masks(rg, 0, MASK_CTRL);
}

static inline void
ctrl_latch_addr(rpi_gpio_t *rg)
{
    /* set both in one shot (no intermediate state) */
    gpio_write_masks(rg, MASK_CTRL, 0);
}

static inline void
ctrl_write_data(rpi_gpio_t *rg)
{
    /*
     * Ensure BC1=0 first; then set BDIR=1 (can be two stores but very tight).
     * If we always go through inactive before write, we can do just "set BDIR".
     */
    gpio_write_masks(rg, MASK_BDIR, MASK_BC1);
}

static void
ym_reset_pulse(rpi_gpio_t *rg)
{
    /* RESETはアクティブHigh想定 (I/F回路はオープンコレクタTr経由で駆動) */
    gpio_write_masks(rg, 0, MASK_RESET);      /* deassert = 0 */
    usleep(10);
    gpio_write_masks(rg, MASK_RESET, 0);      /* assert = 1 */
    usleep(1000);
    gpio_write_masks(rg, 0, MASK_RESET);      /* deassert = 0 */
    usleep(1000);
}

static void
ym_latch_addr(rpi_gpio_t *rg, uint8_t reg)
{
    bus_write8(rg, reg & 0x0f);
    ctrl_latch_addr(rg);
    /* Wait address setup time: 300 ns on YM2149F, 400 ns on AY-3-8910 */
    gpio_wait(rg);
    ctrl_inactive(rg);
}

static void
ym_write_data(rpi_gpio_t *rg, uint8_t data)
{
    bus_write8(rg, data);
    /* recommended: always start from inactive so only BDIR needs to be raised */
    ctrl_inactive(rg);
    ctrl_write_data(rg);
    /* Wait write signal time: 300 ns on YM2149F, 500 ns on AY-3-8910 */
    gpio_wait(rg);
    ctrl_inactive(rg);
}

static void
ym_write_reg_raw(rpi_gpio_t *rg, uint8_t reg, uint8_t val)
{
    ym_latch_addr(rg, reg);
    ym_write_data(rg, val);
}

/* ---- backend ops ---- */

static int
rpi_gpio_init(psg_backend_t *psgbe)
{
    if (psgbe == NULL)
        return 0;

    /* エラーメッセージ初期化 */
    psgbe->last_error[0] = '\0';

    rpi_gpio_t *rg = calloc(1, sizeof(*rg));
    if (rg == NULL) {
        snprintf(psgbe->last_error, PSG_BACKEND_LAST_ERROR_MAXLEN,
            "calloc(ctx): out of memory");
        return 0;
    }

    rg->fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (rg->fd == -1) {
        snprintf(psgbe->last_error, PSG_BACKEND_LAST_ERROR_MAXLEN,
            "open(/dev/mem): %s", strerror(errno));
        free(rg);
        return 0;
    }

    rg->peri_base = detect_peri_base();

    void *p = mmap(NULL, GPIO_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED,
                   rg->fd, rg->peri_base + GPIO_OFFSET);
    if (p == MAP_FAILED) {
        snprintf(psgbe->last_error, PSG_BACKEND_LAST_ERROR_MAXLEN,
            "mmap(GPIO @0x%08x): %s",
            (unsigned int)(rg->peri_base + GPIO_OFFSET), strerror(errno));
        close(rg->fd);
        free(rg);
        return 0;
    }
    rg->gpio = (volatile uint32_t *)p;

    gpio_config(rg);

    void *cm = mmap(NULL, CM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED,
                   rg->fd, rg->peri_base + CM_OFFSET);
    if (p == MAP_FAILED) {
        snprintf(psgbe->last_error, PSG_BACKEND_LAST_ERROR_MAXLEN,
            "mmap(CM @0x%08x): %s",
            (unsigned int)(rg->peri_base + CM_OFFSET), strerror(errno));
        munmap((void *)rg->gpio, GPIO_SIZE);
        close(rg->fd);
        free(rg);
        return 0;
    }
    rg->cm = (volatile uint32_t *)cm;

    /* enable clock by CM */
    uint32_t psgclock = 2000000;
    rpi_gpio_clock_enable(rg, PIN_CLOCK, psgclock);

    /* Safe default: inactive bus, deassert reset, clear data bus */
    ctrl_inactive(rg);
    bus_write8(rg, 0x00);
    gpio_write_masks(rg, 0, MASK_RESET);

    rg->enabled = 0;
    psgbe->ctx = rg;
    return 1;
}

static void
rpi_gpio_fini(psg_backend_t *psgbe)
{
    if (psgbe == NULL || psgbe->ctx == NULL)
        return;

    rpi_gpio_t *rg = psgbe->ctx;

    ctrl_inactive(rg);
    gpio_write_masks(rg, 0, MASK_RESET);

    /* disable clock by CM */
    rpi_gpio_clock_disable(rg);

    if (rg->cm != NULL)
        munmap((void *)rg->cm, CM_SIZE);
    if (rg->gpio != NULL)
        munmap((void *)rg->gpio, GPIO_SIZE);
    if (rg->fd != -1)
        close(rg->fd);

    free(rg);
    psgbe->ctx = NULL;
}

static int
rpi_gpio_enable(psg_backend_t *psgbe)
{
    if (psgbe == NULL)
        return 0;

    if (psgbe->ctx == NULL) {
        snprintf(psgbe->last_error, PSG_BACKEND_LAST_ERROR_MAXLEN,
            "enable: ctx is NULL (not initialized?)");
        return 0;
    }

    rpi_gpio_t *rg = psgbe->ctx;

    /* GPIO の場合 init で初期化済みなので何もしない */
    rg->enabled = 1;
    return 1;
}

static void
rpi_gpio_disable(psg_backend_t *psgbe)
{
    if (psgbe == NULL || psgbe->ctx == NULL)
        return;

    rpi_gpio_t *rg = psgbe->ctx;

    if (rg->enabled != 0) {
        /* サウンド出力を止める */
        ym_write_reg_raw(rg, AY_ENABLE, 0x3f);
        ym_write_reg_raw(rg, AY_AVOL, 0x00);
        ym_write_reg_raw(rg, AY_BVOL, 0x00);
        ym_write_reg_raw(rg, AY_CVOL, 0x00);
    }

    ctrl_inactive(rg);
    rg->enabled = 0;
}

static int
rpi_gpio_reset(psg_backend_t *psgbe)
{
    if (psgbe == NULL)
        return 0;

    if (psgbe->ctx == NULL) {
        snprintf(psgbe->last_error, PSG_BACKEND_LAST_ERROR_MAXLEN,
            "reset: ctx is NULL (not initialized?)");
        return 0;
    }

    rpi_gpio_t *rg = psgbe->ctx;
    if (rg->enabled == 0) {
        snprintf(psgbe->last_error, PSG_BACKEND_LAST_ERROR_MAXLEN,
            "reset: backend is disabled");
        return 0;
    }

    ctrl_inactive(rg);
    bus_write8(rg, 0x00);
    ym_reset_pulse(rg);

    return 1;
}

static int
rpi_gpio_write_reg(psg_backend_t *psgbe, uint8_t reg, uint8_t val)
{
    if (psgbe == NULL)
        return 0;

    if (psgbe->ctx == NULL) {
        snprintf(psgbe->last_error, PSG_BACKEND_LAST_ERROR_MAXLEN,
            "write_reg: ctx is NULL (not initialized?)");
        return 0;
    }

    rpi_gpio_t *rg = psgbe->ctx;
    if (rg->enabled == 0) {
        snprintf(psgbe->last_error, PSG_BACKEND_LAST_ERROR_MAXLEN,
            "write_reg: backend is disabled");
        return 0;
    }

    ym_write_reg_raw(rg, reg, val);
    return 1;
}

void
psg_backend_rpi_gpio_bind(psg_backend_ops_t *ops)
{
    memset(ops, 0, sizeof(*ops));
    ops->id        = "rpi-gpio";
    ops->init      = rpi_gpio_init;
    ops->fini      = rpi_gpio_fini;
    ops->enable    = rpi_gpio_enable;
    ops->disable   = rpi_gpio_disable;
    ops->reset     = rpi_gpio_reset;
    ops->write_reg = rpi_gpio_write_reg;
}
