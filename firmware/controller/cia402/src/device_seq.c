/*
 * device_seq.c — 执行 CiA 402 对象读写与同步控制字序列
 *
 * 所有流程只能由服务线程执行，等待对象应答时可能阻塞。
 * 序列支持故障复位、起点快进、取消和步骤超时。
 *
 * Author: zager
 */

#include "device_internal.h"

#include <cia402/od.h>
#include <cia402/pds.h>

#include <stddef.h>

/* 利用故障复位位的上升沿清除故障，并等待退出 FAULT */
static int cia402_device_seq_reset(struct cia402_device *d, uint32_t timeout_ms);

/**
 * cia402_device_od_write - 写入一个设备对象
 * @d: 设备对象
 * @index: 对象索引
 * @sub: 子索引
 * @value: 待写值
 * @size: 字节数
 * Return: 0 成功；-1 访问接口缺失或传输失败。
 */
int cia402_device_od_write(struct cia402_device *d, uint16_t index, uint8_t sub, uint32_t value,
			   uint8_t size)
{
	if (d->access == NULL || d->access->xfer == NULL) {
		return -1;
	}
	return d->access->xfer(d->access->ctx, SDO_DIR_WRITE, index, sub, value, size, NULL, NULL,
			       CIA402_DEVICE_SDO_TIMEOUT_MS);
}

/**
 * cia402_device_od_read - 读取一个设备对象
 * @d: 设备对象
 * @index: 对象索引
 * @sub: 子索引
 * @value: 输出值
 * @len: 输出字节数
 * Return: 0 成功；-1 访问接口缺失或传输失败。
 */
int cia402_device_od_read(struct cia402_device *d, uint16_t index, uint8_t sub, uint32_t *value,
			  uint8_t *len)
{
	if (d->access == NULL || d->access->xfer == NULL) {
		return -1;
	}
	return d->access->xfer(d->access->ctx, SDO_DIR_READ, index, sub, 0u, 0u, value, len,
			       CIA402_DEVICE_SDO_TIMEOUT_MS);
}

/**
 * cia402_device_now_ms - 读取设备访问通道的单调时钟
 * @d: 设备对象
 *
 * Return: 毫秒计数；时钟接口缺失时返回 0。
 */
uint32_t cia402_device_now_ms(struct cia402_device *d)
{
	return (d->access != NULL && d->access->now_ms != NULL) ? d->access->now_ms(d->access->ctx)
								: 0u;
}

/**
 * cia402_device_cancelled - 检查当前流程是否收到取消请求
 * @d: 设备对象
 *
 * Return: 非 0 已取消；0 未取消。
 */
int cia402_device_cancelled(struct cia402_device *d)
{
	int c;

	pthread_mutex_lock(&d->lock);
	c = d->cancel != 0u;
	pthread_mutex_unlock(&d->lock);
	return c;
}

/**
 * cia402_device_write_controlword - 写入并缓存 6040 控制字
 * @d: 设备对象
 * @value: 控制字
 *
 * Return: 0 写入并缓存成功；-1 对象访问失败。
 */
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

/**
 * cia402_device_read_statusword - 读取并缓存 6041 状态字
 * @d: 设备对象
 * @out: 输出状态字
 *
 * Return: 0 读取并缓存成功；-1 对象访问失败。
 */
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

/**
 * cia402_device_seq_run - 从当前状态执行控制字序列
 * @d: 设备对象
 * @seq: 控制字序列表
 * @count: 序列长度
 * @step_timeout_ms: 单步超时
 * Return: 0 全部完成；-1 参数、访问、故障复位、取消或超时失败。
 */
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

/**
 * cia402_device_do_config - 配置并校验 PP 操作模式
 * @d: 设备对象
 *
 * Return: 0 模式一致；-1 写入、读回或校验失败。
 */
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

/**
 * cia402_device_do_enable - 执行使能控制字序列
 * @d: 设备对象
 *
 * Return: 0 已使能；-1 流程失败、取消或超时。
 */
int cia402_device_do_enable(struct cia402_device *d)
{
	uint8_t n = 0u;
	const struct cia402_seq_step *seq = cia402_seq_enable(&n);

	return cia402_device_seq_run(d, seq, n, CIA402_DEVICE_STEP_TIMEOUT_MS);
}

/**
 * cia402_device_do_disable - 执行断使能控制字序列
 * @d: 设备对象
 *
 * Return: 0 已断使能；-1 流程失败、取消或超时。
 */
int cia402_device_do_disable(struct cia402_device *d)
{
	uint8_t n = 0u;
	const struct cia402_seq_step *seq = cia402_seq_disable(&n);

	return cia402_device_seq_run(d, seq, n, CIA402_DEVICE_STEP_TIMEOUT_MS);
}

/**
 * cia402_device_do_halt - 减速停车并保持使能
 * @d: 设备对象
 *
 * Return: 0 停稳并清除 Halt 位；-1 访问失败、故障、取消或超时。
 */
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
	return cia402_device_write_controlword(
		d, (uint16_t)(d->controlword & (uint16_t)~CIA402_CW_HALT));
}

/**
 * cia402_device_do_quick_stop - 快速停止后断使能
 * @d: 设备对象
 *
 * 读状态失败或等待超时后仍尝试写断使能控制字。
 *
 * Return: 0 断使能写入成功；-1 最终断使能写入失败。
 */
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
