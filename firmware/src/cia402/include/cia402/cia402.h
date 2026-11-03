/*
 * cia402.h — CiA 402 设备子协议（驱动器语义）
 *
 * 这一层是"402 知识"的唯一所在地：控制字怎么拼、状态字怎么解、使能分几步、
 * 每一步等哪个状态位。上层只拿事实（使能了没有 / 哪一步失败了），
 * 拿不到 0x6040 / 0x6041 这些号码。
 *
 * 本层不知道：
 *   - 机械单位（脉冲当量换算归 axis）；
 *   - 应用状态机（PREPARING / RUNNING… 归 app）；
 *   - 心跳、NMT 运行状态（归 canopen / app）。
 *
 * SDO 只有一条通道，所以一个进程里同一时刻只应有一个节点在推进；谁的轴在
 * 推进由 axis 层排（依次使能）。本层只做"我这一步要发一个 SDO，等它回来"。
 */

#ifndef CIA402_CIA402_H
#define CIA402_CIA402_H

#include <canopen/master.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* DS 402 的 6 个状态（状态字的关键位组合）；UNKNOWN = 不匹配任何已知组合 */
enum cia402_state {
	CIA402_NOT_READY = 0,      /* 未准备好（bit6=0） */
	CIA402_SWITCH_ON_DISABLED, /* 禁止启动（0x0040） */
	CIA402_READY,              /* 准备启动（0x0021） */
	CIA402_SWITCHED_ON,        /* 已启动（0x0023） */
	CIA402_OP_ENABLED,         /* 可操作（0x0027） */
	CIA402_FAULT,              /* 故障（bit3=1） */
	CIA402_STATE_UNKNOWN
};

/* 使能推进到哪一步。上层通常只关心 ENABLED / FAILED */
enum cia402_step {
	CIA402_STEP_IDLE = 0,  /* 没被要求使能 */
	CIA402_STEP_SET_MODE,  /* 6060 = 1（PP），先定模式再使能 */
	CIA402_STEP_SHUTDOWN,  /* 6040 = 06 → 等 0x21 */
	CIA402_STEP_SWITCH_ON, /* 6040 = 07 → 等 0x23 */
	CIA402_STEP_ENABLE,    /* 6040 = 0F → 等 0x27 */
	CIA402_STEP_ENABLED,   /* 已到 Operation enabled */
	CIA402_STEP_FAILED     /* 中途出错（驱动器 abort 或状态字进故障） */
};

/* 一次 SDO 事务的进度。DONE / FAILED 是"结果已出、还没被推进器取走" */
enum cia402_tx {
	CIA402_TX_NONE = 0,
	CIA402_TX_READ,
	CIA402_TX_WRITE,
	CIA402_TX_DONE,
	CIA402_TX_FAILED
};

struct cia402_node {
	struct co_master *master; /* 只借用；SDO 客户端从它取 */
	uint8_t node_id;

	enum cia402_step step;
	uint8_t substep; /* 0 = 该发写；1 = 等写结果；2 = 等读结果 */
	enum cia402_tx tx;

	enum cia402_state state; /* 最近一次读到的状态字解析结果 */
	uint16_t statusword;     /* 最近一次读到的原始状态字 */
	uint32_t abort_code; /* 失败时驱动器回的 SDO abort；0 = 不是 abort 引起的 */

	uint32_t last_ms; /* 上一拍时刻，用来算上游要的 timeDifference */
	uint8_t started;
};

/* 纯函数：状态字 → 402 状态。可单独测，不碰总线 */
enum cia402_state cia402_parse(uint16_t statusword);
const char *cia402_state_name(enum cia402_state s);

void cia402_init(struct cia402_node *n, struct co_master *m, uint8_t node_id);

/* 请求上使能：0 = 已受理；1 = 已经在使能/已使能/已失败（不重复发） */
int cia402_request_enable(struct cia402_node *n);

/* 每拍推进一步；内部按需发起 SDO 读写。与"是否有请求"无关地调用 */
void cia402_poll(struct cia402_node *n, uint32_t now_ms);

int cia402_is_enabled(const struct cia402_node *n);
int cia402_failed(const struct cia402_node *n);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CIA402_CIA402_H */
