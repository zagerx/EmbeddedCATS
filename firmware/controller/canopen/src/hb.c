/*
 * hb.c — 心跳状态（纯时间戳，不碰总线、不知道有几个站）
 */

#include <canopen/hb.h>

#include <string.h>

void co_hb_reset(struct co_hb *hb)
{
	if (hb != NULL) {
		memset(hb, 0, sizeof(*hb));
	}
}

void co_hb_on_frame(struct co_hb *hb, const uint8_t data[8], uint32_t now_ms)
{
	if (hb == NULL || data == NULL) {
		return;
	}
	hb->last_ms = now_ms;
	hb->seen = 1u;
	hb->nmt_state = data[0];
}

int co_hb_lost(const struct co_hb *hb, uint32_t now_ms, uint32_t timeout_ms)
{
	if (hb == NULL || hb->seen == 0u) {
		return 1; /* 还没收到过第一帧 */
	}
	if ((uint32_t)(now_ms - hb->last_ms) > timeout_ms) {
		return 1; /* 窗口内没心跳 */
	}
	return 0;
}
