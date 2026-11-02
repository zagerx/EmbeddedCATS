/*
 * master.c — 主站容器、NMT 发送、收帧路由与心跳事实
 *
 * 分工刻意划清：
 *   本文件  只维护事实（谁什么时候发过什么），不判断“该不该报错”；
 *   应用层  拿这些事实做策略（超时进 FAULT、重启进 FAULT）。
 * 这样换设备、换心跳周期都不必改这里。
 */

#include <canopen/master.h>

#include <stddef.h>

/* COB-ID 分段（DS 301 第 7 章） */
#define CO_COB_NMT_ERR_CTRL_MIN 0x701u /* 心跳 / boot-up：0x700 + 站号 */
#define CO_COB_NMT_ERR_CTRL_MAX 0x77Fu
#define CO_COB_SDO_RESP_MIN 0x581u /* SDO 回应：0x580 + 站号 */
#define CO_COB_SDO_RESP_MAX 0x5FFu

void co_master_init(struct co_master *m, struct can_io *bus)
{
	m->bus = bus;
	m->node_count = 0;
}

struct co_node *co_master_find_node(struct co_master *m, uint8_t node_id)
{
	for (uint8_t i = 0; i < m->node_count; i++) {
		if (m->nodes[i].node_id == node_id) {
			return &m->nodes[i];
		}
	}
	return NULL;
}

int co_master_add_node(struct co_master *m, uint8_t node_id)
{
	struct co_node *n;

	if (m->node_count >= CO_MASTER_MAX_NODES) {
		return 0;
	}
	if (co_master_find_node(m, node_id) != NULL) {
		return 0;
	}

	n = &m->nodes[m->node_count++];
	n->node_id = node_id;
	n->nmt = CO_NMT_OFFLINE; /* 还没收到任何心跳，先当离线 */
	n->last_heartbeat_ms = 0;
	n->boot_seen = 0;
	n->rebooted = 0;
	return 1;
}

int co_master_nmt(struct co_master *m, uint8_t node_id, uint8_t command)
{
	const struct can_io_frame frame = {
		.id = 0x000, /* NMT 永远是 0x000，全网一条 */
		.dlc = 2,
		.data = {command, node_id}, /* node_id=0 表示对所有节点生效 */
	};

	return can_io_send(m->bus, &frame) == CAN_IO_OK;
}

/* ---- 收帧路由 ---- */

/*
 * 心跳 / boot-up 帧：data[0] 是 NMT 状态；data[0] == 0 是 boot-up。
 * boot-up 是“第一次心跳”，所以它也刷新时间戳。
 * 已经上线过的节点又发 boot-up ⇒ 它被重启了，记下这个事实。
 */
static void node_on_heartbeat(struct co_node *n, const struct can_io_frame *f, uint32_t now_ms)
{
	n->last_heartbeat_ms = now_ms;

	if (f->data[0] == (uint8_t)CO_NMT_BOOTUP) {
		if (n->boot_seen) {
			n->rebooted = 1;
		}
		n->boot_seen = 1;
		n->nmt = CO_NMT_BOOTUP;
		return;
	}
	n->nmt = (enum co_nmt_state)f->data[0];
}

static void master_route(struct co_master *m, const struct can_io_frame *f, uint32_t now_ms)
{
	struct co_node *n;

	if (f->dlc < 1u) {
		return;
	}

	if (f->id >= CO_COB_NMT_ERR_CTRL_MIN && f->id <= CO_COB_NMT_ERR_CTRL_MAX) {
		n = co_master_find_node(m, (uint8_t)(f->id - 0x700u));
		if (n != NULL) {
			node_on_heartbeat(n, f, now_ms);
		}
		return;
	}

	if (f->id >= CO_COB_SDO_RESP_MIN && f->id <= CO_COB_SDO_RESP_MAX) {
		/* SDO 回应：现在没有消费者（co_sdo_read 还是自己 recv 的阻塞版）。
		 * 接非阻塞 SDO 时在这里送进等待槽，届时 co_sdo_read 不再碰总线。 */
		return;
	}

	/* 其余（NMT 回显、TPDO、EMCY…）当前无人消费，丢弃 */
}

uint32_t co_master_poll(struct co_master *m, uint32_t now_ms)
{
	struct can_io_frame f;
	uint32_t processed = 0;

	while (processed < CO_MASTER_RX_MAX_PER_POLL) {
		if (can_io_recv(m->bus, &f, 0) != CAN_IO_OK) {
			break; /* 抽干了（或总线出错），本拍到此为止 */
		}
		master_route(m, &f, now_ms);
		processed++;
	}
	return processed;
}

void co_master_reset_seen(struct co_master *m)
{
	for (uint8_t i = 0; i < m->node_count; i++) {
		m->nodes[i].nmt = CO_NMT_OFFLINE;
		m->nodes[i].last_heartbeat_ms = 0;
		m->nodes[i].boot_seen = 0;
		m->nodes[i].rebooted = 0;
	}
}

/* ---- 事实查询 ---- */

int co_master_all_boot_seen(const struct co_master *m)
{
	for (uint8_t i = 0; i < m->node_count; i++) {
		if (!m->nodes[i].boot_seen) {
			return 0;
		}
	}
	return 1; /* 空节点表算“都到了”：本工程的壳一定会先登记轴节点 */
}

int co_master_all_alive(const struct co_master *m, uint32_t now_ms, uint32_t timeout_ms)
{
	for (uint8_t i = 0; i < m->node_count; i++) {
		const struct co_node *n = &m->nodes[i];

		if (!n->boot_seen) {
			return 0;
		}
		if ((uint32_t)(now_ms - n->last_heartbeat_ms) > timeout_ms) {
			return 0;
		}
	}
	return 1;
}

int co_master_any_rebooted(const struct co_master *m)
{
	for (uint8_t i = 0; i < m->node_count; i++) {
		if (m->nodes[i].rebooted) {
			return 1;
		}
	}
	return 0;
}

void co_master_clear_rebooted(struct co_master *m)
{
	for (uint8_t i = 0; i < m->node_count; i++) {
		m->nodes[i].rebooted = 0;
	}
}
