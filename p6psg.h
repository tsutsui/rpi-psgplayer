/*
 * p6psg.h
 *  PC-6001 PSG音源ドライバ用データファイル読み出し定義
 */

#ifndef P6PSG_H
#define P6PSG_H

#include <stddef.h>
#include <stdint.h>

typedef struct p6psg p6psg_t;

#define P6PSG_CH_COUNT 3

typedef enum p6psg_channel {
    P6PSG_CH_A = 0,
    P6PSG_CH_B = 1,
    P6PSG_CH_C = 2
} p6psg_channel_t;

typedef struct {
    const uint8_t *ptr;  /* 演奏データ開始アドレス */
    size_t len;          /* 演奏データ長さ (計算値) */
} p6psg_channel_data_t;

typedef struct {
    p6psg_channel_data_t ch[P6PSG_CH_COUNT];
} p6psg_channel_dataset_t;

/* 公開関数 */

/* オブジェクト生成 */
p6psg_t *p6psg_create(void);

/* オブジェクト破棄 */
void p6psg_destroy(p6psg_t *psg);

/* 演奏データオブジェクト読み込みおよびパース */
int p6psg_load(p6psg_t *psg, const char *path, p6psg_channel_dataset_t *channels);

/* エラーメッセージ */
const char *p6psg_last_error(const p6psg_t *psg);

#endif /* P6PSG_H */
