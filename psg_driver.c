#include "psg_driver.h"
#include <string.h>
#include <stdio.h>

/* 12音階テーブル */
static const uint16_t psg_tone_table_oct0[13] = {
    0,      /* 0: R  */
    0x1DDD, /* 1: C  */
    0x1C2F, /* 2: C# */
    0x1A9A, /* 3: D  */
    0x191C, /* 4: D# */
    0x17B3, /* 5: E  */
    0x165F, /* 6: F  */
    0x151D, /* 7: F# */
    0x13EE, /* 8: G  */
    0x12D0, /* 9: G# */
    0x11C1, /* A: A  */
    0x10C2, /* B: A# */
    0x0FD2  /* C: B  */
};

/* octave 値 (1〜8) と note (1〜12) からトーン値を算出（ざっくり）。 */
static uint16_t
psg_calc_tone(uint8_t octave, uint8_t note)
{
    if (note == 0 || note > 12 || octave < 1 || octave > 8) {
        return 0;
    }

    /* octave=0 基準 (オリジナル P6 ドライバ準拠) */
    uint16_t base = psg_tone_table_oct0[note];

    base >>= octave; /* オクターブ毎に周期値を半分＝周波数 2倍 */

    return base;
}

/* ドライバテンポ値から bpm値を算出 */
static inline uint16_t
calc_bpm_x10_from_t96(uint8_t t96)
{
    if (t96 == 0)
        return 0;
    /*
     * 1分 (60秒) あたりの4分音符数を bpm として表示する。
     *  - 4分音符は 96分音符の 24個分
     *  - 96分音符の長さ（秒）: (t96 × tick_ms) / 1000
     *  - 4分音符の長さ（秒） :   24 × [96分音符の長さ（秒）]
                                = 24 × (t96 × tick_ms / 1000)
     *  - 4分音符基準 bpm     : 60 / [4分音符の長さ（秒）]
     *
     * よって
     *  bpm = 60 / (24 × (t96 × tick_ms) / 1000)
     *      = (60 * 1000 / 24) / (t96 × tick_ms)
     *      = 2500 / (t96 × tick_ms)
     *
     * tick_ms = 2 [ms] の場合、かつ、四捨五入の 1/2 を分子に足すと
     *  bpm = (1250 + (t96 / 2)) / t96
     *
     * ここでは小数点以下1桁まで出す (x10) ので1桁増やして以下
     *  bpm_x10 = (1250 * 10 + (t96 / 2)) / t96
     */
    return (uint16_t)((12500u + (t96 / 2u)) / t96);
}

/* PSG レジスタ書き込みヘルパ */
static inline void
psg_write(PSGDriver *drv, uint8_t reg, uint8_t val)
{
    if (drv->write_reg) {
        (*drv->write_reg)(drv, reg, val);
    }
}

/* デモ画面表示用ノートデータ書き込み */
static inline void
psg_note_event(PSGDriver *drv, int ch, uint8_t octave, uint8_t note,
               uint8_t volume, uint16_t len, uint8_t is_rest, uint16_t bpm_x10)
{
    if (drv->note_event) {
        (*drv->note_event)(drv, ch, octave, note, volume, len, is_rest, bpm_x10);
    }
}

/* チャンネルをリセット */
static void
psg_channel_reset(PSGChannel *ch, int index)
{
    memset(ch, 0, sizeof(*ch));
    ch->channel_index = (uint8_t)index;
    ch->active        = 0;

    ch->l_default     = 24;
    ch->lplus_default = 192;

    ch->volume        = 12;
    ch->octave        = 4;

    ch->j_return_offset = 0;
}

/* ドライバ初期化 */
void
psg_driver_init(PSGDriver *drv,
                PSGWriteRegFn write_cb,
                void *write_user,
                PSGNoteEventFn note_cb,
                void *note_user)
{
    memset(drv, 0, sizeof(*drv));
    drv->write_reg  = write_cb;
    drv->write_user = write_user;
    drv->note_event = note_cb;
    drv->note_user  = note_user;

    drv->main.fade_value = 0;
    drv->main.fade_step = 0;
    drv->main.fade_active = 0;

    drv->main.tempo_val = 10;
    drv->main.tempo_counter = drv->main.tempo_val;
    drv->main.bpm_x10 = calc_bpm_x10_from_t96(drv->main.tempo_val);
    /*
     * enable tones (0..2 = 0)
     * disable noise (3..5 = 1)
     * both I/O output (7..6 = 1) => 0xf8
     */
    const uint8_t reg7_default = 0xf8;
    psg_write(drv, AY_ENABLE, reg7_default);
    drv->main.reg7_value = reg7_default;
    const uint8_t reg6_default = 0xc0;
    psg_write(drv, AY_NOISEPER, reg6_default);
    drv->main.reg6_value = reg6_default;

    for (int i = 0; i < 3; i++) {
        psg_channel_reset(&drv->ch[i], i);
    }
}

/* チャンネルにオブジェクトデータを設定 */
void
psg_driver_set_channel_data(PSGDriver *drv,
                            int ch_index,
                            const uint8_t *data)
{
    if (ch_index < 0 || ch_index >= 3) {
        return;
    }
    PSGChannel *ch = &drv->ch[ch_index];

    ch->data_base   = data;
    ch->data_offset = 0;
    ch->wait_counter = 1;
    ch->active       = 1;
}

/* 再生開始（とりあえずリセットしたうえで active=1 にする程度） */
void
psg_driver_start(PSGDriver *drv)
{
    for (int i = 0; i < 3; i++) {
        PSGChannel *ch = &drv->ch[i];
        ch->active       = (ch->data_base != NULL) ? 1 : 0;
    }
}

/* 再生停止 */
void
psg_driver_stop(PSGDriver *drv)
{
    for (int i = 0; i < 3; i++) {
        PSGChannel *ch = &drv->ch[i];
        ch->active       = 0;

        /* ボリューム0を書いてミュート */
        psg_write(drv, AY_AVOL + i, 0);
    }
}

/* I コマンド値取得（今は単純に MAIN ワークの内容を返すだけ） */
uint8_t
psg_driver_get_i_command(const PSGDriver *drv)
{
    return drv->main.i_command_value;
}

/* 1チャンネルぶんの 1tick 処理（Stage 1 簡易版） */
static void
psg_channel_tick(PSGDriver *drv, PSGChannel *ch)
{
    if (!ch->active) {
        return;
    }

    uint8_t wait_counter = ch->wait_counter;
    wait_counter--;
    ch->wait_counter = wait_counter;

    if (wait_counter > 0) {
        /* ノート再生中の処理 */

        /* 休符中は再生中処理なしで終了 */
        if ((ch->flags & CH_F_REST) != 0)
            return;

        if (ch->wait_counter == ch->q_counter) {
            /* 残り時間がゲート時間になったら音量オフ */
            psg_write(drv, AY_AVOL + ch->channel_index, 0);
            /* 後は休符相当で発声処理なしなので休符フラグセットしてリターン */
            ch->flags |= CH_F_REST;
            return;
        }

        /* 発声中ビブラート (LFO) 処理（未実装） */

        /* 発声中ソフトウェアエンベロープ (EG) 処理（未実装） */

        /* ノート継続なので終了 */
        return;
    }

    /* ノート終了時は次コマンド解析 */

    /* 次のオブジェクトを読み取るループ。
       コマンドだけが連続する場合を考慮して、音符を読むまで回す。 */
    for (;;) {
        uint8_t code = ch->data_base[ch->data_offset++];

        if ((code & F_NOTE) == 0) {
            /* === 音符オブジェクト === */

            int tie = (code & F_TIE) ? 1 : 0;

            /* ゲートオフ時間設定 */
            uint8_t q_counter = ch->q_default; 
            if (tie)
                q_counter = 0;

            uint8_t note     = code & F_PITCH;
            uint8_t len_flag = code & F_LEN;

            uint16_t len = 0;

            switch (len_flag) {
            case F_LEN_L: /* L デフォルト */
                len = ch->l_default;
                break;
            case F_LEN_LPLUS: /* L+ デフォルト */
                len = ch->lplus_default;
                break;
            case F_LEN_1BYTE: /* 1バイト音長 */
                len = ch->data_base[ch->data_offset++];
                break;
            case F_LEN_2BYTE: /* 2バイト音長 (little endian) */
                len  = ch->data_base[ch->data_offset++];
                len |= (uint16_t)ch->data_base[ch->data_offset++] << 8;
                break;
            }

            /* 音長カウンタセット */
            ch->wait_counter = len;

            /* ゲートオフ時間が発声時間より長い場合も1tickは発声させる */
            if (q_counter >= len)
                q_counter = len - 1;
            /* Q カウンタ現在値セット */
            ch->q_counter = q_counter;

            if (note == 0) {
                /* 休符 */

                /* 休符フラグセット */
                ch->flags |= CH_F_REST;

                /* 音量0を書き込み */
                psg_write(drv, AY_AVOL + ch->channel_index, 0);

                /* ui 表示用ノートイベント更新 */
                psg_note_event(drv, ch->channel_index,
                               ch->octave, 0, ch->volume, (uint16_t)len, 1,
                               drv->main.bpm_x10);

            } else {
                /* 通常の音符 */

                /* 休符フラグクリア */
                ch->flags &= ~CH_F_REST;

                if ((ch->flags & CH_F_TIE) == 0) {
                    /* ソフトウェアエンベロープ (EG) ワーク初期化（未実装） */
                }

                if ((ch->flags & CH_F_VIB_ON) != 0) {
                    /* ビブラート (LFO) ワーク初期化（未実装） */
                }

                /* 周波数レジスタ値算出 */
                uint16_t tone = psg_calc_tone(ch->octave, note);

                if (ch->detune != 0) {
                    /* デチューン分調整 */
                    if ((ch->detune & 0x80u) == 0) {
                        /* 最上位ビットが0なら周波数上げるので値は減算 */
                        tone -= ch->detune;
                    } else {
                        /* 最上位ビットが1なら周波数下げるので値は加算 */
                        tone += (ch->detune & ~0x80u);
                    }
                }

                /* 前ノートがタイでない場合は書き込み前にボリュームオフ */
                if (!tie)
                    psg_write(drv, AY_AVOL + ch->channel_index, 0);

                /* 現在の基準トーン値セット */
                ch->freq_value = tone;

                /* 周波数レジスタセット */
                psg_write(drv, AY_AFINE + ch->channel_index * 2,
                    (uint8_t)(tone & 0xFF));
                psg_write(drv, AY_ACOARSE + ch->channel_index * 2,
                    (uint8_t)((tone >> 8) & 0x0F));
                /* 音量レジスタセット */
                psg_write(drv, AY_AVOL + ch->channel_index,
                        (uint8_t)(ch->volume & 0x0F));

                /* ui 表示用ノートイベント更新 */
                psg_note_event(drv, ch->channel_index,
                               ch->octave, note, ch->volume, (uint16_t)len, 0,
                               drv->main.bpm_x10);
            }

            /* タイフラグ更新 */
            if (tie)
                ch->flags |= CH_F_TIE;
            else
                ch->flags &= ~CH_F_TIE;

            /* 音符を処理したので、この tick は終了 */
            return;
        }

        /* === コマンドオブジェクト === */
        uint8_t hi = code & 0xF0;

        if (hi == 0x80) {
            /* オクターブ o1〜o8 */
            ch->octave = code & 0x0F;
            /* 続けて次のオブジェクトを見る */
            continue;
        } else if (hi == 0x90) {
            /* ボリューム v0〜v15 */
            ch->volume = code & 0x0F;
            continue;
        } else if (hi == 0xA0) {
            int vol = ch->volume + (code & 0x0F);
            if (vol > 15)
                vol = 15;
            ch->volume = vol;
            continue;
        } else if (hi == 0xB0) {
            int vol = ch->volume - (code & 0x0F);
            if (vol < 0)
                vol = 0;
            ch->volume = vol;
            continue;
        }

        uint8_t reg6, reg7;
        uint32_t tbit, nbit;
        int wval;
        int nest;
        uint16_t offset;
        uint8_t t96;
        int8_t detune, diff;
        switch (code) {
        case 0xea:    /* S コマンド */
             ch->eg_width_base = ch->data_base[ch->data_offset++];
             if (ch->eg_width_base != 0) {
                 ch->eg_count_base = ch->data_base[ch->data_offset++];
                 ch->eg_delta_base = ch->data_base[ch->data_offset++];
                 ch->eg2_width_base = ch->data_base[ch->data_offset++];
                 ch->eg2_count_base = ch->data_base[ch->data_offset++];
            }
            continue;
        case 0xeb:    /* W コマンド */
            reg6 = ch->data_base[ch->data_offset++];
            psg_write(drv, AY_NOISEPER, reg6);
            drv->main.reg6_value = reg6;
            continue;
        case 0xec:    /* W+/- コマンド */
            wval = drv->main.reg6_value + ch->data_base[ch->data_offset++];
            if (wval > 31)
                wval = 31;
            if (wval < 0)
                wval = 0;
            reg6 = (uint8_t)wval;
            psg_write(drv, AY_NOISEPER, reg6);
            drv->main.reg6_value = reg6;
            continue;
        case 0xed:    /* P1 コマンド */
        case 0xee:    /* P2 コマンド */
        case 0xef:    /* P3 コマンド */
            tbit = 0x1u << ch->channel_index;
            nbit = 0x1u << (ch->channel_index + 3);
            reg7 = drv->main.reg7_value;
            if ((code & 0x01u) != 0) {
                /* トーン有効 */
                reg7 &= ~tbit;
            } else {
                /* トーン無効 */
                reg7 |= tbit;
            }
            if ((code & 0x02u) != 0) {
                /* ノイズ有効 */
                reg7 &= ~nbit;
            } else {
                /* ノイズ無効 */
                reg7 |= nbit;
            }
            psg_write(drv, AY_ENABLE, reg7);
            drv->main.reg7_value = reg7;
            continue;

        case 0xf0:    /* [ コマンド */
            nest = ch->flags & CH_F_NEST;
            if (nest >= 4) {
                /* コンパイル時にチェックされているはずだが一応 */
                (void)ch->data_base[ch->data_offset++];
                continue;
            }
            nest++;
            ch->flags = (ch->flags & 0xF8u) | nest;
            ch->nest_flag[nest - 1] = ch->data_base[ch->data_offset++];
            ch->l_backup = ch->l_default;
            ch->lplus_backup = ch->lplus_default;
            ch->octave_backup = (ch->octave_backup & 0xF0) | ch->octave;
            continue;
        case 0xf1:    /* ] コマンド (ジャンプ1バイト) */
        case 0xf2:    /* ] コマンド (ジャンプ2バイト) */
            offset  = ch->data_base[ch->data_offset++];
            if (code == 0xf2) {
                offset |= ch->data_base[ch->data_offset++] << 8;
            } else {
                offset |= 0xFF00u;
            }
            nest = ch->flags & CH_F_NEST;
            if (nest == 0) {
                /* コンパイル時にチェックされているはずだが一応 */
                continue;
            }
            if (--ch->nest_flag[nest - 1] == 0) {
                /* ネスト回数終了して脱出 */
                nest--;
                ch->flags = (ch->flags & 0xF8) | nest;
                continue;
            }
            /* ネストの最初に戻る */
            ch->data_offset += offset;
            ch->l_default = ch->l_backup;
            ch->lplus_default = ch->lplus_backup;
            ch->octave = ch->octave_backup & 0x0f;
            continue;
        case 0xf3:    /* : コマンド */
            offset  = ch->data_base[ch->data_offset++];
            offset |= ch->data_base[ch->data_offset++] << 8;
            nest = ch->flags & CH_F_NEST;
            if (nest > 4 || nest == 0) {
                /* コンパイル時にチェックされているはずだが一応 */
                continue;
            }
            if (ch->nest_flag[nest - 1] == 1) {
                /* 最後の繰り返しなのでネスト脱出 */
                ch->nest_flag[nest - 1] = 0;
                nest--;
                ch->flags = (ch->flags & 0xF8) | nest;
                ch->data_offset += offset;
            }
            continue;
        case 0xf4:    /* I コマンド */
            drv->main.i_command_value = ch->data_base[ch->data_offset++];
            continue;
        case 0xf5:    /* M コマンド */
             ch->vib_weight_base = ch->data_base[ch->data_offset++];
             ch->vib_count_base = ch->data_base[ch->data_offset++];
             ch->vib_amp_base = ch->data_base[ch->data_offset++];
             ch->vib_delta_base = ch->data_base[ch->data_offset++];
             /* 第4パラメータでビブラートフラグセットクリア */
            continue;
        case 0xf6:    /* N コマンド */
            /* ビブラート効果の有効／無効スイッチ */
            continue;
        case 0xf7:    /* L+ コマンド */
            ch->lplus_default = ch->data_base[ch->data_offset++];
            continue;
        case 0xf8:    /* T コマンド */
            t96 = ch->data_base[ch->data_offset++]; /* 96分音符長の 2ms回数 */
            (void)ch->data_base[ch->data_offset++]; /* P6ポートF6h値 (無視) */
            drv->main.tempo_val = t96;
            drv->main.bpm_x10   = calc_bpm_x10_from_t96(t96); /* ui 表示用 */
            continue;
        case 0xf9:    /* L コマンド */
            ch->l_default = ch->data_base[ch->data_offset++];
            continue;
        case 0xfa:    /* Q コマンド */
           ch->q_default = ch->data_base[ch->data_offset++];
           continue;
        case 0xfb:    /* U% コマンド */
            ch->detune = ch->data_base[ch->data_offset++];
            continue;
        case 0xfc:    /* U+/- コマンド */
            diff   = ch->data_base[ch->data_offset++];
            detune = (int8_t)ch->detune;
            if (((uint8_t)detune & 0x80u) != 0) {
                detune = (int8_t)((uint8_t)detune & ~0x80u);
                detune = -detune;
            }
            detune += diff;
            if (detune < 0) {
                detune = -detune;
                detune = (int8_t)((uint8_t)detune | 0x80u);
            }
            ch->detune = (uint8_t)detune;
            continue;
        case 0xfd:    /* M% コマンド */
             ch->vib_delta_base = ch->data_base[ch->data_offset++];
             /* 第4パラメータでビブラートフラグセットクリア */
            continue;
        case 0xfe:    /* J コマンド */
            ch->j_return_offset = ch->data_offset;
            ch->octave_backup = (ch->octave << 4) | (ch->octave_backup & 0x0f);
            continue;
        case 0xff:    /* エンドマーク */
            if (ch->j_return_offset != 0) {
                ch->data_offset = ch->j_return_offset;
                ch->octave = (ch->octave_backup >> 4) & 0x0f;
                continue;
            } else {
                ch->active = 0;
                return;
            }
        default:
            printf("ch %d unknown command: %02x\n", ch->channel_index, code);
            continue;
        }
    }
}

/* 2msごとの割り込み相当処理 */
void
psg_driver_tick(PSGDriver *drv)
{
    if (--drv->main.tempo_counter == 0) {
        for (int i = 0; i < 3; i++) {
            psg_channel_tick(drv, &drv->ch[i]);
        }
        drv->main.tempo_counter = drv->main.tempo_val;
    }

    /* フェード等があればここで main ワークを更新（後で実装） */
}
