/* 主站：一条 SocketCAN 通道、节点表及 CANopenNode 的收发推进。 */

#ifndef MASTER_MASTER_H
#define MASTER_MASTER_H

#include "CANopen.h" /* CANopenNode 的 CO_t / CO_SDOclient_t */
#include "co_driver_cats.h"

#include <pthread.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MASTER_MAX_NODES 16

/* 本主站自己的站号：CANopenNode 要求真实值（未配置时 CO_process 直接返回） */
#define MASTER_NODE_ID 0x7Fu

/* NMT 命令（与 CANopenNode 的 CO_NMT_command_t 对应，避免上层 include 栈头） */
enum master_nmt_cmd {
	MASTER_NMT_START = 1,
	MASTER_NMT_RESET_COMM = 130,
};

/* 站号 0 = 总线上所有从站 */
#define MASTER_NMT_ALL_NODES 0u

/*
 * 主站句柄：零 malloc（CANopenNode 用 CO_USE_GLOBALS 编译，静态实例）。
 * node_id[] 与 CANopenNode 的 SDOclient[] 同序：第 i 个登记的从站用第 i 条
 * SDO 通道（OD 里 0x1280/0x1281 就是这么配的）。
 */
struct master {
	uint32_t now_ms;  /* 上一拍的时刻 */
	uint32_t last_us; /* 上一拍的微秒时刻（CO_process 要 timeDifference） */
	uint8_t node_count;
	uint8_t node_id[MASTER_MAX_NODES];

	/* 待发 NMT 命令：任意线程只登记，I/O 线程在 master_send_step() 里发出 */
	pthread_mutex_t nmt_lock;
	uint8_t nmt_pending;
	uint8_t nmt_node_id;
	enum master_nmt_cmd nmt_cmd;

	CO_t *co;               /* CANopenNode 实例（静态） */
	struct co_cats_can can; /* SocketCAN 移植层句柄：借用 fd + 收发错误标记 */
};

/* 绑定借用的 SocketCAN fd 并初始化 CANopenNode，不发任何帧 */
void master_init(struct master *m, int fd);

/* 登记一个从站（第 i 个登记 ⇒ 第 i 条 SDO 通道）。重复登记、超上限返回 0 */
int master_add_node(struct master *m, uint8_t node_id);

/* 已登记的从站数量 / 第 idx 个从站的站号（越界返回 0） */
uint8_t master_node_count(const struct master *m);
uint8_t master_node_id(const struct master *m, uint8_t idx);

/* 取某个从站的 SDO 客户端；没登记过返回 NULL */
CO_SDOclient_t *master_sdo(struct master *m, uint8_t node_id);

/*
 * 登记一条待发 NMT 命令；站号 0 = 总线上所有从站。任意线程可调。
 * 只登记不发送：帧由 I/O 线程在下一拍 master_send_step() 开头发出，
 * 保证“帧只从 I/O 线程出去”。同一拍只保留最后一条登记。
 * 0 = 已登记，非 0 = 主站未初始化。
 */
int master_request_nmt(struct master *m, uint8_t node_id, enum master_nmt_cmd cmd);

/*
 * 一拍两段（必须在同一个线程里按顺序调：总线只有一个收发者）：
 *   收帧交给栈，随后推进 CO_process（心跳超时、SDO 超时、NMT、发帧）。
 * 中间可以插入 SDO 请求与结果搬移。
 */
void master_recv_step(struct master *m, uint32_t now_ms);
void master_send_step(struct master *m, uint32_t now_ms);

/* 心跳是否正常：0 = 正常（ACTIVE）；非 0 = 不正常（未登记/未知/超时） */
int master_node_alive(const struct master *m, uint8_t node_id);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MASTER_MASTER_H */
