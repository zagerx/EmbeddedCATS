/*
 * hb.h — 心跳消费者（从站 → 主站，0x700 + id）
 *
 * 一个从站一份状态，只记"最近一次收到的时刻"和"有没有收到过"。
 * 超时判定不做滚动计时器，而是"要用的时候和当前时刻比一下"，
 * 所以不依赖定时器、也不会因为调用次数变化而漂移。
 *
 * 超时时长**不在本层**：那是"主站愿意等多久"，由调用方传进来。
 */

#ifndef CANOPEN_HB_H
#define CANOPEN_HB_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct co_hb {
	uint32_t last_ms;  /* 最近一次收到心跳（或 boot-up）的时刻 */
	uint8_t seen;      /* 至少收到过一次 */
	uint8_t nmt_state; /* 心跳带过来的 NMT 状态字节 */
};

void co_hb_reset(struct co_hb *hb);

/* 收到一帧心跳（8 字节里的 data[0] 是 NMT 状态） */
void co_hb_on_frame(struct co_hb *hb, const uint8_t data[8], uint32_t now_ms);

/* 返回 0 = 在位；1 = 不在位（还没收到过第一帧，或距上次超过 timeout_ms） */
int co_hb_lost(const struct co_hb *hb, uint32_t now_ms, uint32_t timeout_ms);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CANOPEN_HB_H */
