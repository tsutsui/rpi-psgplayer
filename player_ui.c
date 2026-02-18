/*
 * player_ui.c
 *  For PSG Player demonstration on Raspberry Pi 3B at Open Source Conference
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <inttypes.h>
#include <assert.h>
#include <math.h>
#include <wchar.h>
#include <locale.h>
#include <termios.h>

#include "player_ui.h"
#include "ym2149f.h"

/* ---- 固定テンプレ（79桁×23行） ---- */

static const char *ui_tmpl[UI_ROWS] = {
/* 0000000000111111111122222222223333333333444444444455555555556666666666777777777 */
/* 0123456789012345678901234567890123456789012345678901234567890123456789012345678 */
  "+-----------------------------------------------------------------------------+", /*  0 */
  "| YM2149 P6 PSG Player on Raspberry Pi 3B @ Open Source Conference Osaka 2026 |", /*  1 */
  "| Clock: 2.000 MHz, Rate: 2ms/tick, BCM2837 GPIO controlled by NetBSD/evbarm  |", /*  2 */
  "+-----------------------------------------------------------------------------+", /*  3 */
  "| Music Title: _____________________________________    bpm=___._  t=_____._s |", /*  4 */
  "+-----------------------------------------------------------------------------+", /*  5 */
  "| Ch A: NOTE=--   ---.-Hz  VOL=__ [...............]  TONE=ON   NOISE=OFF      |", /*  6 */
  "| Ch B: NOTE=--   ---.-Hz  VOL=__ [...............]  TONE=ON   NOISE=OFF      |", /*  7 */
  "| Ch C: NOTE=--   ---.-Hz  VOL=__ [...............]  TONE=ON   NOISE=OFF      |", /*  8 */
  "+-+--------+-----------+-----------+-----------+-----------+-----------+------+", /*  9 */
  "| |O1      |O2         |O3         |O4         |O5         |O6         |O7    |", /* 10 */
  "|C|  # # # |# #  # # # |# #  # # # |# #  # # # |# #  # # # |# #  # # # |# #  #|", /* 11 */
  "|h|<F G A BC D EF G A BC D EF G A BC D EF G A BC D EF G A BC D EF G A BC D EF>|", /* 12 */
  "+-+--------+-----------+-----------+-----------+-----------+-----------+------+", /* 13 */
  "|A|...........................................................................|", /* 14 */
  "|B|...........................................................................|", /* 15 */
  "|C|...........................................................................|", /* 16 */
  "+-----------------------------------------------------------------------------+", /* 17 */
  "| Reg0 (Freq Fine A): --h || Reg1 (Freq Rough A) : --h || Reg8 (Level A): --h |", /* 18 */
  "| Reg2 (Freq Fine B): --h || Reg3 (Freq Rough B) : --h || Reg9 (Level B): --h |", /* 19 */
  "| Reg4 (Freq Fine C): --h || Reg5 (Freq Rough C) : --h || RegA (Level C): --h |", /* 20 */
  "| Reg6 (Freq Noise) : --h || Reg7 (Mixer Setting): --h ||                     |", /* 21 */
  "+-----------------------------------------------------------------------------+"  /* 22 */
};

/* 更新表示必要なパラメータの行と桁定義 (0-origin) */
enum {
    /* タイトル行: タイトル、テンポ、経過時間 */
    ROW_TITLE = 4,

    COL_TITLE = 15,
    COL_TEMPO = 60,
    COL_TSEC  = 69,

    /* チャンネル A/B/C 行: ノート、周波数、ボリューム、トーン/ノイズ */
    ROW_CH_A = 6,
    ROW_CH_B = 7,
    ROW_CH_C = 8,

    COL_NOTE  = 13,
    COL_HZ    = 17,
    COL_VOLN  = 31,
    COL_BAR   = 35,
    COL_TONE  = 58,
    COL_NOISE = 69,

    /* ピアノロール表示行 */
    ROW_PIANO_A = 14,
    ROW_PIANO_B = 15,
    ROW_PIANO_C = 16,

    /* レジスタ値表示行 */
    ROW_R0 = 18,
    ROW_R2 = 19,
    ROW_R4 = 20,
    ROW_R6 = 21,

    COL_R0 = 22, COL_R1 = 51, COL_R8 = 74,
    COL_R2 = 22, COL_R3 = 51, COL_R9 = 74,
    COL_R4 = 22, COL_R5 = 51, COL_RA = 74,
    COL_R6 = 22, COL_R7 = 51
};

/* UI画面出力バッファリング */
static inline void
ui_out_reset(UI_state *ui)
{
    ui->out_len = 0;
}

static inline void
ui_out_flush(UI_state *ui)
{
    if (ui->out_len == 0)
        return;
    /* 描画テキストが揃ったところで1回のwrite(2)で画面更新 */
    (void)write(STDOUT_FILENO, ui->out_buf, ui->out_len);
    ui->out_len = 0;
}

static inline void
ui_out_append(UI_state *ui, const char *s, size_t n)
{
    if (n > UI_OUT_CAP) {
        /* 念の為で大量描画の場合は直書き出力 */
        ui_out_flush(ui);
        (void)write(STDOUT_FILENO, s, n);
        return;
    }

    /* バッファが足りなければ途中 flush */
    if (ui->out_len + n > UI_OUT_CAP) {
        ui_out_flush(ui);
    }
    memcpy(ui->out_buf + ui->out_len, s, n);
    ui->out_len += n;
}

static inline void
ui_out_puts(UI_state *ui, const char *s)
{
    ui_out_append(ui, s, strlen(s));
}

static inline void
ui_out_printf(UI_state *ui, const char *fmt, ...)
{
    char tmp[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);

    if (n <= 0)
        return;

    if ((size_t)n < sizeof(tmp)) {
        ui_out_append(ui, tmp, (size_t)n);
        return;
    }

    /* 256 を超えるなら必要サイズで確保して append */
    char *dyn = malloc((size_t)n + 1);
    if (!dyn)
        return;
    va_start(ap, fmt);
    vsnprintf(dyn, (size_t)n + 1, fmt, ap);
    va_end(ap);
    ui_out_append(ui, dyn, (size_t)n);
    free(dyn);
}

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

static void
ui_term_apply(UI_state *ui)
{
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO))
        return;

    /* termios 保存 */
    if (tcgetattr(STDIN_FILENO, &ui->tio_saved) == 0) {
        ui->tio_saved_valid = 1;

        struct termios tio = ui->tio_saved;
        tio.c_lflag &= ~(ICANON | ECHO);
        tio.c_cc[VMIN]  = 1;
        tio.c_cc[VTIME] = 0;
        (void)tcsetattr(STDIN_FILENO, TCSANOW, &tio);
    }

    /*
     * genfb(4) + wsdisplay(4) だとカーソルを消してから alternate screen すると
     * カーソルを消した位置でカーソル表示が出て残ってしまうっぽい（バグ?）ので
     * 先に表示枠外に移動させておく
     */
    fputs("\033[24;1H", stdout);

    /* カーソル消す */
    fputs("\033[?25l", stdout);
    ui->cursor_hidden = 1;

    /* autowrap OFF */
    fputs("\033[?7l", stdout);
    ui->wrap_disabled = 1;
}

static void
ui_term_restore(UI_state *ui)
{
    if (ui->wrap_disabled) {
        fputs("\033[?7h", stdout);
        ui->wrap_disabled = 0;
    }
    if (ui->cursor_hidden) {
        fputs("\033[?25h", stdout);
        ui->cursor_hidden = 0;
    }
    if (ui->tio_saved_valid) {
        (void)tcsetattr(STDIN_FILENO, TCSANOW, &ui->tio_saved);
        ui->tio_saved_valid = 0;
    }
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
make_note_ascii(char out[UI_W_NOTE + 1], uint8_t octave, uint8_t note, uint8_t is_rest)
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
            if (out + 1 < dstsz)
                dst[out++] = '?';
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
/* ピアノロール行は "|A|.....|" なので、プロット領域は col=3..(UI_COLS-2) */

static int
piano_plot_col(uint8_t octave, uint8_t note /*1..12*/)
{
    /* octave range is O1..O7 */
    if (octave < 1 || octave > 7)
        return -1;
    if (note < 1 || note > 12)
        return -1;

    /* プロット領域左端 col=3 の位置が O1E (octave=1, note=5) なので -1 必要 */
    int x = (octave - 1) * 12 + (note - 1) - 1;

    /* clamp into drawable region */
    if (x < 3)
        x = 3;
    if (x > UI_COLS - 2)
        x = UI_COLS - 2;
    return x;
}

static int
piano_plot_col_noise(uint8_t reg6)
{
    /*
     * ノイズのみの時はMMLのノートは意味がない一方で
     * Reg6のノイズ周波数は聴感とは一致しないので
     * ピアノロールはノイズ設定の Reg6値で適当に散らす。
     * Reg6の値が大きいほど低く聞こえるので Reg6 31..0 の並びを
     * O3Cから配置してみる。
     *
     * プロット領域左端 col=3 (0-origin) の位置が O1E (octave=1, note=5) で
     * O3C の位置はそこから +8 +12 なので +20 する
     */
    return 3 + 8 + 12 + (31 - reg6);
}

/* 状態表示更新処理 */

/* チャンネル別チャンネル状態表示行 */
static inline int
row_ch(int ch)
{
    return (ch == 0) ? ROW_CH_A : (ch == 1) ? ROW_CH_B : ROW_CH_C;
}

/* チャンネル別ピアノロール表示行 */
static inline int
row_piano(int ch)
{
    return (ch == 0) ? ROW_PIANO_A : (ch == 1) ? ROW_PIANO_B : ROW_PIANO_C;
}

/* 渡された width バイトが変化ありなら (row,col) 位置から表示出力ヘルパ */
static void
ui_put_fixed_if_changed(UI_state *ui,
    int row0, int col0, int width,
    const char *cur_fixed, char *cache_fixed)
{
    /* 変化なければ更新不要 */
    if (memcmp(cache_fixed, cur_fixed, (size_t)width) == 0)
        return;

    /* CUP is 1-based */
    ui_out_printf(ui, "\033[%d;%dH", row0 + 1, col0 + 1);
    ui_out_append(ui, cur_fixed, (size_t)width);

    memcpy(cache_fixed, cur_fixed, (size_t)width);
    cache_fixed[width] = '\0';
}

/* 固定幅文字列を返すフォーマットヘルパ */
static void
fmt_pad(char *dst, int width, const char *src)
{
    int n = (int)strlen(src);
    if (n > width)
        n = width;
    memcpy(dst, src, (size_t)n);
    for (int i = n; i < width; i++)
        dst[i] = ' ';
    dst[width] = '\0';
}

/* 引数のdoubleに対応する固定幅小数点文字列を返すフォーマットヘルパ */
static void
fmt_f1_fixed(char *dst, int width, double x)
{
    char tmp[64];
    /* right aligned, 1 decimal */
    snprintf(tmp, sizeof(tmp), "%*.*f", width, 1, x);
    fmt_pad(dst, width, tmp);
}

/* 引数のintに対応する固定幅整数文字列を返すフォーマットヘルパ */
static void
fmt_u_fixed(char *dst, int width, unsigned int v)
{
    char tmp[64];
    snprintf(tmp, sizeof(tmp), "%*u", width, v);
    fmt_pad(dst, width, tmp);
}

/* 引数のuint8_tに対応する固定幅16進2桁を返すフォーマットヘルパ */
static void
fmt_hex2h_fixed(char dst3[4], uint8_t v)
{
    snprintf(dst3, 4, "%02Xh", v);
}

/* ボリュームバー表示を返すヘルパ */
static void
fmt_vol_bar_fixed(char *dst, int width, uint8_t vol, uint8_t reg)
{
    int vfilled = (int)((vol * (uint8_t)width + 14) / 15);
    int rfilled = (int)((reg * (uint8_t)width + 14) / 15);
    /* MML上のVOLを - 、休符やソフトウェアエンベロープ含む実音量を # で表示 */
    for (int i = 0; i < width; i++)
        dst[i] = (i < rfilled) ? '#' : (i < vfilled) ? '-' : '.';
    dst[width] = '\0';
}

/* 初回の画面テンプレート表示 */
static void
ui_draw_template_once(UI_state *ui)
{
    ui_out_reset(ui);
    ui_out_puts(ui, "\033[H");
    for (int r = 0; r < UI_ROWS; r++) {
        size_t n = strlen(ui_tmpl[r]);
        assert(n == UI_COLS);
        ui_out_append(ui, ui_tmpl[r], UI_COLS);
        ui_out_puts(ui, "\n");
    }
    ui_out_puts(ui, "\033[24;1H\033[J");
    ui_out_flush(ui);
    ui->template_drawn = 1;
}

/* ピアノロールマーカー表示更新 */
static void
ui_update_piano_marker(UI_state *ui, int ch, int want_mark, int x_new,
  char mark_char)
{
    int row = row_piano(ch);

    int x_old = ui->cache_piano_x[ch];
    char m_old = ui->cache_piano_mark[ch];

    /* 前回表示ありの場合、今回発声無しもしくは変化ありなら '.' に戻す */
    if (x_old >= 0) {
        if (!want_mark || x_new != x_old || mark_char != m_old) {
            ui_out_printf(ui, "\033[%d;%dH", row + 1, x_old + 1);
            ui_out_append(ui, ".", 1);
            ui->cache_piano_x[ch] = -1;
            ui->cache_piano_mark[ch] = '\0';
        }
    }

    if (want_mark) {
        /* 今回発声ありかつ変化ありなら表示更新 */
        if (x_old != x_new || m_old != mark_char) {
            ui_out_printf(ui, "\033[%d;%dH", row + 1, x_new + 1);
            ui_out_append(ui, &mark_char, 1);
            ui->cache_piano_x[ch] = x_new;
            ui->cache_piano_mark[ch] = mark_char;
        }
    }
}

/* レジスタ値表示更新 */
static void
put_reg_if_changed(UI_state *ui, int row0, int col0, int regno)
{
    /* 変化なければ更新不要 */
    if (ui->cache_reg[regno] == ui->reg[regno])
        return;
    char buf[4];
    fmt_hex2h_fixed(buf, ui->reg[regno]);
    ui_out_printf(ui, "\033[%d;%dH", row0 + 1, col0 + 1);
    ui_out_append(ui, buf, 3);
    ui->cache_reg[regno] = ui->reg[regno];
}

/* 描画キャッシュデータクリア */
static void
ui_cache_clear(UI_state *ui)
{
    ui->template_drawn = 0;
    for (int ch = 0; ch < 3; ch++) {
        ui->cache_note[ch][0]  = '\0';
        ui->cache_hz[ch][0]    = '\0';
        ui->cache_voln[ch][0]  = '\0';
        ui->cache_bar[ch][0]   = '\0';
        ui->cache_tone[ch][0]  = '\0';
        ui->cache_noise[ch][0] = '\0';
        ui->cache_piano_x[ch]  = -1;
        ui->cache_piano_mark[ch] = '\0';
    }
    ui->cache_title[0] = '\0';
    ui->cache_bpm[0]   = '\0';
    ui->cache_tsec[0]  = '\0';
    ui->cache_reg_valid = 0;
}

/*
 * Required UI_state fields (already in your earlier UI_state):
 * - mus[3] : contains octave/note/volume/len/is_rest/t_ns
 * - reg[16]  : register shadow updated in ui_on_reg_write()
 * - tone_enable[3], noise_enable[3] : from mixer (r7)
 * - noise_period
 * - w ring etc (you can keep it; this template UI doesn't show last-writes)
 */

static void
ui_render(UI_state *ui, uint64_t now_ns, const char *title)
{
    ui_out_reset(ui);

    if (ui->redraw) {
        ui_cache_clear(ui);
        ui->redraw = 0;
    }

    if (!ui->template_drawn)
        ui_draw_template_once(ui);

    /* 1) Fill title (UTF-8, column-fitted, diff update) */
    {
        char fitted[UI_W_TITLE * 4 + 1];

        /*
         * タイトルは日本語文字列表示も想定して UTF-8 にも対応するので
         * 表示幅数と文字列バイト数とは一致しない。
         * ただ、差分更新表示仕様では表示用の固定幅バッファは使用せず
         * 「表示内容に変化があったら指定行の指定桁からタイトル文字列を出力」
         * という操作なので、表示幅ではなく表示するUTF-8文字列バイト全体を
         * キャッシュして比較する。
         * ここで utf8_fit_cols() はコードポイント単位で文字幅を判定するので
         * 絵文字・ZWJ・国旗などの合成グリフなどは端末上の表示幅と一致しない
         * ケースがあるが、そこまでの厳密な UTF-8対応はせずに
         * 「日本語が出せる」「途中で切っても文字化けしない」
         * という仕様まで。
         */
        utf8_fit_cols(fitted, sizeof(fitted),
            (title ? title : "(no title)"), UI_W_TITLE);

        size_t cur_len = strlen(fitted);
        size_t old_len = strlen(ui->cache_title);

        if (cur_len != old_len ||
          memcmp(ui->cache_title, fitted, cur_len) != 0) {
            ui_out_printf(ui, "\033[%d;%dH", ROW_TITLE + 1, COL_TITLE + 1);
            ui_out_append(ui, fitted, cur_len);

            /* 表示幅分のUTF-8文字列バイト数でキャッシュ */
            if (cur_len >= sizeof(ui->cache_title))
                cur_len = sizeof(ui->cache_title) - 1;
            memcpy(ui->cache_title, fitted, cur_len);
            ui->cache_title[cur_len] = '\0';
        }
    }

    /* 2) bpm and time */
    {
        char bpm_fixed[UI_W_BPM + 1];
        double bpm = ui->bpm_x10 / 10.0;
        fmt_f1_fixed(bpm_fixed, UI_W_BPM, bpm);
        ui_put_fixed_if_changed(ui, ROW_TITLE, COL_TEMPO, UI_W_BPM, bpm_fixed,
          ui->cache_bpm);

        char tsec_fixed[UI_W_TSEC + 1];
        double tsec = (double)(now_ns - ui->start_ns) / 1e9;
        fmt_f1_fixed(tsec_fixed, UI_W_TSEC, tsec);
        ui_put_fixed_if_changed(ui, ROW_TITLE, COL_TSEC, UI_W_TSEC, tsec_fixed,
          ui->cache_tsec);
    }

    /* 3) channel lines (NOTE/Hz/VOL/bar/TONE/NOISE) and piano markers */
    const double clock_hz = 2000000.0;
    for (int ch = 0; ch < 3; ch++) {
        int row = row_ch(ch);

        /* 「トーン無しノイズのみ」の判定用 */
        int noise_only =
          (ui->tone_enable[ch] == 0) && (ui->noise_enable[ch] != 0);

        /* NOTE */
        {
            char note_tmp[UI_W_NOTE + 1];
            make_note_ascii(note_tmp, ui->mus[ch].octave, ui->mus[ch].note,
              ui->mus[ch].is_rest);
            if (noise_only && (ui->mus[ch].volume != 0)) {
                /* ノイズのみの時はMMLのノートは意味がないので別表示にする */
                strcpy(note_tmp, "NOI");
            }
            char note_fixed[UI_W_NOTE + 1];
            fmt_pad(note_fixed, UI_W_NOTE, note_tmp);
            ui_put_fixed_if_changed(ui, row, COL_NOTE, UI_W_NOTE, note_fixed,
              ui->cache_note[ch]);
        }

        /* Hz from register shadow (period) */
        {
            uint16_t period =
                (uint16_t)ui->reg[ch * 2 + 0] |
                ((uint16_t)(ui->reg[ch * 2 + 1] & 0x0f) << 8);

            char hz_fixed[UI_W_HZ + 1];
            if (ui->mus[ch].is_rest ||
                ui->mus[ch].note == 0 ||
                ui->mus[ch].volume == 0 ||
                period == 0 ||
                noise_only) {
                fmt_pad(hz_fixed, UI_W_HZ, " -----");
            } else {
                double hz = psg_period_to_hz(period, clock_hz);
                /* clamp for display sanity */
                if (hz > 9999.9)
                    hz = 9999.9;
                fmt_f1_fixed(hz_fixed, UI_W_HZ, hz);
            }
            ui_put_fixed_if_changed(ui, row, COL_HZ, UI_W_HZ, hz_fixed,
              ui->cache_hz[ch]);
        }

        /* VOL number */
        {
            char vol_fixed[UI_W_VOLN + 1];
            fmt_u_fixed(vol_fixed, UI_W_VOLN, ui->mus[ch].volume & 0x0f);
            ui_put_fixed_if_changed(ui, row, COL_VOLN, UI_W_VOLN, vol_fixed,
              ui->cache_voln[ch]);
        }

        /* Volume BAR */
        {
            char bar_fixed[UI_W_BAR + 1];
            fmt_vol_bar_fixed(bar_fixed, UI_W_BAR,
              ui->mus[ch].volume & 0x0f, ui->reg[AY_AVOL + ch] & 0x0f);
            ui_put_fixed_if_changed(ui, row, COL_BAR, UI_W_BAR, bar_fixed,
              ui->cache_bar[ch]);
        }

        /* TONE / NOISE fixed ("ON " or "OFF") */
        {
            const char *tone_s  = ui->tone_enable[ch]  ? "ON " : "OFF";
            const char *noise_s = ui->noise_enable[ch] ? "ON " : "OFF";
            ui_put_fixed_if_changed(ui, row, COL_TONE,  3, tone_s,
              ui->cache_tone[ch]);
            ui_put_fixed_if_changed(ui, row, COL_NOISE, 3, noise_s,
              ui->cache_noise[ch]);
        }

        /* piano marker: only if audible-ish, else clear */
        {
            int audible = ui->mus[ch].is_rest == 0 &&
                          ui->mus[ch].note != 0 &&
                          ui->mus[ch].volume != 0;
            int want = audible;

            int x = -1;
            if (want) {
                if (noise_only) {
                    x = piano_plot_col_noise(ui->reg[6]);
                } else {
                    x = piano_plot_col(ui->mus[ch].octave, ui->mus[ch].note);
                }
                if (x < 0)
                    want = 0;
            }
            char mark =
              noise_only ? 'N' : (ch == 0) ? 'A' : (ch == 1) ? 'B' : 'C';
            ui_update_piano_marker(ui, ch, want, x, mark);
        }
    }

    /* 4) registers display (xxh fields) */
    {
        if (!ui->cache_reg_valid) {
            memcpy(ui->cache_reg, ui->reg, sizeof(ui->cache_reg));
            ui->cache_reg_valid = 1;
            /* force a write on first render after init */
            for (int i = 0; i < 16; i++)
                ui->cache_reg[i] ^= 0xFF;
        }

        put_reg_if_changed(ui, ROW_R0, COL_R0, 0);
        put_reg_if_changed(ui, ROW_R0, COL_R1, 1);
        put_reg_if_changed(ui, ROW_R0, COL_R8, 8);

        put_reg_if_changed(ui, ROW_R2, COL_R2, 2);
        put_reg_if_changed(ui, ROW_R2, COL_R3, 3);
        put_reg_if_changed(ui, ROW_R2, COL_R9, 9);

        put_reg_if_changed(ui, ROW_R4, COL_R4, 4);
        put_reg_if_changed(ui, ROW_R4, COL_R5, 5);
        put_reg_if_changed(ui, ROW_R4, COL_RA, 10);

        put_reg_if_changed(ui, ROW_R6, COL_R6, 6);
        put_reg_if_changed(ui, ROW_R6, COL_R7, 7);
    }

    /* park cursor + flush once */
    ui_out_puts(ui, "\033[24;1H");
    ui_out_flush(ui);
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
    uint16_t len, uint8_t is_rest, uint16_t bpm_x10)
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

    ui->bpm_x10 = bpm_x10;
}

/* ANSI UI init/shutdown/render entry points */

void
ui_init(UI_state *ui, uint64_t now_ns)
{
    memset(ui, 0, sizeof(*ui));
    ui->ui_period_ns = 33333333ull; /* 33.3ms (30 fps) */
    ui->start_ns     = now_ns;
    ui->next_ui_ns   = now_ns + ui->ui_period_ns;

    /* caches: mark as invalid */
    ui_cache_clear(ui);

    ui_term_apply(ui);

    /* alternate screen + clear */
    fputs("\033[?1049h\033[H\033[J", stdout);
    fflush(stdout);

    /* 曲タイトル UTF-8 表示用の utf8_fit_cols() で必要 */
    setlocale(LC_CTYPE, "");

    /* draw template now (once) */
    ui_draw_template_once(ui);

    /*
     * コンソール画面描画完了までとりあえず 500ms 待たせる
     * マルチコアの Pi3 だと write(2) システムコールの先の
     * wsdisplay 描画完了前でもユーザープロセスに戻ってくるようで
     * 演奏開始後の初回描画で待たされてしまうようである
     */
    usleep(500 * 1000);

    ui->initialized = 1;
}

void
ui_shutdown(UI_state *ui)
{
    if (ui == NULL || ui->initialized == 0)
        return;

    ui_term_restore(ui);

    /* move cursor (in case of no alternate screen) */
    fputs("\033[24;1H", stdout);

    /* leave alternate screen */
    fputs("\033[?1049l", stdout);
    ui->initialized = 0;
}

void
ui_maybe_render(UI_state *ui, uint64_t now_ns, const char *title)
{
    if (now_ns < ui->next_ui_ns)
        return;
    ui_render(ui, now_ns, title);
    ui->next_ui_ns = now_ns + ui->ui_period_ns;
}

void
ui_request_redraw(UI_state *ui)
{
    if (ui == NULL)
        return;

    ui->redraw = 1;
}
