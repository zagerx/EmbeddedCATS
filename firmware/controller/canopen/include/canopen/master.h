/*
 * master.h — CANopen 主站句柄（对外接口）
 *
 * 实现是本层自己写的 DS 301 子集（NMT + 心跳 + SDO 客户端），
 * 不再依赖第三方协议栈。对外语义不变：绑定一条总线、登记节点、切 NMT 状态、
 * 每拍推进、查节点心跳。每个登记过的节点还带一份"对象访问"槽（见 node.h），
 * 由本层每拍推进。
 *
 * 单实例以外无任何全局状态；换实现时上层不用改。
 */

#ifndef CANOPEN_MASTER_H
#define CANOPEN_MASTER_H

#include <bus/can_io.h>
#include <canopen/node.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CO_MASTER_MAX_NODES 16

/* NMT 广播站号：0 表示总线上所有节点（DS 301 的"所有节点"约定） */
#define CO_MASTER_ALL_NODES 0U

/* 心跳超时：从站心跳周期 500 ms，取 1.5 倍。超时以内收到过心跳即算在位 */
#define CO_MASTER_HB_TIMEOUT_MS 750u

/* 每个已登记节点的协议状态（只记心跳，够回答"还活着吗"） */
struct co_master_node_state {
	uint32_t hb_last_ms; /* 最近一次收到心跳（或 boot-up）的时刻 */
	uint8_t hb_seen;     /* 至少收到过一次 */
};

/*
 * 主站句柄。内部状态全在这里，零 malloc：调用方给一块静态内存即可。
 * node[] 与 nstate[] 同序，都与 node_id[] 对得上。
 */
struct co_master {
	struct can_io *bus; /* 只借用；通道由壳创建与关闭 */
	uint32_t now_ms;    /* 上一拍的时刻（心跳超时按它算） */
	uint8_t node_count;
	uint8_t node_id[CO_MASTER_MAX_NODES];     /* 登记顺序 */
	struct co_node node[CO_MASTER_MAX_NODES]; /* 每节点的对象访问槽 */
	struct co_master_node_state nstate[CO_MASTER_MAX_NODES];
};

/* 初始化协议栈（上游 CO_new/CO_CANinit/CO_CANopenInit）并绑定总线。不发任何帧 */
void co_master_init(struct co_master *m, struct can_io *bus);

/* 登记一个节点（同时初始化它的对象访问槽）。重复登记、超上限返回 0 */
int co_master_add_node(struct co_master *m, uint8_t node_id);

/* 取已登记节点的句柄；没登记过返回 NULL（上层据此判"这个轴不可用"） */
struct co_node *co_master_node(struct co_master *m, uint8_t node_id);

/* 只列工程真正用到的两个状态；要用别的（stop / pre-op / reset node）时再加 */
enum co_master_state {
	CO_MASTER_START = 0,           /* → DS301 Start Remote Node */
	CO_MASTER_RESET_COMMUNICATION, /* → Reset Communication */
};

/* 站号 0 表示总线上所有节点（广播）。0 = 已交给协议栈，非 0 = 发不出去 */
int co_master_set_state(struct co_master *m, uint8_t node_id, enum co_master_state state);

/*
 * 推进一次：抽干总线 → 分派 → 推进各节点的在途事务 → 心跳超时判定。
 * 每拍无条件调用，与运行状态无关。
 */
void co_master_poll(struct co_master *m, uint32_t now_ms);

/*
 * 查一个节点的心跳是否正常。
 * 返回 0 = 正常；返回非 0 = 不正常。
 * 判据：登记过、并且最近 CO_MASTER_HB_TIMEOUT_MS 内收到过心跳。
 * 未登记或还没收到过第一帧，都算不正常。
 */
int co_master_node_alive(const struct co_master *m, uint8_t node_id);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CANOPEN_MASTER_H */
