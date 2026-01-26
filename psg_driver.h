#ifndef PSG_DRIVER_H
#define PSG_DRIVER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* PSG レジスタ書き込みコールバック型 */
typedef void (*PSGWriteRegFn)(void *user, uint8_t reg, uint8_t val);

/* チャンネルワーク（Z80 IY+0x00〜0x27 に対応するログical構造） */
typedef struct PSGChannel {
    const uint8_t *data_base;   /* HL が指すオブジェクトデータ先頭 */
    uint16_t       data_offset; /* HL 相当 */

    uint16_t       wait_counter;    /* 音長カウンタ */

    uint8_t        q_default;       /* Q カウンタ元値 */
    uint8_t        l_default;       /* L デフォルト音長 */
    uint8_t        lplus_default;   /* L+ デフォルト音長 */

    uint8_t        volume;          /* 現在ボリューム (0..15) */
    uint8_t        octave;          /* 現在オクターブ (1..8) */
    uint8_t        q_counter;       /* Q カウンタ現在値 */

    uint8_t        flags;           /* 各種フラグ (bit7..bit0) */

    int8_t         detune;          /* デチューン補正値 */

    uint8_t        nest_flag[4];    /* ループ／ネスト用フラグ */

    uint16_t       j_return_offset; /* J コマンド戻り先 HL オフセット */

    uint16_t       freq_value;      /* 現在の基準トーン値 */

    int16_t        vib_offset;      /* ビブラート補正値 */

    uint8_t        vib_weight_base;
    uint8_t        vib_weight_work;
    uint8_t        vib_count_base;
    uint8_t        vib_count_work;
    uint8_t        vib_amp_base;
    uint8_t        vib_amp_work;
    int8_t         vib_delta_base;

    uint8_t        l_backup;
    uint8_t        lplus_backup;
    uint8_t        octave_backup;

    uint8_t        eg_count_base;
    uint8_t        eg_count_work;
    int8_t         eg_width_base;
    int8_t         eg_width_work;
    int8_t         eg_delta_base;
    int8_t         eg2_width_base;
    int8_t         eg2_count_base;

    int8_t         volume_adjust;

    /* C版独自の補助情報 */
    uint8_t        channel_index;   /* 0,1,2 など */
    uint8_t        active;          /* 0=停止, 1=再生中 */
} PSGChannel;

/* ドライバ全体のワーク */
typedef struct PSGMainWork {
    uint8_t fade_value;
    int8_t  fade_step;
    uint8_t fade_active;

    uint8_t i_command_value;        /* I コマンドで書き込まれた値 */
} PSGMainWork;

/* PSG ドライバ本体 */
typedef struct PSGDriver {
    PSGMainWork   main;
    PSGChannel    ch[3];            /* A/B/C の3チャンネル */
    PSGWriteRegFn write_reg;        /* PSG レジスタ書き込み */
    void         *write_user;       /* コールバックコンテキスト */
    uint32_t      tick_count;       /* 経過 tick 数 */
} PSGDriver;

/* 初期化 */
void psg_driver_init(PSGDriver *drv,
                     PSGWriteRegFn write_cb,
                     void *user);

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

/* AY/YM2149 register numbers */
enum {
    AY_AFINE    = 0,
    AY_ACOARSE  = 1,
    AY_BFINE    = 2,
    AY_BCOARSE  = 3,
    AY_CFINE    = 4,
    AY_CCOARSE  = 5,
    AY_NOISEPER = 6,
    AY_ENABLE   = 7,
    AY_AVOL     = 8,
    AY_BVOL     = 9,
    AY_CVOL     = 10,
    AY_EFINE    = 11,
    AY_ECOARSE  = 12,
    AY_ESHAPE   = 13,
    AY_PORTA    = 14,
    AY_PORTB    = 15
};

#ifdef __cplusplus
}
#endif

#endif /* PSG_DRIVER_H */
