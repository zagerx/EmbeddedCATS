/*
 * master.c — CANopen 主站：CANopenNode 的薄封装
 *
 * 这一层只做三件事：
 *   1. 按上游要求初始化协议栈（CO_new / CO_CANinit / CO_CANopenInit）；
 *   2. 每拍把总线帧分派给协议对象并推进协议栈；
 *   3. 回答应用层的查询（某个节点的心跳是否正常）。
 *
 * 上游在 CO_USE_GLOBALS 下只有一份静态对象，所以本层也是单实例。
 */

#include <canopen/master.h>
#include <canopen/sdo_client.h>

#include "CANopen.h"
#include "OD.h"
#include "301/CO_HBconsumer.h"
#include "301/CO_NMT_Heartbeat.h"
#include "node/co_driver_cats.h"

#include <stddef.h>

/*
 * 主站自己的站号。必须是一个真实站号（1..127）：
 * 上游 CO_process() 在 nodeIdUnconfigured 时直接返回，协议栈什么都不处理。
 * 0x7F 是常见的主站/配置工具站号，避开驱动器的 1 / 2。
 */
#define CO_MASTER_NODE_ID 0x7Fu

/*
 * 首个心跳的延迟。主站要不要发自己的心跳由 OD 0x1017 决定（照抄来的初值是 0
 * ⇒ 不发 0x77F），上游把这个值仅当作“第一个心跳前等多久”，必须给个数。
 */
#define CO_MASTER_FIRST_HB_MS 500u
#define CO_MASTER_NMT_CONTROL CO_NMT_STARTUP_TO_OPERATIONAL
#define CO_MASTER_SDO_SRV_TIMEOUT_MS 1000u
#define CO_MASTER_SDO_CLI_TIMEOUT_MS 750u

static CO_t *s_co;         /* 上游协议栈对象（静态分配） */
static uint32_t s_last_ms; /* 上一拍的时刻，用于算 timeDifference_us */
static uint8_t s_started;  /* 是否已经跑过第一拍 */

/* ---- 对外接口 ---- */

void co_master_init(struct co_master *m, struct can_io *bus)
{
	uint32_t heap_used = 0;
	uint32_t err_info = 0;
	CO_ReturnError_t err;

	if (m == NULL) {
		return;
	}

	m->bus = bus;
	m->node_count = 0;
	s_last_ms = 0;
	s_started = 0;

	s_co = CO_new(NULL, &heap_used); /* CO_USE_GLOBALS：返回静态对象，不分配 */
	if (s_co == NULL) {
		return;
	}

	err = CO_CANinit(s_co, bus, 0);
	if (err != CO_ERROR_NO) {
		s_co = NULL; /* 初始化失败：后续查询一律返回否定，应用层会走 BOOTUP_TIMEOUT */
		return;
	}

	/* od 必须传真实的 OD：CO_SDOserver_init() 会校验它非空。
	 * 未定义 CO_MULTIPLE_OD 时，OD.c 里的全局 OD 就是默认对象字典。 */
	err = CO_CANopenInit(s_co, NULL, NULL, OD, NULL, CO_MASTER_NMT_CONTROL,
			     CO_MASTER_FIRST_HB_MS, CO_MASTER_SDO_SRV_TIMEOUT_MS,
			     CO_MASTER_SDO_CLI_TIMEOUT_MS, false, CO_MASTER_NODE_ID, &err_info);
	if (err != CO_ERROR_NO) {
		s_co = NULL;
		return;
	}

	/* 主站不用本地 PDO：上游 CO_process() 不碰 PDO，RPDO/TPDO 留 NULL 即可。
	 * 将来需要主站侧 PDO 时，再按 CO_MASTER_NODE_ID 调 CO_CANopenInitPDO()。 */
	CO_CANsetNormalMode(s_co->CANmodule);
}

int co_master_add_node(struct co_master *m, uint8_t node_id)
{
	const CO_HBconsNode_t *slot;
	uint8_t idx;

	if (m == NULL || node_id == 0U || m->node_count >= CO_MASTER_MAX_NODES) {
		return 0;
	}
	for (idx = 0; idx < m->node_count; idx++) {
		if (m->node_id[idx] == node_id) {
			return 0; /* 已登记 */
		}
	}
	if (s_co == NULL || s_co->HBcons == NULL) {
		return 0; /* 协议栈没起来：无法监控心跳 */
	}

	/* 监控下标 idx 对应 OD 里 0x1016 的第 idx+1 个子索引。槽位里的站号是 OD
	 * 配的，必须与本次登记的站号一致，否则查询会落到别的节点上。 */
	idx = m->node_count;
	slot = &s_co->HBcons->monitoredNodes[idx];
	if (slot->nodeId != node_id) {
		return 0;
	}

	m->node_id[idx] = node_id;
	m->node_count++;
	return 1;
}

int co_master_nmt(struct co_master *m, uint8_t node_id, uint8_t command)
{
	if (m == NULL || s_co == NULL || s_co->NMT == NULL) {
		return 0;
	}
	/* DS301 的命令字与 CO_NMT_command_t 的数值定义一致 */
	return CO_NMT_sendCommand(s_co->NMT, (CO_NMT_command_t)command, node_id) == CO_ERROR_NO;
}

void co_master_poll(struct co_master *m, uint32_t now_ms)
{
	uint32_t diff_us = 0;

	if (m == NULL || s_co == NULL) {
		return;
	}

	if (s_started) {
		diff_us = (uint32_t)(now_ms - s_last_ms) * 1000u; /* ms → us，无符号相减处理回绕 */
	}
	s_last_ms = now_ms;
	s_started = 1;

	co_driver_cats_receive(s_co->CANmodule);      /* 抽帧 → 按 ident/mask 分派 */
	(void)CO_process(s_co, false, diff_us, NULL); /* 推进 NMT/心跳/SDO/EMCY… */
}

int co_master_node_alive(const struct co_master *m, uint8_t node_id)
{
	uint8_t idx;

	if (m == NULL || s_co == NULL || s_co->HBcons == NULL) {
		return 1;
	}
	/* 站号 → 监控下标（两者同序，见 co_master_add_node） */
	for (idx = 0; idx < m->node_count; idx++) {
		if (m->node_id[idx] == node_id) {
			break;
		}
	}
	if (idx >= m->node_count) {
		return 1; /* 没登记过 = 不可能监控 → 不正常 */
	}
	return CO_HBconsumer_getState(s_co->HBcons, idx) == CO_HBconsumer_ACTIVE ? 0 : 1;
}

CO_SDOclient_t *co_master_sdo_client(struct co_master *m, uint8_t node_id)
{
	uint8_t i;

	if (m == NULL || s_co == NULL) {
		return NULL;
	}
	/* “哪个客户端服务哪个站号”只在 OD 里声明（1280h/1281h 的子索引 3），
	 * 上游 init 时已把站号和 COB-ID 绑进对象，这里只查表，不猜下标。 */
	for (i = 0; i < OD_CNT_SDO_CLI; i++) {
		if (s_co->SDOclient[i].valid &&
		    s_co->SDOclient[i].nodeIDOfTheSDOServer == node_id) {
			return &s_co->SDOclient[i];
		}
	}
	return NULL;
}
