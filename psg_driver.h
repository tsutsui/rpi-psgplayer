#ifndef PSG_DRIVER_H
#define PSG_DRIVER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* PSG レジスタ書き込みコールバック型 */
typedef void (*PSGWriteRegFn)(void *opaque, uint8_t reg, uint8_t val);

/* デモ画面表示用ノートデータ書き込みコールバック関数型 */
typedef void (*PSGNoteEventFn)(void *opaque, int ch,
                              uint8_t octave, uint8_t note,
                              uint8_t volume, uint16_t len,
                              uint8_t is_rest, uint16_t bpm_x10);

/* コマンド/ノートのオブジェクトデータ定義 */
#define F_NOTE          0x80u       /* 0:音符,休符, 1:コマンド */
#define F_TIE           0x40u       /* 0:タイなし, 1:タイあり */
#define F_LEN           0x30u       /* 音長フラグ */
#define F_LEN_L         0x00u       /* 00b:音長なし音符（Lコマンド音長） */
#define F_LEN_LPLUS     0x10u       /* 01b:音長なし音符（L+コマンド音長） */
#define F_LEN_1BYTE     0x20u       /* 10b:音長あり音符（音長１バイト） */
#define F_LEN_2BYTE     0x30u       /* 11b:音長あり音符（音長２バイト） */
#define F_PITCH         0x0fu       /* 0:休符, 1〜12:ド(C)〜シ(B) */

/* チャンネルワーク（Z80 IY+0x00〜0x27 に対応するログical構造） */
typedef struct PSGChannel {
    const uint8_t *data_base;       /* HL が指すオブジェクトデータ先頭 */
    uint16_t       data_offset;     /* HL 相当 */

    uint16_t       wait_counter;    /* 音長カウンタ */

    uint8_t        q_default;       /* Q カウンタ元値 */
    uint8_t        l_default;       /* L デフォルト音長 */
    uint8_t        lplus_default;   /* L+ デフォルト音長 */

    uint8_t        volume;          /* 現在ボリューム (0..15) */
    uint8_t        octave;          /* 現在オクターブ (1..8) */
    uint8_t        q_counter;       /* Q カウンタ現在値 */

    uint8_t        flags;           /* 各種フラグ (bit7..bit0) */
#define CH_F_REST       0x80u       /* 休符 */
#define CH_F_VIB_ON     0x40u       /* ビブラート有無 */
#define CH_F_VIB_PM     0x20u       /* ビブラート+/- */
#define CH_F_PSG_EG     0x10u       /* PSG-EGフラグ (第4パラメータ以降使用) */
#define CH_F_TIE        0x08u       /* タイフラグ */
#define CH_F_NEST       0x07u       /* ネストカウンタ */

    uint8_t        detune;          /* デチューン補正値 (bit7:±, bit6..0:値)*/

    uint8_t        nest_flag[4];    /* ループ／ネスト用フラグ */

    uint16_t       j_return_offset; /* J コマンド戻り先 HL オフセット */

    uint16_t       freq_value;      /* 現在の基準トーン値 */

    int16_t        vib_offset;      /* ビブラート補正値 */

    uint8_t        vib_wait_base;   /* ビブラートウェイト(p1)元値 */
    uint8_t        vib_wait_work;   /* ビブラートウェイトワーク */
    uint8_t        vib_count_base;  /* ビブラートカウンタ(p2)元値 */
    uint8_t        vib_count_work;  /* ビブラートカウンタワーク */
    uint8_t        vib_amp_base;    /* ビブラート振幅回数(p3)元値 */
    uint8_t        vib_amp_work;    /* ビブラート振幅回数ワーク */
    int8_t         vib_delta_base;  /* ビブラート増減値(p4)元値 */

    uint8_t        l_backup;
    uint8_t        lplus_backup;
    uint8_t        octave_backup;

    uint8_t        eg_count_base;   /* PSG-EGカウンタ(p2)元値  */
    uint8_t        eg_count_work;   /* PSG-EGカウンタワーク  */
    int8_t         eg_width_base;   /* PSG音量幅カウンタ(p1)元値 */
    int8_t         eg_width_work;   /* PSG音量幅カウンタワーク/2ndカウンタワーク */
    int8_t         eg_delta_base;   /* PSG-EG増減値(p3)元値 */
    int8_t         eg2_width_base;  /* 2nd音量幅(p4)元値 */
    int8_t         eg2_count_base;  /* 2ndカウンタ(p5)元値 */

    int8_t         volume_adjust;   /* 音量補正値 */

    /* C版独自の補助情報 */
    uint8_t        channel_index;   /* 0,1,2 など */
    uint8_t        active;          /* 0=停止, 1=再生中 */
} PSGChannel;

/* ドライバ全体のワーク */
typedef struct PSGMainWork {
    uint8_t tempo_val;
    uint8_t tempo_counter;

    uint8_t fade_value;
    int8_t  fade_step;
    uint8_t fade_active;

    uint8_t reg6_value;             /* レジスタ6 (AY_NOISEPER) 書き込み値 */
    uint8_t reg7_value;             /* レジスタ7 (AY_ENABLE) 書き込み値 */

    uint8_t i_command_value;        /* I コマンドで書き込まれた値 */

    uint16_t bpm_x10;
} PSGMainWork;

/* PSG ドライバ本体 */
typedef struct PSGDriver {
    PSGMainWork   main;
    PSGChannel    ch[3];            /* A/B/C の3チャンネル */
    PSGWriteRegFn write_reg;        /* PSG レジスタ書き込み */
    PSGNoteEventFn note_event;      /* デモ表示用ノートデータ書き込み */
    void          *opaque;          /* コールバックopaque */
    uint32_t      tick_count;       /* 経過 tick 数 */
} PSGDriver;

/* 初期化 */
void psg_driver_init(PSGDriver *drv,
                     PSGWriteRegFn reg_write_cb,
                     PSGNoteEventFn ui_note_cb,
                     void *opaque);

/* チャンネルにオブジェクトデータを設定 */
void psg_driver_set_channel_data(PSGDriver    *drv,
                                 int           ch_index,
                                 const uint8_t *data);

/* 再生開始 */
void psg_driver_start(PSGDriver *drv);

/* 再生停止 */
void psg_driver_stop(PSGDriver *drv);

/* 2msごとの割り込み相当処理 */
void psg_driver_tick(PSGDriver *drv);

/* I コマンド値取得 */
uint8_t psg_driver_get_i_command(const PSGDriver *drv);

#ifdef __cplusplus
}
#endif

#endif /* PSG_DRIVER_H */
