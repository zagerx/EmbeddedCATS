/*
 * heartbeat.c — 心跳与 boot-up（从站 → 主站，COB-ID 0x700+id）
 *
 * 只记两件事：最近一次收到的时刻、带过来的状态字节。
 * 超时判定不在这里滚动计时器，而是"需要用的时候和当前时刻比一下"，
 * 因此本文件不依赖任何定时器，也不会因为调用次数变化而漂移。
 */

#include "internal.h"

void co_hb_on_frame(struct co_master *m, uint8_t node_id, uint32_t now_ms)
{
	int idx = co_master_index(m, node_id);

	if (idx < 0) {
		return; /* 没登记的站号：不关心 */
	}
	m->nstate[idx].hb_last_ms = now_ms;
	m->nstate[idx].hb_seen = 1u;
}

int co_hb_alive(const struct co_master *m, uint8_t node_id)
{
	int idx = co_master_index(m, node_id);

	if (idx < 0) {
		return 1; /* 没登记 = 监控不了 → 判不正常 */
	}
	if (m->nstate[idx].hb_seen == 0u) {
		return 1; /* 还没收到过第一帧 */
	}
	if ((uint32_t)(m->now_ms - m->nstate[idx].hb_last_ms) > CO_MASTER_HB_TIMEOUT_MS) {
		return 1; /* 窗口内没心跳 */
	}
	return 0;
}
