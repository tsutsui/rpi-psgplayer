/*
 * psg_backend_rpi_gpio.c
 *
 * This backend assumes:
 * - Raspberry Pi 3B (BCM2837): PERI_BASE = 0x3F000000
 * - /dev/mem mmap GPIO
 * - Wiring (BC2=H fixed, A8=H A9=L fixed):
 *     GPIO4..11 -> DA0..7 (LSB=GPIO4)
 *     GPIO12    -> BDIR
 *     GPIO13    -> BC1
 *     GPIO16    -> RESET (active-high)
 */

#include "psg_backend_rpi_gpio.h"
#include "ym2149f.h"

#include <sys/types.h>
#include <sys/mman.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ---- BCM2837 (Raspberry Pi 2/3) fixed addresses ---- */
#define PERI_BASE   0x3F000000u
#define GPIO_BASE   (PERI_BASE + 0x200000u)
#define GPIO_SIZE   0x1000u

/* GPIO registers */
#define GPFSEL0     0x00
#define GPFSEL1     0x04
#define GPFSEL2     0x08
#define GPSET0      0x1c
#define GPCLR0      0x28

/* ---- Fixed pin assignment (BCM GPIO numbering) ---- */
enum {
    PIN_D0    = 4,  /* DA0 */
    PIN_D1    = 5,
    PIN_D2    = 6,
    PIN_D3    = 7,
    PIN_D4    = 8,
    PIN_D5    = 9,
    PIN_D6    = 10,
    PIN_D7    = 11, /* DA7 */
    PIN_BDIR  = 12,
    PIN_BC1   = 13,
    PIN_RESET = 16
};

#define MASK_DATABUS   (0xFFu << PIN_D0)     /* GPIO4..11 */
#define MASK_BDIR      (1u << PIN_BDIR)
#define MASK_BC1       (1u << PIN_BC1)
#define MASK_CTRL      (MASK_BDIR | MASK_BC1)
#define MASK_RESET     (1u << PIN_RESET)

/* Wait loop: dummy reads count (tuned for AY-3-8910 worst case) */
#define NREAD_WAIT     3u

typedef struct {
    int fd;
    volatile uint32_t *gpio;
    int enabled;
} rpi_gpio_t;

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

/* Put value on data bus GPIO4..11 in one operation (2 stores: clear then set) */
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

    rpi_gpio_t *rg = calloc(1, sizeof(*rg));
    if (rg == NULL) {
        perror("malloc(ctx)");
        return 0;
    }

    rg->fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (rg->fd == -1) {
        perror("open(/dev/mem)");
        free(rg);
        return 0;
    }

    void *p = mmap(NULL, GPIO_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED,
                   rg->fd, GPIO_BASE);
    if (p == MAP_FAILED) {
        perror("mmap(GPIO)");
        close(rg->fd);
        free(rg);
        return 0;
    }
    rg->gpio = (volatile uint32_t *)p;

    gpio_config(rg);

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
    if (psgbe == NULL || psgbe->ctx == NULL)
        return 0;

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
    if (psgbe == NULL || psgbe->ctx == NULL)
        return 0;

    rpi_gpio_t *rg = psgbe->ctx;
    if (rg->enabled == 0)
        return 0;

    ctrl_inactive(rg);
    bus_write8(rg, 0x00);
    ym_reset_pulse(rg);

    return 1;
}

static int
rpi_gpio_write_reg(psg_backend_t *psgbe, uint8_t reg, uint8_t val)
{
    if (psgbe == NULL || psgbe->ctx == NULL)
        return 0;

    rpi_gpio_t *rg = psgbe->ctx;
    if (rg->enabled == 0)
        return 0;

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
