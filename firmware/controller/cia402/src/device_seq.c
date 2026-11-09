/* CiA 402 对象读写及同步控制字序列；只能由服务线程执行。 */

#include "device_internal.h"

#include <cia402/od.h>
#include <cia402/pds.h>

#include <stddef.h>

/* ---- 对象读写 ---- */

int cia402_device_od_write(struct cia402_device *d, uint16_t index, uint8_t sub, uint32_t value, uint8_t size)
{
	if (d->access == NULL || d->access->write == NULL) {
		return -1;
	}
	return d->access->write(d->access->ctx, index, sub, value, size, CIA402_DEVICE_SDO_TIMEOUT_MS);
}

int cia402_device_od_read(struct cia402_device *d, uint16_t index, uint8_t sub, uint32_t *value, uint8_t *len)
{
	if (d->access == NULL || d->access->read == NULL) {
		return -1;
	}
	return d->access->read(d->access->ctx, index, sub, value, len, CIA402_DEVICE_SDO_TIMEOUT_MS);
}

uint32_t cia402_device_now_ms(struct cia402_device *d)
{
	return (d->access != NULL && d->access->now_ms != NULL) ? d->access->now_ms(d->access->ctx) : 0u;
}

int cia402_device_cancelled(struct cia402_device *d)
{
	int c;

	pthread_mutex_lock(&d->lock);
	c = d->cancel != 0u;
	pthread_mutex_unlock(&d->lock);
	return c;
}

int cia402_device_write_controlword(struct cia402_device *d, uint16_t value)
{
	if (cia402_device_od_write(d, CIA402_OD_CONTROLWORD, 0u, (uint32_t)value, 2u) != 0) {
		return -1;
	}
	pthread_mutex_lock(&d->lock);
	d->controlword = value;
	pthread_mutex_unlock(&d->lock);
	return 0;
}

int cia402_device_read_statusword(struct cia402_device *d, uint16_t *out)
{
	uint32_t v = 0u;
	uint8_t len = 0u;

	if (cia402_device_od_read(d, CIA402_OD_STATUSWORD, 0u, &v, &len) != 0) {
		return -1;
	}
	*out = (uint16_t)(v & 0xFFFFu);
	pthread_mutex_lock(&d->lock);
	d->statusword = *out;
	d->fb_valid |= CIA402_DEVICE_FB_STATUSWORD;
	pthread_mutex_unlock(&d->lock);
	return 0;
}

/* ---- 故障复位：bit7 需要上升沿，所以先写 0x00 造下沿 ---- */

static int cia402_device_seq_reset(struct cia402_device *d, uint32_t timeout_ms)
{
	const uint32_t deadline = cia402_device_now_ms(d) + timeout_ms;
	uint16_t sw = 0u;

	if (cia402_device_write_controlword(d, CIA402_CW_DISABLE) != 0) {
		return -1;
	}
	if (cia402_device_write_controlword(d, CIA402_CW_FAULT_RESET) != 0) {
		return -1;
	}
	for (;;) {
		if (cia402_device_read_statusword(d, &sw) != 0) {
			return -1;
		}
		if (cia402_parse(sw) != CIA402_FAULT) {
			return 0;
		}
		if (cia402_device_cancelled(d) != 0) {
			return -1;
		}
		if ((int32_t)(cia402_device_now_ms(d) - deadline) >= 0) {
			return -1;
		}
	}
}

/* ---- 走序列表：起点快进 + 逐条"写 → 等" ---- */

int cia402_device_seq_run(struct cia402_device *d, const struct cia402_seq_step *seq, uint8_t count,
		  uint32_t step_timeout_ms)
{
	uint16_t sw = 0u;
	uint8_t i;

	if (seq == NULL || count == 0u) {
		return -1;
	}
	if (cia402_device_read_statusword(d, &sw) != 0) {
		return -1;
	}
	/* 在故障里：先复位，之后重新规划（序列不替设备决定怎么复位） */
	if (cia402_parse(sw) == CIA402_FAULT) {
		if (cia402_device_seq_reset(d, step_timeout_ms * 2u) != 0) {
			return -1;
		}
		if (cia402_device_read_statusword(d, &sw) != 0) {
			return -1;
		}
	}

	/* 起点快进：已经走过的不再走一遍 */
	i = cia402_seq_skip(sw, seq, count);
	for (; i < count; i++) {
		const uint32_t deadline = cia402_device_now_ms(d) + step_timeout_ms;

		if (cia402_device_cancelled(d) != 0) {
			return -1;
		}
		if (cia402_device_write_controlword(d, seq[i].cw) != 0) {
			return -1;
		}
		for (;;) {
			if (cia402_device_read_statusword(d, &sw) != 0) {
				return -1;
			}
			if ((sw & seq[i].exp_mask) == seq[i].exp_value) {
				break; /* 这一步到位 */
			}
			if (cia402_parse(sw) == CIA402_FAULT) {
				return -1;
			}
			if (cia402_device_cancelled(d) != 0) {
				return -1;
			}
			if ((int32_t)(cia402_device_now_ms(d) - deadline) >= 0) {
				return -1; /* 卡在这一步 */
			}
		}
	}
	return 0;
}

/* ---- 具体流程 ---- */

int cia402_device_do_config(struct cia402_device *d)
{
	uint32_t v = 0u;
	uint8_t len = 0u;

	if (cia402_device_od_write(d, CIA402_OD_MODE, 0u, CIA402_MODE_PP, 1u) != 0) {
		return -1;
	}
	if (cia402_device_od_read(d, CIA402_OD_MODE_ACTUAL, 0u, &v, &len) != 0) {
		return -1;
	}
	return ((v & 0xFFu) == CIA402_MODE_PP) ? 0 : -1;
}

int cia402_device_do_enable(struct cia402_device *d)
{
	uint8_t n = 0u;
	const struct cia402_seq_step *seq = cia402_seq_enable(&n);

	return cia402_device_seq_run(d, seq, n, CIA402_DEVICE_STEP_TIMEOUT_MS);
}

int cia402_device_do_disable(struct cia402_device *d)
{
	uint8_t n = 0u;
	const struct cia402_seq_step *seq = cia402_seq_disable(&n);

	return cia402_device_seq_run(d, seq, n, CIA402_DEVICE_STEP_TIMEOUT_MS);
}

int cia402_device_do_halt(struct cia402_device *d)
{
	const uint32_t deadline = cia402_device_now_ms(d) + CIA402_DEVICE_HALT_TIMEOUT_MS;
	uint16_t sw = 0u;

	if (cia402_device_write_controlword(d, (uint16_t)(d->controlword | CIA402_CW_HALT)) != 0) {
		return -1;
	}
	for (;;) {
		if (cia402_device_read_statusword(d, &sw) != 0) {
			return -1;
		}
		if (cia402_parse(sw) == CIA402_FAULT) {
			return -1;
		}
		if ((sw & CIA402_SW_TARGET_REACHED) != 0u) {
			break; /* 停稳 */
		}
		if (cia402_device_cancelled(d) != 0) {
			return -1;
		}
		if ((int32_t)(cia402_device_now_ms(d) - deadline) >= 0) {
			return -1;
		}
	}
	/* 停稳后清 bit8：保持使能（有保持力矩），可以立刻接下一句运动 */
	return cia402_device_write_controlword(d, (uint16_t)(d->controlword & (uint16_t)~CIA402_CW_HALT));
}

int cia402_device_do_quick_stop(struct cia402_device *d)
{
	const uint32_t deadline = cia402_device_now_ms(d) + CIA402_DEVICE_QUICK_STOP_TIMEOUT_MS;
	uint16_t sw = 0u;

	(void)cia402_device_write_controlword(d, CIA402_CW_QUICK_STOP); /* 写不进去也继续往下 */

	for (;;) {
		if (cia402_device_read_statusword(d, &sw) != 0) {
			break; /* 读不到也要断使能 */
		}
		if (cia402_parse(sw) == CIA402_SWITCH_ON_DISABLED ||
		    cia402_parse(sw) == CIA402_NOT_READY) {
			break; /* 已无输出力矩 */
		}
		if ((int32_t)(cia402_device_now_ms(d) - deadline) >= 0) {
			break;
		}
	}
	/* 出口是"没有力矩" */
	return cia402_device_write_controlword(d, CIA402_CW_DISABLE);
}
