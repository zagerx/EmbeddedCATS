/*
 * master.h — 主站实体：一条总线 + 若干从站
 *
 * 这个头回答的是"这台机器上有哪几个站、它们还活着吗、我要让谁上线"，
 * 不回答"帧长什么样"（那些在 canopen/ 的四个头里）。
 *
 * 它持有并独占总线：整个进程只有 master_recv_step() 收帧、只有 master_send() 发帧。
 * 每个登记过的从站各有一份 SDO 客户端（struct co_sdo），所以多站访问是并行的。
 *
 * 本层不知道：402、对象语义（drive/ds402）、"轴""运动"这些业务词、线程。
 */

#ifndef MASTER_MASTER_H
#define MASTER_MASTER_H

#include <bus/can_io.h>
#include <canopen/hb.h>
#include <canopen/nmt.h>
#include <canopen/sdo.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MASTER_MAX_NODES 16

/*
 * 心跳超时：从站心跳周期 500 ms，取 1.5 倍。
 * 这是"本机愿意等多久"，不是协议常量，所以放在主站而不是 canopen。
 */
#define MASTER_HB_TIMEOUT_MS 750u

/* 一个已登记的从站：站号 + 对象访问 + 心跳状态 */
struct master_node {
	uint8_t node_id;
	struct co_sdo sdo;
	struct co_hb hb;
};

/*
 * 主站句柄：内部状态全在这里，零 malloc（调用方给一块静态内存即可）。
 * node[] 按下标与 node_id 对得上，前 node_count 个有效。
 */
struct master {
	struct can_io *bus; /* 只借用；通道由壳创建与关闭 */
	uint32_t now_ms;    /* 上一拍的时刻（心跳超时按它算） */
	uint8_t node_count;
	struct master_node node[MASTER_MAX_NODES];
};

/* 绑定总线并清空节点表。不发任何帧 */
void master_init(struct master *m, struct can_io *bus);

/* 登记一个从站（同时备好它的 SDO 客户端与心跳状态）。重复登记、超上限返回 0 */
int master_add_node(struct master *m, uint8_t node_id);

/* 已登记的从站数量 / 第 idx 个从站的站号（越界返回 0） */
uint8_t master_node_count(const struct master *m);
uint8_t master_node_id(const struct master *m, uint8_t idx);

/* 取某个从站的 SDO 客户端；没登记过返回 NULL（上层据此判"这个轴不可用"） */
struct co_sdo *master_sdo(struct master *m, uint8_t node_id);

/* 总线句柄（壳取当前时刻用）；未装配返回 NULL */
struct can_io *master_bus(struct master *m);

/* 发一条 NMT 命令：站号 0 = 总线上所有从站。0 = 已发出，非 0 = 发不出去 */
int master_set_state(struct master *m, uint8_t node_id, enum co_nmt_cmd cmd);

/*
 * 一拍分两步，中间允许插入别的事（宿主正是这么排的）：
 *
 *   ① master_recv_step()   抽帧 + 按 COB-ID 分派（心跳 / SDO 应答）
 *   ② （宿主：od_sdo_io_step 把服务线程的请求提交给协议）
 *   ③ master_send_step()   推进各站在途事务，该发的帧发出去
 *
 * 两步共用同一份“上一拍时刻”（心跳超时按它算）。
 * **两步必须在同一个线程里按顺序调**：总线只有一个收发者。
 */

/* ① 收：把总线上已到的帧抽干并分派（不阻塞） */
void master_recv_step(struct master *m, uint32_t now_ms);

/* ③ 发：每个从站的事务槽推进一步（要发请求帧 / abort 帧就发） */
void master_send_step(struct master *m, uint32_t now_ms);

/*
 * 查一个从站的心跳是否正常：0 = 正常；非 0 = 不正常。
 * 判据：登记过、并且最近 MASTER_HB_TIMEOUT_MS 内收到过心跳。
 * 未登记或还没收到过第一帧，都算不正常。
 */
int master_node_alive(const struct master *m, uint8_t node_id);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MASTER_MASTER_H */
