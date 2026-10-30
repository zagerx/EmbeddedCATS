/*
 * master.h — CANopen 主站（内核层）
 *
 * 命名约定（方案 A，**不用 typedef**）：
 *   struct co_master    = 协议意义上的“主站”：一条总线 + 若干节点 + NMT 状态。
 *   struct co_node      = 总线上的一个 CANopen 节点；本项目的双轴驱动器 = 节点 1 与节点 2。
 *   enum co_nmt_state   = 每个节点的 NMT 状态（Boot-up / Pre-op / Operational / Stopped）。
 *
 * 这里**没有** CiA 402 的东西：驱动器的 402 状态（Ready / Operation enabled / Fault）
 * 属于设备层，节点“掉线”和“驱动器报警”必须分开存，否则报错定位不了。
 *
 * 帧池（pool）：收发共用一条总线，周期帧只可能是“上一个周期没取走的应答”或
 * “心跳/TPDO”。pool 数组在本结构内静态分配，固定上限，不动态分配内存。
 */

#ifndef CANOPEN_MASTER_H
#define CANOPEN_MASTER_H

#include <canopen/can_io.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CO_MASTER_MAX_NODES 16
#define CO_MASTER_POOL_SIZE 16 /* 每拍从总线捞到的帧暂存；调满要判满并丢 */

/* DS 301 7.3.2：NMT 状态值与心跳/boot-up 帧的 data[0] 一致，可直接比较 */
enum co_nmt_state {
	CO_NMT_BOOTUP = 0x00,
	CO_NMT_STOPPED = 0x04,
	CO_NMT_OPERATIONAL = 0x05,
	CO_NMT_PREOPERATIONAL = 0x7F
};

struct co_node {
	uint8_t node_id;
	enum co_nmt_state nmt;
	uint32_t last_heartbeat_ms; /* 最近一次收到 0x700+id 的时刻 */
};

struct co_master {
	struct can_io *bus; /* 只借用；通道由壳创建与关闭 */

	struct co_node nodes[CO_MASTER_MAX_NODES];
	uint8_t node_count;

	/* 本拍收到的帧：心跳等被动消息由 SDO 客户端从池里消费 */
	struct can_io_frame pool[CO_MASTER_POOL_SIZE];
	uint8_t pool_count;
};

/* 只做初始化；不发任何帧，也不改节点状态 */
void co_master_init(struct co_master *m, struct can_io *bus);

/* 登记一个节点。重复登记返回 0；达到上限返回 0 */
int co_master_add_node(struct co_master *m, uint8_t node_id);

/* NMT 命令（cs=0x01 start / 0x80 enter pre-op / 0x81 reset node…）。node_id=0 广播 */
int co_master_nmt(struct co_master *m, uint8_t node_id, uint8_t command);

/* 按站号查节点；找不到返回 NULL（内核内部用） */
struct co_node *co_master_find_node(struct co_master *m, uint8_t node_id);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CANOPEN_MASTER_H */
