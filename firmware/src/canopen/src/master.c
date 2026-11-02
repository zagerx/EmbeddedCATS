/*
 * master.c — 主站容器与 NMT 发送（其余能力随功能补）
 *
 * 本文件只放“最小可用”的那几件事：初始化、登记节点、发 NMT 命令。
 * 心跳监控、PDO 分发、pool 消费在后续拍里加进来。
 */

#include <canopen/master.h>

#include <stddef.h>

void co_master_init(struct co_master *m, struct can_io *bus)
{
	m->bus = bus;
	m->node_count = 0;
	m->pool_count = 0;
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
