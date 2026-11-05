/*
 * cia402_monitor.c — 后台监控读数（状态字 / 实际位置 / 实际速度 / 驱动器错误码）
 *
 * 每 50 ms 轮转读一个对象：6041 → 6064 → 606C → 603F，单节点 200 ms 一圈。
 * 与状态机共用同一条 SDO 通道（一个节点只有一个客户端对象），规则：
 *   1) 只在 n->sdo == IDLE 时发起；有别人的事务在途就跳过这一圈；
 *   2) 事务打归属标记（sdo_owner = MONITOR），流程调 cia402_sdo_result()
 *      看不见它，不会误读监控的数据；
 *   3) 超时用短的（20 ms），失败只记次数并保留上一次有效值，不报错、不改状态。
 */

#include "cia402_internal.h"

#include <stddef.h>

/* 轮转间隔：一条读的事务通常 1–3 ms 完成，50 ms 给状态机留足空档 */
#define MONITOR_INTERVAL_MS 50u

/* 监控读的等待上限：比流程用的 200 ms 短得多，避免拖住状态机 */
#define MONITOR_TIMEOUT_MS 20u

#define MONITOR_SLOT_COUNT 4u

/* 轮转表：下标 = monitor.slot；顺序即刷新优先级 */
static const uint16_t kSlotIndex[MONITOR_SLOT_COUNT] = {
	CIA402_IDX_STATUSWORD,      /* 0：状态字 */
	CIA402_IDX_POSITION_ACTUAL, /* 1：实际位置 */
	CIA402_IDX_ACTUAL_VELOCITY, /* 2：实际速度 */
	CIA402_IDX_ERROR_CODE,      /* 3：驱动器错误码 */
};

/* 把 n->data（小端）拼成 32 位；不足 4 字节的高位补 0 */
static uint32_t le32(const struct cia402_node *n)
{
	uint32_t v = 0;
	uint8_t i;

	for (i = 0; i < n->data_len && i < 4U; i++) {
		v |= (uint32_t)n->data[i] << (8u * i);
	}
	return v;
}

/* 一条读成功：按当前 slot 落到对应字段，并置 valid 位 */
static void store(struct cia402_node *n, uint8_t slot)
{
	const uint32_t v = le32(n);

	switch (slot) {
	case 0:
		n->monitor.statusword = (uint16_t)(v & 0xFFFFu);
		n->monitor.valid |= CIA402_MON_VALID_STATUSWORD;
		break;
	case 1:
		n->monitor.position = (int32_t)v;
		n->monitor.valid |= CIA402_MON_VALID_POSITION;
		break;
	case 2:
		n->monitor.velocity = (int32_t)v;
		n->monitor.valid |= CIA402_MON_VALID_VELOCITY;
		break;
	default:
		n->monitor.error_code = (uint16_t)(v & 0xFFFFu);
		n->monitor.valid |= CIA402_MON_VALID_ERROR_CODE;
		break;
	}
}

void cia402_monitor_poll(struct cia402_node *n, uint32_t now_ms)
{
	if (n == NULL) {
		return;
	}

	/* 1) 有在途的监控读：等它结束，收结果（成功才覆盖，失败保留旧值） */
	if (n->monitor.await) {
		if (n->sdo == CIA402_SDO_READING || n->sdo == CIA402_SDO_WRITING) {
			return; /* 还在途 */
		}
		if (n->sdo == CIA402_SDO_DONE && n->sdo_owner == CIA402_SDO_OWNER_MONITOR) {
			store(n, n->monitor.slot);
		} else {
			n->monitor.fail_count++; /* 超时 / abort：这圈没读到 */
		}
		n->sdo = CIA402_SDO_IDLE;
		n->sdo_owner = CIA402_SDO_OWNER_PROCESS; /* 通道还给流程 */
		n->monitor.await = 0;
		n->monitor.slot = (uint8_t)((n->monitor.slot + 1u) % MONITOR_SLOT_COUNT);
		n->monitor.last_ms = now_ms;
		return;
	}

	/* 2) 通道被别人占着：这一圈直接放弃，不排队、不覆盖 */
	if (n->sdo != CIA402_SDO_IDLE) {
		return;
	}

	/* 3) 节流：不到 50 ms 不发 */
	if ((uint32_t)(now_ms - n->monitor.last_ms) < MONITOR_INTERVAL_MS) {
		return;
	}

	if (cia402_sdo_read_to(n, kSlotIndex[n->monitor.slot], 0, MONITOR_TIMEOUT_MS) != 0) {
		return; /* 发不出去：下一拍再试 */
	}
	n->sdo_owner = CIA402_SDO_OWNER_MONITOR;
	n->monitor.await = 1;
	n->monitor.last_ms = now_ms;
}

/* ---- 上层读取（只读最新有效值，不触发任何传输） ---- */

int32_t cia402_monitor_position(const struct cia402_node *n)
{
	return (n == NULL) ? 0 : n->monitor.position;
}

int32_t cia402_monitor_velocity(const struct cia402_node *n)
{
	return (n == NULL) ? 0 : n->monitor.velocity;
}

uint16_t cia402_monitor_statusword(const struct cia402_node *n)
{
	return (n == NULL) ? 0 : n->monitor.statusword;
}

uint16_t cia402_monitor_error_code(const struct cia402_node *n)
{
	return (n == NULL) ? 0 : n->monitor.error_code;
}

uint8_t cia402_monitor_valid(const struct cia402_node *n)
{
	return (n == NULL) ? 0 : n->monitor.valid;
}
