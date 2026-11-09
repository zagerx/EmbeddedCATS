/*
 * master.c — CANopen 主站：装配、收帧分派、每拍推进、发帧出口
 *
 * 这一层只做四件事：
 *   1. 登记节点（站号 → 下标、事务槽、心跳状态）；
 *   2. 每拍把总线抽干，按 COB-ID 分派给心跳或对应节点的 SDO 事务；
 *   3. 推进各节点的在途事务（超时判定）；
 *   4. 对外回答"某个节点还活着吗"。
 *
 * 它不认识对象号，也不认识"轴"；整个进程只有它一个地方碰 can_io。
 */

#include "internal.h"

#include <stddef.h>
#include <string.h>

/* 每拍最多抽多少帧：防止总线上灌满无关帧时把状态机饿死 */
#define CO_MASTER_RX_BUDGET 64u

/* COB-ID 段（DS 301 预定义连接集）：只列本层真正会收发的那三段 */
#define COB_ID_NMT 0x000u
#define COB_ID_SDO_TX 0x580u /* 从站 → 主站（本主站的接收方向） */
#define COB_ID_HB 0x700u

/* NMT 命令字（DS 301 原始码，只出现在本文件） */
#define NMT_CMD_START 0x01u
#define NMT_CMD_RESET_COMM 0x82u

int co_master_send(struct co_master *m, uint16_t cob_id, const uint8_t *data, uint8_t len)
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

void co_master_init(struct co_master *m, struct can_io *bus)
{
	uint8_t i;

	if (m == NULL) {
		return;
	}
	m->bus = bus;
	m->now_ms = 0u;
	m->node_count = 0u;
	for (i = 0u; i < CO_MASTER_MAX_NODES; i++) {
		m->node_id[i] = 0u;
		co_node_init(&m->node[i], NULL, 0u);
		memset(&m->nstate[i], 0, sizeof(m->nstate[i]));
	}
}

int co_master_index(const struct co_master *m, uint8_t node_id)
{
	uint8_t i;

	if (m == NULL || node_id == 0u) {
		return -1;
	}
	for (i = 0u; i < m->node_count; i++) {
		if (m->node_id[i] == node_id) {
			return (int)i;
		}
	}
	return -1;
}

int co_master_add_node(struct co_master *m, uint8_t node_id)
{
	uint8_t idx;

	if (m == NULL || node_id == 0u || node_id > 0x7Fu) {
		return 0;
	}
	if (m->node_count >= CO_MASTER_MAX_NODES) {
		return 0;
	}
	if (co_master_index(m, node_id) >= 0) {
		return 0; /* 重复登记 */
	}
	idx = m->node_count;
	m->node_id[idx] = node_id;
	co_node_init(&m->node[idx], m, node_id);
	memset(&m->nstate[idx], 0, sizeof(m->nstate[idx]));
	m->node_count++;
	return 1;
}

struct co_node *co_master_node(struct co_master *m, uint8_t node_id)
{
	int idx = co_master_index(m, node_id);

	return (idx < 0) ? NULL : &m->node[idx];
}

int co_master_set_state(struct co_master *m, uint8_t node_id, enum co_master_state state)
{
	uint8_t data[2];

	if (m == NULL || node_id > 0x7Fu) {
		return -1;
	}
	switch (state) {
	case CO_MASTER_START:
		data[0] = NMT_CMD_START;
		break;
	case CO_MASTER_RESET_COMMUNICATION:
		data[0] = NMT_CMD_RESET_COMM;
		break;
	default:
		return -1;
	}
	data[1] = node_id;
	return co_master_send(m, COB_ID_NMT, data, 2u);
}

/* 收到一帧：只认三段 COB-ID，其余（PDO/EMCY…）不认识就不管 */
static void dispatch(struct co_master *m, const struct can_io_frame *frame, uint32_t now_ms)
{
	uint16_t cob = (uint16_t)(frame->id & 0x7FFu);

	if (frame->dlc == 0u) {
		return;
	}
	if (cob >= COB_ID_HB && cob < (COB_ID_HB + 0x80u)) {
		co_hb_on_frame(m, (uint8_t)(cob - COB_ID_HB), now_ms);
		return;
	}
	if (cob >= COB_ID_SDO_TX && cob < (COB_ID_SDO_TX + 0x80u)) {
		struct co_node *n = co_master_node(m, (uint8_t)(cob - COB_ID_SDO_TX));

		if (n != NULL) {
			co_node_on_frame(n, frame->data, now_ms);
		}
	}
}

void co_master_poll(struct co_master *m, uint32_t now_ms)
{
	struct can_io_frame frame;
	uint8_t i;
	uint8_t budget = (uint8_t)CO_MASTER_RX_BUDGET;

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

	/* 2. 每个节点的事务槽推进一步：要发请求帧 / abort 帧就在这里发 */
	for (i = 0u; i < m->node_count; i++) {
		co_node_advance(&m->node[i], now_ms);
	}
}

int co_master_node_alive(const struct co_master *m, uint8_t node_id)
{
	return co_hb_alive(m, node_id);
}
