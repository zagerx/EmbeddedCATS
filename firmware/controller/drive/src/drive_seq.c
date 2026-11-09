/*
 * drive_seq.c — 402 流程：对象读写 + 序列表推进
 *
 * 全工程唯一一处"写控制字 + 等状态字"的循环（drive_seq_run）。
 * 它同步阻塞：等状态字时读对象，读对象阻塞在 condvar 上（让出 CPU）。
 *
 * 序列本身（06→07→0F、期望 0x21/0x23/0x27）在 ds402 里，
 * 本文件只负责：发出去、等回来、超时、被取消、遇故障。
 */

#include "drive_internal.h"

#include <ds402/od.h>
#include <ds402/pds.h>

#include <stddef.h>

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

uint32_t drive_now_ms(struct drive *d)
{
	return (d->od != NULL && d->od->now_ms != NULL) ? d->od->now_ms(d->od->ctx) : 0u;
}

int drive_cancelled(struct drive *d)
{
	int c;

	pthread_mutex_lock(&d->lock);
	c = d->cancel != 0u;
	pthread_mutex_unlock(&d->lock);
	return c;
}

int drive_write_controlword(struct drive *d, uint16_t value)
{
	if (drive_od_write(d, DS402_OD_CONTROLWORD, 0u, (uint32_t)value, 2u) != 0) {
		return -1;
	}
	pthread_mutex_lock(&d->lock);
	d->controlword = value;
	pthread_mutex_unlock(&d->lock);
	return 0;
}

int drive_read_statusword(struct drive *d, uint16_t *out)
{
	uint32_t v = 0u;
	uint8_t len = 0u;

	if (drive_od_read(d, DS402_OD_STATUSWORD, 0u, &v, &len) != 0) {
		return -1;
	}
	*out = (uint16_t)(v & 0xFFFFu);
	pthread_mutex_lock(&d->lock);
	d->statusword = *out;
	d->fb_valid |= DRIVE_FB_STATUSWORD;
	pthread_mutex_unlock(&d->lock);
	return 0;
}

/* ---- 故障复位：bit7 需要上升沿，所以先写 0x00 造下沿 ---- */

static int drive_seq_reset(struct drive *d, uint32_t timeout_ms)
{
	const uint32_t deadline = drive_now_ms(d) + timeout_ms;
	uint16_t sw = 0u;

	if (drive_write_controlword(d, DS402_CW_DISABLE) != 0) {
		return -1;
	}
	if (drive_write_controlword(d, DS402_CW_FAULT_RESET) != 0) {
		return -1;
	}
	for (;;) {
		if (drive_read_statusword(d, &sw) != 0) {
			return -1;
		}
		if (ds402_parse(sw) != DS402_FAULT) {
			return 0;
		}
		if (drive_cancelled(d) != 0) {
			return -1;
		}
		if ((int32_t)(drive_now_ms(d) - deadline) >= 0) {
			return -1;
		}
	}
}

/* ---- 走序列表：起点快进 + 逐条"写 → 等" ---- */

int drive_seq_run(struct drive *d, const struct ds402_seq_step *seq, uint8_t count,
		  uint32_t step_timeout_ms)
{
	uint16_t sw = 0u;
	uint8_t i;

	if (seq == NULL || count == 0u) {
		return -1;
	}
	if (drive_read_statusword(d, &sw) != 0) {
		return -1;
	}
	/* 在故障里：先复位，之后重新规划（序列不替设备决定怎么复位） */
	if (ds402_parse(sw) == DS402_FAULT) {
		if (drive_seq_reset(d, step_timeout_ms * 2u) != 0) {
			return -1;
		}
		if (drive_read_statusword(d, &sw) != 0) {
			return -1;
		}
	}

	/* 起点快进：已经走过的不再走一遍 */
	i = ds402_seq_skip(sw, seq, count);
	for (; i < count; i++) {
		const uint32_t deadline = drive_now_ms(d) + step_timeout_ms;

		if (drive_cancelled(d) != 0) {
			return -1;
		}
		if (drive_write_controlword(d, seq[i].cw) != 0) {
			return -1;
		}
		for (;;) {
			if (drive_read_statusword(d, &sw) != 0) {
				return -1;
			}
			if ((sw & seq[i].exp_mask) == seq[i].exp_value) {
				break; /* 这一步到位 */
			}
			if (ds402_parse(sw) == DS402_FAULT) {
				return -1;
			}
			if (drive_cancelled(d) != 0) {
				return -1;
			}
			if ((int32_t)(drive_now_ms(d) - deadline) >= 0) {
				return -1; /* 卡在这一步 */
			}
		}
	}
	return 0;
}

/* ---- 具体流程 ---- */

int drive_do_config(struct drive *d)
{
	uint32_t v = 0u;
	uint8_t len = 0u;

	if (drive_od_write(d, DS402_OD_MODE, 0u, DS402_MODE_PP, 1u) != 0) {
		return -1;
	}
	if (drive_od_read(d, DS402_OD_MODE_ACTUAL, 0u, &v, &len) != 0) {
		return -1;
	}
	return ((v & 0xFFu) == DS402_MODE_PP) ? 0 : -1;
}

int drive_do_enable(struct drive *d)
{
	uint8_t n = 0u;
	const struct ds402_seq_step *seq = ds402_seq_enable(&n);

	return drive_seq_run(d, seq, n, DRIVE_STEP_TIMEOUT_MS);
}

int drive_do_disable(struct drive *d)
{
	uint8_t n = 0u;
	const struct ds402_seq_step *seq = ds402_seq_disable(&n);

	return drive_seq_run(d, seq, n, DRIVE_STEP_TIMEOUT_MS);
}

int drive_do_halt(struct drive *d)
{
	const uint32_t deadline = drive_now_ms(d) + DRIVE_HALT_TIMEOUT_MS;
	uint16_t sw = 0u;

	if (drive_write_controlword(d, (uint16_t)(d->controlword | DS402_CW_HALT)) != 0) {
		return -1;
	}
	for (;;) {
		if (drive_read_statusword(d, &sw) != 0) {
			return -1;
		}
		if (ds402_parse(sw) == DS402_FAULT) {
			return -1;
		}
		if ((sw & DS402_SW_TARGET_REACHED) != 0u) {
			break; /* 停稳 */
		}
		if (drive_cancelled(d) != 0) {
			return -1;
		}
		if ((int32_t)(drive_now_ms(d) - deadline) >= 0) {
			return -1;
		}
	}
	/* 停稳后清 bit8：保持使能（有保持力矩），可以立刻接下一句运动 */
	return drive_write_controlword(d, (uint16_t)(d->controlword & (uint16_t)~DS402_CW_HALT));
}

int drive_do_quick_stop(struct drive *d)
{
	const uint32_t deadline = drive_now_ms(d) + DRIVE_QUICK_STOP_TIMEOUT_MS;
	uint16_t sw = 0u;

	(void)drive_write_controlword(d, DS402_CW_QUICK_STOP); /* 写不进去也继续往下 */

	for (;;) {
		if (drive_read_statusword(d, &sw) != 0) {
			break; /* 读不到也要断使能 */
		}
		if (ds402_parse(sw) == DS402_SWITCH_ON_DISABLED ||
		    ds402_parse(sw) == DS402_NOT_READY) {
			break; /* 已无输出力矩 */
		}
		if ((int32_t)(drive_now_ms(d) - deadline) >= 0) {
			break;
		}
	}
	/* 出口是"没有力矩" */
	return drive_write_controlword(d, DS402_CW_DISABLE);
}
