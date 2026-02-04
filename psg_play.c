/*
 * psg_play.c
 *  Minimal YM2149 (AY-3-8910 compatible) player
 */

#include <sys/select.h>

#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

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

static void
die(const char *msg)
{
    perror(msg);
    exit(1);
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

    free(psgdata);
    exit(status);
}
