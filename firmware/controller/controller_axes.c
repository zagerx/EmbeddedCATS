/*
 * controller_axes.c — 整机级轴集合动作与汇总判定
 *
 * 集合的所有者是组合根，所以批量动作与"全部/任一"判定放在这一层；
 * axis 层只暴露单轴接口，本文件用 ctl->axis_count 作为循环上界，
 * 未装配（0 轴）时全部动作是空操作，判定一律回答 0。
 *
 * Author: zager
 */

#include "controller_internal.h"

/**
 * controller_config_movemode - 给指定轴写运动模式
 * @ctl: 控制上下文
 * @axis_no: 轴号；传 AXIS_ALL 表示全部轴
 *
 * 只负责下发，不看每轴结果：已配过的轴会被设备层拒绝，不算错。
 *
 * Return: 0 已下发；-1 轴号越界、未装配或参数为空。
 */
int controller_config_movemode(struct controller *ctl, uint8_t axis_no)
{
	uint8_t first;
	uint8_t last;
	uint8_t i;

	if (ctl == NULL || ctl->axis_count == 0u) {
		return -1;
	}
	if (axis_no == AXIS_ALL) {
		first = 0u;
		last = (uint8_t)(ctl->axis_count - 1u);
	} else if (axis_no < ctl->axis_count) {
		first = axis_no;
		last = axis_no;
	} else {
		return -1;
	}
	for (i = first; i <= last; i++) {
		(void)axis_config_movemode(&ctl->axes[i]);
	}
	return 0;
}

/**
 * controller_enable_all - 逐轴调用使能请求
 * @ctl: 控制上下文
 *
 * Return: 0 已下发，不表示每轴均受理；-1 未装配或参数为空。
 */
int controller_enable_all(struct controller *ctl)
{
	uint8_t i;

	if (ctl == NULL || ctl->axis_count == 0u) {
		return -1;
	}
	for (i = 0; i < ctl->axis_count; i++) {
		(void)axis_enable(&ctl->axes[i]);
	}
	return 0;
}

/**
 * controller_quick_stop_all - 请求所有轴快速停止并断使能
 * @ctl: 控制上下文；NULL 时直接返回
 */
void controller_quick_stop_all(struct controller *ctl)
{
	uint8_t i;

	if (ctl == NULL) {
		return;
	}
	for (i = 0; i < ctl->axis_count; i++) {
		(void)axis_quick_stop(&ctl->axes[i]);
	}
}

/**
 * controller_all_configured - 检查是否所有轴均已配置
 * @ctl: 控制上下文；NULL 安全
 *
 * Return: 1 全部配置；0 存在未配置轴或未装配。
 */
int controller_all_configured(const struct controller *ctl)
{
	struct cia402_status status;
	uint8_t i;

	if (ctl == NULL || ctl->axis_count == 0u) {
		return 0;
	}
	for (i = 0; i < ctl->axis_count; i++) {
		cia402_get_status(&ctl->axes[i].dev, &status);
		if (status.configured == 0u) {
			return 0;
		}
	}
	return 1;
}

/**
 * controller_all_enabled - 检查是否所有轴均已使能
 * @ctl: 控制上下文；NULL 安全
 *
 * Return: 1 全部使能；0 存在未使能轴或未装配。
 */
int controller_all_enabled(const struct controller *ctl)
{
	struct cia402_status status;
	uint8_t i;

	if (ctl == NULL || ctl->axis_count == 0u) {
		return 0;
	}
	for (i = 0; i < ctl->axis_count; i++) {
		cia402_get_status(&ctl->axes[i].dev, &status);
		if (status.enabled == 0u) {
			return 0;
		}
	}
	return 1;
}

/**
 * controller_any_failed - 检查是否存在失败轴
 * @ctl: 控制上下文；NULL 安全
 *
 * Return: 1 至少一轴失败；0 无失败轴或未装配。
 */
int controller_any_failed(const struct controller *ctl)
{
	struct cia402_status status;
	uint8_t i;

	if (ctl == NULL) {
		return 0;
	}
	for (i = 0; i < ctl->axis_count; i++) {
		cia402_get_status(&ctl->axes[i].dev, &status);
		if (status.failed != 0u) {
			return 1;
		}
	}
	return 0;
}
