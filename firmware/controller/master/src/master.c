/*
 * master.c — 主站实体：登记从站、收帧分派、每拍推进、发帧出口
 *
 * 四件事：
 *   1. 登记从站（站号 → 下标、SDO 客户端、心跳状态）；
 *   2. 每拍把总线抽干，按 COB-ID 交给心跳或对应从站的 SDO 客户端；
 *   3. 推进各从站的在途事务（要发帧就发）；
 *   4. 对外回答"某个从站还活着吗"。
 *
 * 帧长什么样、命令字是几，全在 canopen/ 里；本文件只做"谁的帧发给谁"。
 * 它不认识对象号，也不认识"轴"；整个进程只有它一个地方碰 can_io。
 */

#include <master/master.h>

#include <canopen/cob.h>

#include <stddef.h>
#include <string.h>

/* 每拍最多抽多少帧：防止总线上灌满无关帧时把状态机饿死 */
#define MASTER_RX_BUDGET 64u

static int master_send(struct master *m, uint16_t cob_id, const uint8_t *data, uint8_t len)
{
	struct can_io_frame frame;

	if (m == NULL || m->bus == NULL || data == NULL || len > CAN_IO_MAX_DLC) {
		return -1;
	}
	frame.id = cob_id;
	frame.dlc = len;
	memcpy(frame.data, data, len);
	return (can_io_send(m->bus, &frame) == CAN_IO_OK) ? 0 : -1;
}

/* 注入给 SDO 客户端的发帧能力：它给出 COB-ID 和 8 字节，这里只负责出去 */
static int master_tx(void *ctx, uint16_t cob_id, const uint8_t data[8])
{
	return master_send(ctx, cob_id, data, 8u);
}

/* 站号 → 下标；未登记返回 -1 */
static int master_index(const struct master *m, uint8_t node_id)
{
	uint8_t i;

	if (m == NULL || node_id == 0u) {
		return -1;
	}
	for (i = 0u; i < m->node_count; i++) {
		if (m->node[i].node_id == node_id) {
			return (int)i;
		}
	}
	return -1;
}

void master_init(struct master *m, struct can_io *bus)
{
	if (m == NULL) {
		return;
	}
	memset(m, 0, sizeof(*m));
	m->bus = bus;
}

int master_add_node(struct master *m, uint8_t node_id)
{
	struct master_node *n;
	uint8_t idx;

	if (m == NULL || node_id == 0u || node_id > CO_MAX_NODE_ID) {
		return 0;
	}
	if (m->node_count >= MASTER_MAX_NODES) {
		return 0;
	}
	if (master_index(m, node_id) >= 0) {
		return 0; /* 重复登记 */
	}
	idx = m->node_count;
	n = &m->node[idx];
	n->node_id = node_id;
	co_sdo_init(&n->sdo, node_id, master_tx, m);
	co_hb_reset(&n->hb);
	m->node_count++;
	return 1;
}

uint8_t master_node_count(const struct master *m)
{
	return (m == NULL) ? 0u : m->node_count;
}

uint8_t master_node_id(const struct master *m, uint8_t idx)
{
	if (m == NULL || idx >= m->node_count) {
		return 0u;
	}
	return m->node[idx].node_id;
}

struct co_sdo *master_sdo(struct master *m, uint8_t node_id)
{
	int idx = master_index(m, node_id);

	return (idx < 0) ? NULL : &m->node[idx].sdo;
}

struct can_io *master_bus(struct master *m)
{
	return (m == NULL) ? NULL : m->bus;
}

int master_set_state(struct master *m, uint8_t node_id, enum co_nmt_cmd cmd)
{
	uint8_t data[2];
	uint8_t len = co_nmt_frame(data, cmd, node_id);

	if (len == 0u) {
		return -1;
	}
	return master_send(m, CO_COB_NMT, data, len);
}

/* 收到一帧：只认心跳与 SDO 应答两段，其余（PDO/EMCY…）不认识就不管 */
static void dispatch(struct master *m, const struct can_io_frame *frame, uint32_t now_ms)
{
	uint16_t cob = (uint16_t)(frame->id & 0x7FFu);
	uint8_t node_id = 0u;
	int idx;

	if (frame->dlc == 0u) {
		return;
	}
	if (co_cob_is_hb(cob, &node_id) != 0) {
		idx = master_index(m, node_id);
		if (idx >= 0) {
			co_hb_on_frame(&m->node[idx].hb, frame->data, now_ms);
		}
		return;
	}
	if (co_cob_is_sdo_rsp(cob, &node_id) != 0) {
		idx = master_index(m, node_id);
		if (idx >= 0) {
			co_sdo_on_frame(&m->node[idx].sdo, frame->data, now_ms);
		}
	}
}

void master_poll(struct master *m, uint32_t now_ms)
{
	struct can_io_frame frame;
	uint8_t i;
	uint8_t budget = (uint8_t)MASTER_RX_BUDGET;

	if (m == NULL || m->bus == NULL) {
		return;
	}
	m->now_ms = now_ms;

	/* 1. 把总线上已到的帧抽干（不阻塞）；抽帧与分派在同一个地方做 */
	while (budget > 0u) {
		if (can_io_recv(m->bus, &frame, 0u) != CAN_IO_OK) {
			break; /* 本拍的帧已经抽完 */
		}
		budget--;
		dispatch(m, &frame, now_ms);
	}

	/* 2. 每个从站的事务槽推进一步：要发请求帧 / abort 帧就在这里发 */
	for (i = 0u; i < m->node_count; i++) {
		co_sdo_advance(&m->node[i].sdo, now_ms);
	}
}

int master_node_alive(const struct master *m, uint8_t node_id)
{
	int idx = master_index(m, node_id);

	if (idx < 0) {
		return 1; /* 没登记 = 监控不了 → 判不正常 */
	}
	return co_hb_lost(&m->node[idx].hb, m->now_ms, MASTER_HB_TIMEOUT_MS);
}
