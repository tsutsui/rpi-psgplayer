/*
 * player_ui.h
 *  API definitions for demonstration screen layout etc.
 */

#include <termios.h>
#include <stdint.h>
#include <stddef.h>

#define UI_ROWS 23
#define UI_COLS 79   /* visible columns, excluding '\0' */

/* output buffer capacity (per render) */
#define UI_OUT_CAP 8192

/* ---- UI fixed field widths (template dependent) ---- */
#define UI_W_TITLE   38  /* underscores in template */
#define UI_W_BPM     5   /* "___._" */
#define UI_W_TSEC    7   /* "_____._" */

#define UI_W_NOTE    3   /* "E4 " or "C#4" */
#define UI_W_HZ      6   /* " 329.6" or " -----" */
#define UI_W_VOLN    2   /* "10" */
#define UI_W_BAR     15  /* [...............] */

/* NOTE: caches are ASCII/UTF-8 byte strings padded to fixed width */

typedef struct {
    uint64_t t_ns;     /* when this note/rest was issued */
    uint8_t octave;    /* as-is from driver (O0..O8 etc.) */
    uint8_t note;      /* 0=rest, 1..12 */
    uint8_t volume;    /* 0..15 */
    uint16_t len;      /* ticks/units as you set wait_counter */
    uint8_t is_rest;   /* current note is rest */
} UI_music_ch;

typedef struct {
    /* music state per channel (from driver) */
    UI_music_ch mus[3];

    /* music tempo in bpm x10 (from driver) */
    uint16_t bpm_x10;

    /* minimal reg shadow for mixer/noise display */
    uint8_t reg[16];
    uint8_t noise_period;      /* reg[6] & 0x1f */
    uint8_t tone_enable[3];    /* from reg[7] */
    uint8_t noise_enable[3];   /* from reg[7] */

    /* ui timing */
    uint64_t start_ns;
    uint64_t next_ui_ns;
    uint64_t ui_period_ns;

    int initialized;

    /* screen output buffer */
    char   out_buf[UI_OUT_CAP];
    size_t out_len;

    struct termios tio_saved;
    int tio_saved_valid;
    int cursor_hidden;
    int wrap_disabled;

    /* --- incremental field caches --- */
    int template_drawn;
    int redraw;

    /* fixed-width cached strings (byte-wise compare) */
    char cache_title[UI_W_TITLE * 4 + 1]; /* UTF-8 can be wider in bytes */
    char cache_bpm[UI_W_BPM + 1];
    char cache_tsec[UI_W_TSEC + 1];

    char cache_note[3][UI_W_NOTE + 1];
    char cache_hz[3][UI_W_HZ + 1];
    char cache_voln[3][UI_W_VOLN + 1];
    char cache_bar[3][UI_W_BAR + 1];
    char cache_tone[3][4];   /* "ON " / "OFF" */
    char cache_noise[3][4];  /* "ON " / "OFF" */

    /* piano marker: remember last plotted column; -1 means none */
    int  cache_piano_x[3];
    char cache_piano_mark[3]; /* last mark char */

    /* register cached values (write only when changed) */
    uint8_t cache_reg[16];
    int     cache_reg_valid;
} UI_state;

/* called from register write path */
void ui_on_reg_write(UI_state *ui, uint8_t reg, uint8_t val);

/* called from driver when a note/rest is committed */
void ui_on_note_event(UI_state *ui, uint64_t now_ns, int ch,
                         uint8_t octave, uint8_t note,
                         uint8_t volume, uint16_t len, uint8_t is_rest,
                         uint16_t bpm_x10);

/* ANSI UI init/shutdown */
void ui_init(UI_state *ui, uint64_t now_ns);
void ui_shutdown(UI_state *ui);

/* UI rendering */
void ui_maybe_render(UI_state *ui, uint64_t now_ns, const char *title);

/* request a redraw on next render */
void ui_request_redraw(UI_state *ui);