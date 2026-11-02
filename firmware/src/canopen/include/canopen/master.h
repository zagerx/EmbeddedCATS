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
 * 接收模型（单一接收点）：**只有 co_master_poll() 从总线取帧**，取到后按 COB-ID
 * 路由到对应节点；其他模块（如未来的 SDO 客户端）不许自己 recv，否则两边会互相偷帧。
 * poll 每拍无条件调用，与运行状态无关 —— 这就是“心跳一直检测”的结构性保证。
 */

#ifndef CANOPEN_MASTER_H
#define CANOPEN_MASTER_H

#include <canopen/can_io.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CO_MASTER_MAX_NODES 16
#define CO_MASTER_RX_MAX_PER_POLL 32u /* 单拍最多处理多少帧：总线被灌爆时不卡住调用方 */

/*
 * DS 301 7.3.2：NMT 状态值与心跳/boot-up 帧的 data[0] 一致，可直接赋值。
 * CO_NMT_OFFLINE 是主站自定义：登记了但从没收到过该节点的心跳/boot-up，
 * 用来区分"节点没上电"和"节点在 Pre-op"。它只在主站内部用，不上总线。
 */
enum co_nmt_state {
	CO_NMT_BOOTUP = 0x00,
	CO_NMT_STOPPED = 0x04,
	CO_NMT_OPERATIONAL = 0x05,
	CO_NMT_PREOPERATIONAL = 0x7F,
	CO_NMT_OFFLINE = 0xFF
};

struct co_node {
	uint8_t node_id;
	enum co_nmt_state nmt;
	uint32_t last_heartbeat_ms; /* 最近一次收到 0x700+id 的时刻 */
	uint8_t boot_seen; /* 收到过 boot-up：区分“没上电”与“上线后掉线” */
	uint8_t rebooted;  /* 上线后又 boot 一次 ⇒ 节点被重启过（掉电/复位） */
};

struct co_master {
	struct can_io *bus; /* 只借用；通道由壳创建与关闭 */

	struct co_node nodes[CO_MASTER_MAX_NODES];
	uint8_t node_count;
};

/* 只做初始化；不发任何帧，也不改节点状态 */
void co_master_init(struct co_master *m, struct can_io *bus);

/* 登记一个节点。重复登记返回 0；达到上限返回 0 */
int co_master_add_node(struct co_master *m, uint8_t node_id);

/* NMT 命令（cs=0x01 start / 0x80 enter pre-op / 0x81 reset node…）。node_id=0 广播 */
int co_master_nmt(struct co_master *m, uint8_t node_id, uint8_t command);

/* 按站号查节点；找不到返回 NULL（内核内部用） */
struct co_node *co_master_find_node(struct co_master *m, uint8_t node_id);

/*
 * 收帧：抽干总线 → 按 COB-ID 路由 → 更新节点事实（时间戳 / NMT 状态 / boot-up）。
 * **每拍无条件调用，与运行状态无关**；不做任何“该不该报错”的判断（那是应用层的事）。
 * 返回本拍处理了多少帧。
 */
uint32_t co_master_poll(struct co_master *m, uint32_t now_ms);

/* 清空所有节点的上线/心跳记录。启动流程进入时调用，防止旧数据让启动一步跳过 */
void co_master_reset_seen(struct co_master *m);

/* ---- 事实查询：内核只回答“是不是这样”，阈值由调用方给 ---- */

/* 所有登记节点都收到过 boot-up（没登记节点时返回 1） */
int co_master_all_boot_seen(const struct co_master *m);

/* 所有节点都在 timeout_ms 内发过心跳 */
int co_master_all_alive(const struct co_master *m, uint32_t now_ms, uint32_t timeout_ms);

/* 有没有节点在上线之后又重启过 */
int co_master_any_rebooted(const struct co_master *m);
void co_master_clear_rebooted(struct co_master *m);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CANOPEN_MASTER_H */
