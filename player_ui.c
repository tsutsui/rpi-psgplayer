/*
 * player_ui.c
 *  For PSG Player demonstration on Raspberry Pi 3B at Open Source Conference 
 *  using GPIO via /dev/mem mmap(2) on Raspberry Pi 3B
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <assert.h>
#include <math.h>
#include <wchar.h>
#include <locale.h>

#include "player_ui.h"

static void
ui_update_mixer(UI_state *ui)
{
    uint8_t m = ui->reg[7];
    /* bit=1 disables */
    ui->tone_enable[0]  = ((m & 0x01) == 0);
    ui->tone_enable[1]  = ((m & 0x02) == 0);
    ui->tone_enable[2]  = ((m & 0x04) == 0);
    ui->noise_enable[0] = ((m & 0x08) == 0);
    ui->noise_enable[1] = ((m & 0x10) == 0);
    ui->noise_enable[2] = ((m & 0x20) == 0);
}

/* called from register write path */
void
ui_on_reg_write(UI_state *ui, uint8_t reg, uint8_t val)
{
    reg &= 0x0f;
    ui->reg[reg] = val;

    if (reg == 6) {
        ui->noise_period = ui->reg[6] & 0x1f;
    } else if (reg == 7) {
        ui_update_mixer(ui);
    }
}

/* called from driver when a note/rest is committed */
void
ui_on_note_event(UI_state *ui, uint64_t now_ns, int ch,
    uint8_t octave, uint8_t note, uint8_t volume,
    uint16_t len, uint8_t is_rest)
{
    if (ch < 0 || ch >= 3)
        return;
    UI_music_ch *m = &ui->mus[ch];
    m->t_ns   = now_ns;
    m->octave = octave;
    m->note   = note;
    m->volume = volume & 0x0f;
    m->len    = len;
    m->is_rest = is_rest ? 1 : 0;
}

/* ANSI UI init/shutdown */
void
ui_init(UI_state *ui, uint64_t now_ns)
{
    memset(ui, 0, sizeof(*ui));
    ui->ui_period_ns = 50ull * 1000ull * 1000ull; /* 50ms */
    ui->next_ui_ns   = now_ns;

    /* avoid stdout buffering stalls */
    setvbuf(stdout, NULL, _IONBF, 0);

    /* alternate screen + hide cursor + clear */
    fputs("\033[?1049h\033[?25l\033[H\033[J", stdout);

    ui->initialized = 1;
}

void
ui_shutdown(UI_state *ui)
{
    if (ui == NULL || ui->initialized == 0)
        return;
    /* show cursor + leave alternate screen */
    fputs("\033[?25h\033[?1049l", stdout);
    ui->initialized = 0;
}


/* ---- 固定テンプレ（79桁×23行） ---- */

static const char *ui_tmpl[UI_ROWS] = {
/* 0000000001111111111222222222233333333334444444444555555555566666666667777777777 */
/* 1234567890123456789012345678901234567890123456789012345678901234567890123456789 */
  "+-----------------------------------------------------------------------------+", /*  1 */
  "| YM2149 P6 PSG Player on Raspberry Pi 3B @ Open Source Conference Osaka 2026 |", /*  2 */
  "| Clock: 2.000 MHz, Rate: 2ms/tick, BCM2837 GPIO controlled by NetBSD/evbarm  |", /*  3 */
  "+-----------------------------------------------------------------------------+", /*  4 */
  "| Music Title: _______________________________________  t=___._s  tick=_____  |", /*  5 */
  "+-----------------------------------------------------------------------------+", /*  6 */
  "| Ch A: NOTE=--  ---.-Hz  VOL=__ [...............]  TONE=ON   NOISE=OFF       |", /*  7 */
  "| Ch B: NOTE=--  ---.-Hz  VOL=__ [...............]  TONE=ON   NOISE=OFF       |", /*  8 */
  "| Ch C: NOTE=--  ---.-Hz  VOL=__ [...............]  TONE=ON   NOISE=OFF       |", /*  9 */
  "+-+--------+-----------+-----------+-----------+-----------+-----------+------+", /* 10 */
  "| |O1      |O2         |O3         |O4         |O5         |O6         |O7    |", /* 11 */
  "|C|  # # # |# #  # # # |# #  # # # |# #  # # # |# #  # # # |# #  # # # |# #  #|", /* 12 */
  "|h|<F G A BC D EF G A BC D EF G A BC D EF G A BC D EF G A BC D EF G A BC D EF>|", /* 13 */
  "+-+--------+-----------+-----------+-----------+-----------+-----------+------+", /* 14 */
  "|A|...........................................................................|", /* 15 */
  "|B|...........................................................................|", /* 16 */
  "|C|...........................................................................|", /* 17 */
  "+-----------------------------------------------------------------------------+", /* 18 */
  "| Reg0 (Freq Fine A): --h || Reg1 (Freq Rough A) : --h || Reg8 (Level A): --h |", /* 19 */
  "| Reg2 (Freq Fine B): --h || Reg3 (Freq Rough B) : --h || Reg9 (Level B): --h |", /* 20 */
  "| Reg4 (Freq Fine C): --h || Reg5 (Freq Rough C) : --h || RegA (Level C): --h |", /* 21 */
  "| Reg6 (Freq Noise) : --h || Reg7 (Mixer Setting): --h ||                     |", /* 22 */
  "+-----------------------------------------------------------------------------+"  /* 23 */
};

/* ---- ヘルパ：安全な put ---- */

static void
line_copy_from_template(char dst[UI_COLS + 1], const char *src)
{
    /* テンプレはASCII前提で strlen==UI_COLS を要求 */
    size_t n = strlen(src);
    assert(n == UI_COLS);
    memcpy(dst, src, UI_COLS + 1); /* includes '\0' */
}

/* 0-based column; writes bytes as-is (ASCII想定のフィールドで使う) */
static void
put_bytes(char line[UI_COLS + 1], int col, const char *s)
{
    if (col < 0 || col >= UI_COLS)
        return;
    for (int i = 0; s[i] != '\0' && (col + i) < UI_COLS; i++) {
        line[col + i] = s[i];
    }
}

/* fill field with spaces */
static void
put_spaces(char line[UI_COLS + 1], int col, int width)
{
    if (width <= 0)
        return;
    if (col < 0)
        return;
    for (int i = 0; i < width && (col + i) < UI_COLS; i++)
        line[col + i] = ' ';
}

/* 2-hex + 'h' (e.g., "7Fh") */
static void
put_hex2h(char line[UI_COLS + 1], int col, uint8_t v)
{
    char b[4];
    snprintf(b, sizeof(b), "%02Xh", (unsigned)v);
    put_bytes(line, col, b);
}

/* fixed-width right-aligned integer in ASCII */
static void
put_u32_r(char line[UI_COLS + 1], int col, int width, uint32_t v)
{
    char b[32];
    snprintf(b, sizeof(b), "%*u", width, (unsigned)v);
    put_bytes(line, col, b);
}

/* fixed-width right-aligned float in ASCII */
static void
put_f1_r(char line[UI_COLS + 1], int col, int width, double x)
{
    char b[32];
    snprintf(b, sizeof(b), "%*.*f", width, 1, x);
    put_bytes(line, col, b);
}

/* VOL bar: width=15 in template */
static void
put_vol_bar(char line[UI_COLS + 1], int col, int width, uint8_t vol /*0..15*/)
{
    if (width <= 0)
        return;
    int filled = (int)((vol * (uint8_t)width + 14) / 15);
    for (int i = 0; i < width && (col + i) < UI_COLS; i++)
        line[col + i] = (i < filled) ? '#' : '.';
}

/* NOTE string from mus (ASCII only) */
static const char *
note_name_12(int note_1_12)
{
    static const char *names[13] =
        {"R","C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
    if (note_1_12 < 1 || note_1_12 > 12)
        return "??";
    return names[note_1_12];
}

static void
make_note_ascii(char out[4], uint8_t octave, uint8_t note, uint8_t is_rest)
{
    if (is_rest || note == 0) {
        /* "--" in your CH line example */
        strcpy(out, "--");
        return;
    }
    const char *nn = note_name_12(note); /* "C" or "C#" etc */
    if (nn[1] == '#') {
        /* "C#4" fits 3 */
        out[0] = nn[0];
        out[1] = '#';
        out[2] = (char)('0' + (octave % 10));
        out[3] = '\0';
    } else {
        /* "E4" fits 2 */
        out[0] = nn[0];
        out[1] = (char)('0' + (octave % 10));
        out[2] = '\0';
    }
}

/* period -> Hz (tone) */
static double
psg_period_to_hz(uint16_t period, double clock_hz)
{
    if (period == 0)
        return 0.0;
    return clock_hz / (16.0 * (double)period);
}

/* ---- UTF-8 title fit (display width) ---- */
/* You can reuse your earlier utf8_fit_cols(); included here minimal version. */

static void
utf8_fit_cols(char *dst, size_t dstsz, const char *src, int max_cols)
{
    mbstate_t st;
    memset(&st, 0, sizeof(st));

    int cols = 0;
    size_t out = 0;
    const char *p = src;

    while (*p) {
        wchar_t wc;
        size_t n = mbrtowc(&wc, p, MB_CUR_MAX, &st);
        if (n == (size_t)-1 || n == (size_t)-2) {
            memset(&st, 0, sizeof(st));
            if (cols + 1 > max_cols)
                break;
            if (out + 1 < dstsz) dst[out++] = '?';
            cols += 1;
            p += 1;
            continue;
        }
        if (n == 0)
            break;

        int w = wcwidth(wc);
        if (w < 0)
            w = 1;
        if (cols + w > max_cols)
            break;

        if (out + n < dstsz) {
            memcpy(dst + out, p, n);
            out += n;
        } else {
            break;
        }
        cols += w;
        p += n;
    }

    while (cols < max_cols && out + 1 < dstsz) {
        dst[out++] = ' ';
        cols++;
    }

    if (out < dstsz)
        dst[out] = '\0';
    else
        dst[dstsz - 1] = '\0';
}

/* ---- ピアノロール（79桁内）へのプロット ---- */
/* このテンプレのピアノ行は "|A|.....|" なので、プロット領域は col=3..(UI_COLS-2) */

static int
piano_plot_col(uint8_t octave, uint8_t note /*1..12*/)
{
    /* octave range is O1..O7 */
    if (octave < 1 || octave > 7)
        return -1;
    if (note < 1 || note > 12)
        return -1;

    /* Segment widths derived from your header border:
       O1:8, O2..O6:11, O7:6  (payload only, excluding separators)
       We map 12 semitones into each segment with integer scaling.
     */
    static const int segw[7] = { 8, 11, 11, 11, 11, 11, 6 };

    /* Piano header line has: "| | O1     |O2         |...|O7    |"
       Data lines are "|A|.............|"
       We'll treat the plot area starting right after "|A|" = col 3.
       Then, we lay out each octave segment with a '|' separator exactly like header:
         [seg O1 payload][|][seg O2 payload][|]...[|][seg O7 payload]
       Total payload+seps must match the dots count in template; if minor mismatch,
       it will still look OK because we're only plotting single markers.
     */

    int col = 3; /* first dot column */
    for (int o = 1; o < octave; o++) {
        col += segw[o - 1];
        col += 1; /* separator '|' position in header; in dots line it's also '.' but we keep spacing */
    }

    int w = segw[octave - 1];
    int semi = (int)note - 1; /* 0..11 */

    /* scale semitone to [0..w-1] */
    int dx = (w <= 1) ? 0 : (semi * (w - 1)) / 11;
    int x = col + dx;

    /* clamp into drawable region */
    if (x < 3)
        x = 3;
    if (x > UI_COLS - 2)
        x = UI_COLS - 2;
    return x;
}

/* ---- ここからが差し替え対象の ui_render() 本体 ---- */

/*
 * Required UI_state fields (already in your earlier UI_state):
 * - mus[3] : contains octave/note/volume/len/is_rest/t_ns
 * - r[16]  : register shadow updated in ui_on_reg_write()
 * - tone_enable[3], noise_enable[3] : from mixer (r7)
 * - noise_period
 * - w ring etc (you can keep it; this template UI doesn't show last-writes)
 *
 * Also add:
 * - prev[UI_ROWS][UI_COLS+1], have_prev
 */

static void
ui_render(UI_state *ui, uint64_t now_ns, const char *title)
{
    /* 1) build frame from templates */
    char frame[UI_ROWS][UI_COLS + 1];
    for (int r = 0; r < UI_ROWS; r++)
        line_copy_from_template(frame[r], ui_tmpl[r]);

    /* 2) dynamic fields coordinates (0-based col) */

    /* Music Title line: row 4 in tmpl (0-based), underline field begins after "Music Title: " */
    const int ROW_TITLE = 4;
    const int COL_TITLE = 15;      /* after "| Music Title: " */
    const int W_TITLE   = 40;      /* number of '_' in template */

    /* t=xxx.xs and tick= */
    const int COL_TSEC  = 58;      /* points to first digit in "t=123.4s" */
    const int COL_TICK  = 71;      /* points to first digit in "tick=61700" */
    const int W_TSEC    = 5;       /* "123.4" */
    const int W_TICK    = 5;       /* adjust if you want 6; template shows 61700 */

    /* Channel rows */
    const int ROW_CH[3] = { 6, 7, 8 };

    /* NOTE field: after "NOTE=" in each channel line.
       In template: "| Ch A: NOTE=E4  329.6Hz ..."
                  ^ col 0 is '|'
       We will overwrite from NOTE start to keep consistent.
     */
    const int COL_NOTE  = 13;      /* points to 'E' in "E4" */
    const int W_NOTE    = 3;       /* allow "C#4" */
    const int COL_HZ    = 17;      /* points to first digit in "329.6" */
    const int W_HZ      = 5;       /* "329.6" */
    const int COL_VOLN  = 30;      /* points to first digit in "10" (VOL=10) */
    const int W_VOLN    = 2;
    const int COL_BAR   = 34;      /* inside [...............] start */
    const int W_BAR     = 15;
    const int COL_TONE  = 57;      /* points to 'O' in "ON"/"OFF" */
    const int COL_NOISE = 68;      /* points to 'O' in "OFF" */

    /* Piano marker rows: A,B,C */
    const int ROW_PIANO[3] = { 14, 15, 16 };

    /* Register hex fields in bottom area */
    const int ROW_R0 = 18, ROW_R2 = 19, ROW_R4 = 20, ROW_R6 = 21;
    /* columns of "xxh" in those lines (counted to match template) */
    const int COL_R0 = 22, COL_R1 = 51, COL_R8 = 74;
    const int COL_R2 = 22, COL_R3 = 51, COL_R9 = 74;
    const int COL_R4 = 22, COL_R5 = 51, COL_RA = 74;
    const int COL_R6 = 22, COL_R7 = 51;

    /* 3) Fill title (UTF-8) */
    {
        /* ensure locale once (ok if called repeatedly, but you can move to init) */
        setlocale(LC_CTYPE, "");

        char fitted[W_TITLE * 4 + 1];
        utf8_fit_cols(fitted, sizeof(fitted), (title ? title : "(no title)"), W_TITLE);

        /* replace underscores area */
        put_bytes(frame[ROW_TITLE], COL_TITLE, fitted);
    }

    /* 4) time and tick */
    {
        double tsec = (double)now_ns / 1e9;
        /* overwrite digits only; keep "t=" and "s" fixed */
        put_f1_r(frame[ROW_TITLE], COL_TSEC, W_TSEC, tsec);

        /* tick: show ui->tick_count if you have it; otherwise 0 */
        uint32_t tick = 0;
        /* If your driver exposes tick_count, pass it via UI_state; here assume ui->tick_count exists */
        /* tick = (uint32_t)ui->some_tick_counter; */
        put_u32_r(frame[ROW_TITLE], COL_TICK, W_TICK, tick);
    }

    /* 5) channel lines: NOTE/Hz/VOL/bar/TONE/NOISE */
    const double clock_hz = 2000000.0; /* match your banner */
    for (int ch = 0; ch < 3; ch++) {
        int row = ROW_CH[ch];

        /* NOTE */
        char nbuf[4];
        make_note_ascii(nbuf, ui->mus[ch].octave, ui->mus[ch].note, ui->mus[ch].is_rest);
        put_spaces(frame[row], COL_NOTE, W_NOTE);
        put_bytes(frame[row], COL_NOTE, nbuf);

        /* Hz from register shadow (period) */
        uint16_t period = 0;
        period = (uint16_t)ui->reg[ch * 2 + 0] |
                  ((uint16_t)(ui->reg[ch * 2 + 1] & 0x0f) << 8);

        if (ui->mus[ch].is_rest ||
            ui->mus[ch].note == 0 ||
            ui->mus[ch].volume == 0 ||
            period == 0) {
            put_bytes(frame[row], COL_HZ, "-----");
        } else {
            double hz = psg_period_to_hz(period, clock_hz);
            /* clamp for display sanity */
            if (hz > 9999.9)
                hz = 9999.9;
            put_f1_r(frame[row], COL_HZ, W_HZ, hz);
        }

        /* VOL */
        put_u32_r(frame[row], COL_VOLN, W_VOLN, (uint32_t)(ui->mus[ch].volume & 0x0f));
        /* BAR */
        put_vol_bar(frame[row], COL_BAR, W_BAR, (uint8_t)(ui->mus[ch].volume & 0x0f));

        /* TONE/NOISE flags */
        put_bytes(frame[row], COL_TONE,  ui->tone_enable[ch]  ? "ON " : "OFF");
        put_bytes(frame[row], COL_NOISE, ui->noise_enable[ch] ? "ON " : "OFF");
    }

    /* 6) piano markers: clear lines are from template (dots), then plot one char per channel */
    for (int ch = 0; ch < 3; ch++) {
        int row = ROW_PIANO[ch];
        /* place marker only when audible-ish */
        if (ui->mus[ch].is_rest ||
          ui->mus[ch].note == 0 ||
          ui->mus[ch].volume == 0)
            continue;

        int x = piano_plot_col(ui->mus[ch].octave, ui->mus[ch].note);
        if (x < 0)
            continue;

        char mark = (ch == 0) ? 'A' : (ch == 1) ? 'B' : 'C';
        frame[row][x] = mark;
    }

    /* 7) registers display (xxh fields) */
    put_hex2h(frame[ROW_R0], COL_R0, ui->reg[0]);
    put_hex2h(frame[ROW_R0], COL_R1, ui->reg[1]);
    put_hex2h(frame[ROW_R0], COL_R8, ui->reg[8]);

    put_hex2h(frame[ROW_R2], COL_R2, ui->reg[2]);
    put_hex2h(frame[ROW_R2], COL_R3, ui->reg[3]);
    put_hex2h(frame[ROW_R2], COL_R9, ui->reg[9]);

    put_hex2h(frame[ROW_R4], COL_R4, ui->reg[4]);
    put_hex2h(frame[ROW_R4], COL_R5, ui->reg[5]);
    put_hex2h(frame[ROW_R4], COL_RA, ui->reg[10]);

    put_hex2h(frame[ROW_R6], COL_R6, ui->reg[6]);
    put_hex2h(frame[ROW_R6], COL_R7, ui->reg[7]);

    /* 8) draw: line-diff to reduce flicker */
    fputs("\033[H", stdout); /* home */

    if (ui->have_prev == 0) {
        /* full draw first time */
        for (int r = 0; r < UI_ROWS; r++) {
            fputs(frame[r], stdout);
            fputc('\n', stdout);
        }
        /* clear rest */
        fputs("\033[24;1H\033[J", stdout);
        /* save */
        for (int r = 0; r < UI_ROWS; r++)
            memcpy(ui->prev[r], frame[r], UI_COLS + 1);
        ui->have_prev = 1;
        return;
    }

    for (int r = 0; r < UI_ROWS; r++) {
        if (memcmp(ui->prev[r], frame[r], UI_COLS) != 0) {
            /* move cursor to row r+1, col 1 */
            char esc[32];
            snprintf(esc, sizeof(esc), "\033[%d;1H", r + 1);
            fputs(esc, stdout);
            fputs(frame[r], stdout);
            memcpy(ui->prev[r], frame[r], UI_COLS + 1);
        }
    }
}

void
ui_maybe_render(UI_state *ui, uint64_t now_ns, const char *title)
{
    if (now_ns < ui->next_ui_ns)
        return;
    ui_render(ui, now_ns, title);
    ui->next_ui_ns = now_ns + ui->ui_period_ns;
}
