/*
 * player_ui.h
 *  API definitions for demonstration screen layout etc.
 */

#include <termios.h>

#define UI_ROWS 23
#define UI_COLS 79   /* visible columns, excluding '\0' */

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

    char prev[UI_ROWS][UI_COLS + 1];
    int  have_prev;

    /* screen output buffer */
#define UI_OUT_CAP 8192
    char   out_buf[UI_OUT_CAP];
    size_t out_len;

    struct termios tio_saved;
    int tio_saved_valid;
    int cursor_hidden;
    int wrap_disabled;
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
