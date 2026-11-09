/*
 * sdo.h — SDO 客户端（一个从站一份）
 *
 * DS 301 的 SDO 是 CANopen 自带的服务："一个节点只有一对 0x600/0x580、
 * 同时只能有一个事务"是协议事实。所以**事务槽跟着从站走**：
 *   struct co_sdo  = 站号 + 事务槽 + 一个"把帧发出去"的回调
 *   struct co_sdo_ch = 纯状态机（零 I/O、零锁、零时间源，见 sdo_channel.c）
 *
 * 帧怎么发出去不归本层管：主站在装配时注入 tx 回调（本层不 include bus、
 * 不碰 socket）。上层只说"读/写哪个对象"，不推进传输。
 *
 * 用法（非阻塞）：
 *     co_sdo_read(sdo, 0x6041, 0, 200);       返回 0 已受理 / 1 忙 / -1 参数错
 *     co_sdo_advance(sdo, now_ms);            每拍由主站调用
 *     co_sdo_result(sdo, buf, &len);          1 成功 / -1 失败 / 0 在途
 *
 * 本层不知道：什么时候该读（业务流程决定）、字节是什么意思（对象语义由上层
 * 标准决定）、这台机器上有几个从站（主站的表）。
 */

#ifndef CANOPEN_SDO_H
#define CANOPEN_SDO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 一次事务的进度；DONE / FAILED 是"结果已出、还没被取走" */
enum co_sdo_ch_state {
	CO_SDO_CH_IDLE = 0,
	CO_SDO_CH_QUEUED,   /* 已受理，下一拍才发帧 */
	CO_SDO_CH_WAIT_RSP, /* 请求已发出，等应答 */
	CO_SDO_CH_DONE,     /* 成功结果待取 */
	CO_SDO_CH_FAILED    /* 失败结果待取 */
};

/*
 * 事务槽：一次 SDO 事务的全部状态。
 * 字段由 sdo_channel.c 解释；上层只读 data/len，失败原因读 code。
 */
struct co_sdo_ch {
	enum co_sdo_ch_state state;
	uint8_t is_write;
	uint16_t index;          /* 本次事务访问的对象 */
	uint8_t sub;             /* 本次事务访问的子索引 */
	uint32_t value;          /* 写事务的值（小端） */
	uint8_t size;            /* 写事务的字节数 1..4 */
	uint8_t data[4];         /* 读事务的结果（小端） */
	uint8_t len;             /* data 里有效字节数 */
	uint32_t code;           /* 失败原因：从站 abort 或本层判定；0 = 被取消 */
	uint32_t txn_timeout_ms; /* 本次事务的等待上限（从帧发出算起） */
	uint32_t sent_ms;        /* 请求帧发出的时刻 */
	uint8_t abort_pending;   /* 已决定作废，下一拍补发 abort 帧 */
	uint16_t stale_index;    /* 被作废事务的键：静默窗口内同键帧一律丢弃 */
	uint8_t stale_sub;
	uint8_t stale_valid;
	uint32_t stale_until_ms;
};

/* 发一帧：返回 0 = 已发；非 0 = 发不出去。ctx 由注入方决定 */
typedef int (*co_sdo_tx_fn)(void *ctx, uint16_t cob_id, const uint8_t data[8]);

/*
 * SDO 客户端：主站为每个已登记从站持有一份，上层只借指针、不自己推进。
 */
struct co_sdo {
	uint8_t node_id;
	co_sdo_tx_fn tx; /* 发帧能力，由主站注入 */
	void *tx_ctx;
	struct co_sdo_ch ch; /* 事务槽 */
	uint32_t last_code;  /* 最近一次失败原因，取走结果后仍可查 */
};

void co_sdo_init(struct co_sdo *s, uint8_t node_id, co_sdo_tx_fn tx, void *tx_ctx);

/* 装配是否完成（站号非 0 且有发帧回调）；未就绪时任何发起都返回 -1 */
int co_sdo_ready(const struct co_sdo *s);

/* 发起一次读/写：0 = 已受理；1 = 忙（保持调用方原相位）；-1 = 发不出去 */
int co_sdo_read(struct co_sdo *s, uint16_t index, uint8_t sub, uint16_t timeout_ms);
int co_sdo_write(struct co_sdo *s, uint16_t index, uint8_t sub, uint32_t value, uint8_t size,
		 uint16_t timeout_ms);

/* 取结果：1 = 成功（数据拷进 buf，字节数写进 len），-1 = 失败，0 = 还在途 */
int co_sdo_result(struct co_sdo *s, uint8_t *buf, uint8_t *len);

/* 通道上有没有在途事务 */
int co_sdo_busy(const struct co_sdo *s);

/* 关掉在途事务（安全动作抢占传输时用）：下一拍补发 abort 帧 */
void co_sdo_close(struct co_sdo *s);

/* 最近一次失败时从站回的 abort code；0 = 不是 abort 引起的（超时/参数错） */
uint32_t co_sdo_abort_code(const struct co_sdo *s);

/* 每拍推进：需要发请求帧 / abort 帧时通过 tx 回调发出 */
void co_sdo_advance(struct co_sdo *s, uint32_t now_ms);

/* 收到 0x580+id 的一帧：交给事务槽判定 */
void co_sdo_on_frame(struct co_sdo *s, const uint8_t data[8], uint32_t now_ms);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CANOPEN_SDO_H */
