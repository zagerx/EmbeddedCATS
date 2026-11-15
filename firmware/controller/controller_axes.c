/*
 * controller_axes.c — 整机级轴集合动作
 *
 * 集合的所有者是组合根，所以批量动作放在这一层；axis 层只暴露单轴接口，
 * 本文件用 ctl->axis_count 作为循环上界，未装配（0 轴）时全部动作是空操作。
 * 每轴回执不在这里汇总：由发起动作的状态自己消费（axis_take_motion）。
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
 * controller_axis_mask - 位图里覆盖全部已装配轴的掩码
 * @ctl: 控制上下文；NULL 安全
 *
 * Return: 掩码；未装配时返回 0，调用方据此区分“零轴”与“全部完成”。
 */
uint8_t controller_axis_mask(const struct controller *ctl)
{
	if (ctl == NULL || ctl->axis_count == 0u) {
		return 0u;
	}
	if (ctl->axis_count >= 8u) {
		return 0xFFu;
	}
	return (uint8_t)((1u << ctl->axis_count) - 1u);
}
