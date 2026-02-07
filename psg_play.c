/*
 * psg_play.c
 *  Minimal YM2149 (AY-3-8910 compatible) player
 */

#include <sys/select.h>

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "p6psg.h"
#include "psg_driver.h"
#include "player_ui.h"
#include "psg_backend.h"
#include "psg_backend_rpi_gpio.h"

static volatile sig_atomic_t g_stop = 0;
static volatile sig_atomic_t g_redraw = 0;

static void
on_signal(int signo)
{
    (void)signo;
    g_stop = 1;
}

/* --- timing helpers --- */
static inline uint64_t
nsec_now_monotonic(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

typedef struct psgio {
    psg_backend_t *psgbe;
    UI_state *ui;
} psgio_t;

static void
psg_write_reg_cb(void *opaque, uint8_t reg, uint8_t val)
{
    psgio_t *psgio = opaque;
    psg_backend_t *psgbe = psgio->psgbe;
    UI_state *ui = psgio->ui;
    (void)(*psgbe->ops->write_reg)(psgbe, reg, val);
    ui_on_reg_write(ui, reg, val);
}

static void
ui_note_event_cb(void *opaque, int ch, uint8_t octave, uint8_t note,
                 uint8_t volume, uint16_t len, uint8_t is_rest,
                 uint16_t bpm_x10)
{
    psgio_t *psgio = opaque;
    UI_state *ui = psgio->ui;
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
    const char *title = NULL;
    p6psg_t *p6psg = NULL;
    p6psg_channel_dataset_t channels;
    psgio_t psgiostore, *psgio;
    psg_backend_ops_t ops_store, *ops;
    psg_backend_t psgbe_store, *psgbe;
    int backend_inited = 0;
    int backend_enabled = 0;
    PSGDriver psgdriver, *drv;
    UI_state uistate, *ui;
    int status = EXIT_SUCCESS;

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

    p6psg = p6psg_create();
    if (p6psg == NULL) {
        fprintf(stderr, "p6psg: out of memory\n");
        status = EXIT_FAILURE;
        goto out;
    }

    if (p6psg_load(p6psg, ifname, &channels) == 0) {
        fprintf(stderr, "%s: %s\n", ifname, p6psg_last_error(p6psg));
        status = EXIT_FAILURE;
        goto out;
    }

    /* ---- YM2149 backend bind/init/enable/reset ---- */
    psgio = &psgiostore;

    ops = &ops_store;
    memset(ops, 0, sizeof(*ops));
    psg_backend_rpi_gpio_bind(ops);
    if (ops->id == NULL) {
        fprintf(stderr, "failed to bind backend\n");
        status = EXIT_FAILURE;
        goto out;
    }

    psgbe = &psgbe_store;
    memset(psgbe, 0, sizeof(*psgbe));
    psgbe->ops = ops;

    if ((*psgbe->ops->init)(psgbe) == 0) {
        fprintf(stderr, "failed to init backend: %s\n", psgbe->ops->id);
        status = EXIT_FAILURE;
        goto out;
    }
    psgio->psgbe = psgbe;
    backend_inited = 1;

    if ((*psgbe->ops->enable)(psgbe) == 0) {
        fprintf(stderr, "failed to enable backend: %s\n", psgbe->ops->id);
        status = EXIT_FAILURE;
        goto out;
    }
    backend_enabled = 1;

    ui = &uistate;
    int ui_active = 0;
    uint64_t now0 = nsec_now_monotonic();
    ui_init(ui, now0);
    psgio->ui = ui;
    ui_active = 1;

    drv = &psgdriver;
    psg_driver_init(drv, psg_write_reg_cb, ui_note_event_cb, psgio);
    psg_driver_set_channel_data(drv, P6PSG_CH_A, channels.ch[P6PSG_CH_A].ptr);
    psg_driver_set_channel_data(drv, P6PSG_CH_B, channels.ch[P6PSG_CH_B].ptr);
    psg_driver_set_channel_data(drv, P6PSG_CH_C, channels.ch[P6PSG_CH_C].ptr);
    psg_driver_start(drv);

    /*
     * main player loop
     */

    /* call PSG driver every 2ms */
    const uint64_t tick_ns = 2000000ull;
    uint64_t t0 = nsec_now_monotonic();
    uint64_t next_deadline = t0 + tick_ns;

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
            ui_request_redraw(ui);;
            g_redraw = 0;
        }
        ui_maybe_render(ui, now, title != NULL ? title : "OSC demo");
    }

    psg_driver_stop(drv);

    if (ui_active)
        ui_shutdown(ui);

 out:
    if (backend_enabled) {
        (*psgbe->ops->disable)(psgbe);
        backend_enabled = 0;
    }

    if (backend_inited) {
        (*psgbe->ops->fini)(psgbe);
        backend_inited = 0;
    }

    p6psg_destroy(p6psg);
    exit(status);
}
