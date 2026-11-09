/*
 * drive_sm.c — 走到目标状态 + 对象读写
 *
 * 全工程唯一一处"发命令 + 等状态字"的循环。
 * 需求表达为"从当前状态走到 Operation enabled"（而不是 06→07→0F 序列），
 * 所以故障后复位、断使能后重新使能都自然成立。
 */

#include "drive_internal.h"

#include <ds402/od.h>

/* ---- 对象读写 ---- */

int drive_od_write(struct drive *d, uint16_t index, uint8_t sub, uint32_t value, uint8_t size)
{
	if (d->od == NULL || d->od->write == NULL) {
		return -1;
	}
	return d->od->write(d->od->ctx, index, sub, value, size, DRIVE_SDO_TIMEOUT_MS);
}

int drive_od_read(struct drive *d, uint16_t index, uint8_t sub, uint32_t *value, uint8_t *len)
{
	if (d->od == NULL || d->od->read == NULL) {
		return -1;
	}
	return d->od->read(d->od->ctx, index, sub, value, len, DRIVE_SDO_TIMEOUT_MS);
}

static uint32_t now_of(struct drive *d)
{
	return (d->od != NULL && d->od->now_ms != NULL) ? d->od->now_ms(d->od->ctx) : 0u;
}

int drive_write_controlword(struct drive *d, uint16_t value)
{
	if (drive_od_write(d, DS402_OD_CONTROLWORD, 0u, (uint32_t)value, 2u) != 0) {
		return -1;
	}
	d->controlword = value;
	return 0;
}

/* 读一次状态字，顺手更新事实 */
static int read_statusword(struct drive *d, uint16_t *out)
{
	uint32_t v = 0u;
	uint8_t len = 0u;

	if (drive_od_read(d, DS402_OD_STATUSWORD, 0u, &v, &len) != 0) {
		return -1;
	}
	*out = (uint16_t)(v & 0xFFFFu);
	d->statusword = *out;
	d->fb_valid |= DRIVE_FB_STATUSWORD;
	return 0;
}

/* ---- 状态机推进 ---- */

int drive_sm_run_to(struct drive *d, enum ds402_state target, uint32_t timeout_ms)
{
	const uint32_t deadline = now_of(d) + timeout_ms;

	for (;;) {
		uint16_t sw = 0u;
		enum ds402_state cur;
		struct ds402_step st;

		if (read_statusword(d, &sw) != 0) {
			return -1;
		}
		cur = ds402_status_parse(sw);
		if (cur == target) {
			return 0;
		}

		st = ds402_step(cur, target);
		if (st.cmd == DS402_CMD_NONE) {
			return -1; /* 当前状态下走不动（未准备好 / 状态字未知） */
		}
		if (drive_write_controlword(d, ds402_cw_for(st.cmd)) != 0) {
			return -1;
		}
		if ((int32_t)(now_of(d) - deadline) >= 0) {
			return -1; /* 超时 */
		}
	}
}
