/*
 * node.h — 一个 CANopen 节点的"对象访问"面（DS 301 SDO 客户端）
 *
 * 为什么在这里：SDO 是 CANopen 自带的服务，"一个节点只有一对 0x600/0x580、
 * 同时只能有一个事务"是协议事实。所以事务槽跟着节点走，由主站每拍推进；
 * 上层（如 CiA 402）只说"读/写哪个对象"，不知道传输怎么走、也不需要推它。
 *
 * 用法（非阻塞，控制线程一拍一步）：
 *     co_node_read(n, 0x1000, 0, 200);   返回 0 已受理 / 1 忙 / -1 参数错
 *     ... 每拍由 co_master_poll() 推进（发帧、收应答、判超时）...
 *     co_node_result(n, buf, &len);      返回 1 成功 / -1 失败 / 0 在途
 *
 * 本层不知道：
 *   - 什么时候该读（那是业务流程/设备层的事）；
 *   - 读回来的字节是什么意思（对象语义由上层标准决定）。
 */

#ifndef CANOPEN_NODE_H
#define CANOPEN_NODE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct co_master;

/* 一次事务的进度；DONE / FAILED 是"结果已出、还没被取走" */
enum co_sdo_ch_state {
	CO_SDO_CH_IDLE = 0,
	CO_SDO_CH_QUEUED,   /* 已受理，下一拍才发帧 */
	CO_SDO_CH_WAIT_RSP, /* 请求已发出，等应答 */
	CO_SDO_CH_DONE,     /* 成功结果待取 */
	CO_SDO_CH_FAILED    /* 失败结果待取 */
};

/*
 * 一次 SDO 事务的全部状态（"事务槽"）。
 * 字段由 sdo_channel.c 解释；上层只读 data/len，失败原因读 code。
 *
 * 为什么把结构体放在公共头：事务槽跟着节点走（协议事实）。
 * 但它只有本层会写，上层拿到的只是只读结果。
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

/*
 * 节点句柄：主站为每个已登记节点持有一份（见 struct co_master.node[]）。
 * 上层只借指针，不自己定义、也不自己推进。
 */
struct co_node {
	struct co_master *master; /* 只借用；发送由主站完成 */
	uint8_t node_id;
	struct co_sdo_ch ch; /* 事务槽 */
	uint32_t last_code;  /* 最近一次失败原因，取走结果后仍可查 */
};

void co_node_init(struct co_node *n, struct co_master *m, uint8_t node_id);

/* 装配是否完成（有主站且站号非 0）；未就绪时任何发起都会返回 -1 */
int co_node_ready(const struct co_node *n);

/* 发起一次读/写：0 = 已发出；1 = 忙（保持调用方原相位）；-1 = 发不出去 */
int co_node_read(struct co_node *n, uint16_t index, uint8_t sub, uint16_t timeout_ms);
int co_node_write(struct co_node *n, uint16_t index, uint8_t sub, uint32_t value, uint8_t size,
		  uint16_t timeout_ms);

/* 取结果：1 = 成功（数据拷进 buf，字节数写进 len），-1 = 失败，0 = 还在途 */
int co_node_result(struct co_node *n, uint8_t *buf, uint8_t *len);

/* 通道上有没有在途事务 */
int co_node_busy(const struct co_node *n);

/* 关掉在途事务（安全动作抢占传输时用） */
void co_node_close(struct co_node *n);

/* 最近一次失败时从站回的 abort code；0 = 不是 abort 引起的（超时/参数错） */
uint32_t co_node_abort_code(const struct co_node *n);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CANOPEN_NODE_H */
