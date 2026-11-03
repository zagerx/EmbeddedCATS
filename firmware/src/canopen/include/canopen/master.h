/*
 * master.h — CANopen 主站句柄（对外接口）
 *
 * 实现是 CANopenNode（子模块），但这一层只暴露"主站"语义：
 * 绑定一条总线、登记节点、发 NMT、每拍推进、查某个节点的心跳是否正常。
 *
 * 上游类型一律不出现在本头里 —— 换实现时上层不用改。
 * 单实例：一个进程只允许一个主站（上游 CO_USE_GLOBALS 决定的）。
 */

#ifndef CANOPEN_MASTER_H
#define CANOPEN_MASTER_H

#include <canopen/can_io.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CO_MASTER_MAX_NODES 16

/*
 * 主站句柄。协议栈内部对象藏在 master.c 里（静态分配，零 malloc），
 * 这里只留调用方真正需要的数据。
 *
 * node_id[] 的顺序 = 心跳消费者的监控下标：第 n 次 add_node 对应 OD 里
 * 0x1016 的第 n+1 个子索引。所以**登记顺序必须与 OD 0x1016 的配置顺序一致**
 * （OD.c 里配的是 :1=站号 1、:2=站号 2），否则 add_node 会拒绝。
 */
struct co_master {
	struct can_io *bus; /* 只借用；通道由壳创建与关闭 */
	uint8_t node_count;
	uint8_t node_id[CO_MASTER_MAX_NODES]; /* 登记顺序 = 心跳消费者的监控下标 */
};

/* 初始化协议栈（上游 CO_new/CO_CANinit/CO_CANopenInit）并绑定总线。不发任何帧 */
void co_master_init(struct co_master *m, struct can_io *bus);

/* 登记一个要监控心跳的节点。重复登记、超上限或 OD 里没有对应槽位返回 0 */
int co_master_add_node(struct co_master *m, uint8_t node_id);

/* NMT 命令：command 用 DS301 的原始命令字（0x01 start / 0x80 pre-op / 0x81 / 0x82） */
int co_master_nmt(struct co_master *m, uint8_t node_id, uint8_t command);

/*
 * 推进一次：抽干总线 → 分派给协议对象 → 跑协议栈。
 * 每拍无条件调用，与运行状态无关。
 */
void co_master_poll(struct co_master *m, uint32_t now_ms);

/*
 * 查一个节点的心跳是否正常。
 * 返回 0 = 正常；返回非 0 = 不正常。
 * 判据是该节点的心跳消费者槽位处于 ACTIVE（在 OD 0x1016 配的超时窗口内收到过心跳）；
 * 未登记、OD 0x1016 里没配这个站号、已掉线，一律算不正常。
 */
int co_master_node_alive(const struct co_master *m, uint8_t node_id);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CANOPEN_MASTER_H */
