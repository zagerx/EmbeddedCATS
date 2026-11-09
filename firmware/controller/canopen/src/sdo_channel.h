/*
 * sdo_channel.h — 一次 SDO 事务的纯状态机（层内用，不对外）
 *
 * 零 I/O、零锁、零时间源：帧从参数进来，要发的帧从参数出去，时间由调用方给。
 * 所以可以脱离总线单测（见 tools/canopen_selftest）。
 *
 *   IDLE ──submit──> QUEUED ──advance──> WAIT_RSP ──应答──> DONE / FAILED ──take──> IDLE
 *                                         │
 *                                         └─ 超时 / 取消 ──> 发 abort 帧 → FAILED
 *
 * 两条协议约束（都写在实现里）：
 *   1. 超时与取消都要发 abort 帧（0x80）——不发的话，服务端可能稍后才回一帧，
 *      被下一笔事务误认成自己的应答；SDO 报文没有事务号，靠状态机补。
 *   2. 被作废事务的 index/sub 记成"陈旧键"，在一个静默窗口内收到的同键帧一律丢弃。
 */

#ifndef CANOPEN_SDO_CHANNEL_H
#define CANOPEN_SDO_CHANNEL_H

#include <canopen/sdo.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* advance 的返回值：本层要不要发帧、发哪一帧 */
enum co_sdo_ch_action {
	CO_SDO_CH_ACT_NONE = 0,  /* 无事可做 */
	CO_SDO_CH_ACT_SEND_REQ,  /* out 里是 SDO 请求帧（0x600+id） */
	CO_SDO_CH_ACT_SEND_ABORT /* out 里是 SDO abort 帧（0x600+id） */
};

/* 静默窗口的下限：作废事务的迟到应答在这个窗口内都会被丢掉 */
#define CO_SDO_CH_STALE_MS 500u

void co_sdo_ch_init(struct co_sdo_ch *ch);

/* 受理一笔事务：0 = 已受理（进 QUEUED，下一拍 advance 才发帧）；1 = 通道忙 */
int co_sdo_ch_submit(struct co_sdo_ch *ch, int is_write, uint16_t index, uint8_t sub,
		     uint32_t value, uint8_t size, uint32_t txn_timeout_ms);

/* 每拍调用：可能要发请求帧 / abort 帧 */
enum co_sdo_ch_action co_sdo_ch_advance(struct co_sdo_ch *ch, uint32_t now_ms, uint8_t out[8]);

/* 收到 0x580+id 的一帧 */
void co_sdo_ch_on_frame(struct co_sdo_ch *ch, const uint8_t in[8], uint32_t now_ms);

/* 主动取消在途事务（安全动作用）：下一拍会补发 abort 帧 */
void co_sdo_ch_cancel(struct co_sdo_ch *ch);

/* 帧没能发出去（总线错误）：直接判失败，不建事务 */
void co_sdo_ch_local_fail(struct co_sdo_ch *ch, uint32_t code);

/* 取结果：1 = 成功（data/len 有效）；-1 = 失败（code 有效）；0 = 在途 / 空闲 */
int co_sdo_ch_take(struct co_sdo_ch *ch, uint8_t *data, uint8_t *len, uint32_t *code);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CANOPEN_SDO_CHANNEL_H */
