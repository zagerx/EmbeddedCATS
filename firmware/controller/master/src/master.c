/*
 * master.c — 主站实体：站号表 + 每拍两段（收 / 发）
 *
 * 协议本身全在 CANopenNode 里（NMT、心跳消费、SDO 客户端）；
 * 本文件只负责：
 *   1. 把总线接到 CO_driver 的移植层（co_driver_cats.c）；
 *   2. 登记从站（第 i 个 ⇒ 第 i 条 SDO 客户端通道，对应 OD 0x1280/0x1281）；
 *   3. 每拍：抽帧交给栈 → CO_process 推进（含超时与发帧）。
 */

#include <master/master.h>

#include "OD.h" /* 本机的对象字典（心跳表 0x1016、SDO 客户端 0x1280/0x1281） */

#include <stddef.h>
#include <string.h>

/* 每拍最多抽多少帧：防止总线上灌满无关帧时把状态机饿死 */
#define MASTER_RX_BUDGET 64u

/* CO_CANopenInit 的参数：本机选择 */
#define MASTER_NMT_CONTROL 0u /* 不自动进 operational，由业务状态机下令 */
#define MASTER_FIRST_HB_MS 500u /* 心跳生产者的首帧延时（本机不生产心跳，占位） */
#define MASTER_SDO_SRV_TIMEOUT_MS 1000u
#define MASTER_SDO_CLI_TIMEOUT_MS 500u

/* 站号 → 下标；未登记返回 -1 */
static int master_index(const struct master *m, uint8_t node_id)
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

void master_init(struct master *m, struct can_io *bus)
{
	if (m == NULL) {
		return;
	}
	memset(m, 0, sizeof(*m));
	m->bus = bus;
	co_cats_can_init(&m->can, bus);

	/* CO_USE_GLOBALS：返回的是静态实例，不动态分配内存 */
	m->co = CO_new(NULL, NULL);
	if (m->co == NULL) {
		return;
	}
	(void)CO_CANinit(m->co, &m->can, 0u); /* 波特率由平台侧配置 */
	(void)CO_CANopenInit(m->co, NULL, NULL, OD, NULL, MASTER_NMT_CONTROL, MASTER_FIRST_HB_MS,
			     MASTER_SDO_SRV_TIMEOUT_MS, MASTER_SDO_CLI_TIMEOUT_MS, false,
			     MASTER_NODE_ID, NULL);
}

int master_add_node(struct master *m, uint8_t node_id)
{
	if (m == NULL || m->co == NULL || node_id == 0u || node_id > 0x7Fu) {
		return 0;
	}
	if (m->node_count >= MASTER_MAX_NODES || m->node_count >= (uint8_t)OD_CNT_SDO_CLI) {
		return 0; /* 没有多余的 SDO 通道 */
	}
	if (master_index(m, node_id) >= 0) {
		return 0; /* 重复登记 */
	}
	m->node_id[m->node_count] = node_id;
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
	return m->node_id[idx];
}

CO_SDOclient_t *master_sdo(struct master *m, uint8_t node_id)
{
	int idx = master_index(m, node_id);

	if (idx < 0 || m->co == NULL) {
		return NULL;
	}
	return &m->co->SDOclient[idx];
}

struct can_io *master_bus(struct master *m)
{
	return (m == NULL) ? NULL : m->bus;
}

int master_set_state(struct master *m, uint8_t node_id, enum master_nmt_cmd cmd)
{
	if (m == NULL || m->co == NULL || m->co->NMT == NULL) {
		return -1;
	}
	if (CO_NMT_sendCommand(m->co->NMT, (CO_NMT_command_t)cmd, node_id) != CO_ERROR_NO) {
		return -1;
	}
	return 0;
}

void master_recv_step(struct master *m, uint32_t now_ms)
{
	struct can_io_frame frame;
	uint8_t budget = (uint8_t)MASTER_RX_BUDGET;

	if (m == NULL || m->bus == NULL || m->co == NULL) {
		return;
	}
	m->now_ms = now_ms;

	/* 把总线上已到的帧抽干（不阻塞），逐帧交给栈（软件匹配 ident/mask） */
	while (budget > 0u) {
		if (can_io_recv(m->bus, &frame, 0u) != CAN_IO_OK) {
			break; /* 本拍的帧已经抽完 */
		}
		budget--;
		co_cats_can_receive(m->co->CANmodule, &frame);
	}
}

void master_send_step(struct master *m, uint32_t now_ms)
{
	uint32_t now_us;
	uint32_t dt_us;

	if (m == NULL || m->co == NULL) {
		return;
	}
	m->now_ms = now_ms;
	now_us = now_ms * 1000u;
	dt_us = (m->last_us == 0u) ? 0u : (uint32_t)(now_us - m->last_us);
	m->last_us = now_us;

	/* 栈的统一入口：心跳消费超时、SDO 超时、NMT 处理、发帧都在里面 */
	(void)CO_process(m->co, false, dt_us, NULL);
}

int master_node_alive(const struct master *m, uint8_t node_id)
{
	int8_t hb_idx;
	CO_HBconsumer_state_t state;

	if (m == NULL || m->co == NULL || m->co->HBcons == NULL) {
		return 1;
	}
	hb_idx = CO_HBconsumer_getIdxByNodeId(m->co->HBcons, node_id);
	if (hb_idx < 0) {
		return 1; /* 没在 0x1016 里配置：监控不了 → 判不正常 */
	}
	state = CO_HBconsumer_getState(m->co->HBcons, (uint8_t)hb_idx);
	return (state == CO_HBconsumer_ACTIVE) ? 0 : 1;
}
