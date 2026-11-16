/*
 * 整机级轴集合动作
 *
 * 只遍历单轴接口，完成位图由调用方持有；未装配（0 轴）时全部动作是空操作。
 * 不访问控制器私有上下文，保持组合根 → 轴集合 → 单轴的单向依赖。
 *
 * Author: zager
 */

#include <controller_axis/controller_axis.h>

#define CONTROLLER_AXIS_ALL UINT8_MAX

/**
 * controller_config_movemode - 给指定轴写运动模式
 * @axes: 轴数组
 * @count: 已装配轴数
 * @axis_no: 轴号；传 CONTROLLER_AXIS_ALL 表示全部轴
 *
 * 只负责下发，不看每轴结果：已配过的轴会被设备层拒绝，不算错。
 *
 * Return: 0 已下发；-1 轴号越界、未装配或参数为空。
 */
int controller_config_movemode(struct axis *axes, uint8_t count, uint8_t axis_no)
{
	uint8_t first;
	uint8_t last;
	uint8_t i;

	if (axes == NULL || count == 0u) {
		return -1;
	}
	if (axis_no == CONTROLLER_AXIS_ALL) {
		first = 0u;
		last = (uint8_t)(count - 1u);
	} else if (axis_no < count) {
		first = axis_no;
		last = axis_no;
	} else {
		return -1;
	}
	for (i = first; i <= last; i++) {
		(void)axis_config_movemode(&axes[i]);
	}
	return 0;
}

/**
 * controller_enable_all - 逐轴调用使能请求
 * @axes: 轴数组
 * @count: 已装配轴数
 *
 * Return: 0 已下发，不表示每轴均受理；-1 未装配或参数为空。
 */
int controller_enable_all(struct axis *axes, uint8_t count)
{
	uint8_t i;

	if (axes == NULL || count == 0u) {
		return -1;
	}
	for (i = 0u; i < count; i++) {
		(void)axis_enable(&axes[i]);
	}
	return 0;
}

/**
 * controller_axes_poll - 推进每个轴的设备状态机
 * @axes: 轴数组
 * @count: 已装配轴数
 * @now_ms: 本轮统一时刻，由调用方取得
 *
 * 只做遍历；时钟归组合根持有，本模块不自行读表。
 */
void controller_axes_poll(struct axis *axes, uint8_t count, uint32_t now_ms)
{
	uint8_t i;

	if (axes == NULL) {
		return;
	}
	for (i = 0u; i < count; i++) {
		axis_poll(&axes[i], now_ms);
	}
}

/**
 * controller_axes_collect - 汇总全部轴当前集合动作的回执
 * @axes: 轴数组
 * @count: 已装配轴数
 * @done_mask: 跨拍保存的已完成位图
 *
 * 优先报告失败，只有全部轴成功才报完成。仅由工作线程调用。
 *
 * Return: WAITING 等待后续回执；DONE 全部成功；FAILED 至少一轴失败。
 */
enum controller_axes_result controller_axes_collect(struct axis *axes, uint8_t count,
						    uint8_t *done_mask)
{
	uint8_t failed = 0u;
	uint8_t i;

	for (i = 0u; i < count; i++) {
		if ((*done_mask & (uint8_t)(1u << i)) != 0u) {
			continue;
		}
		switch (axis_take_motion(&axes[i])) {
		case AXIS_MOTION_FAILED:
			failed = 1u;
			break;
		case AXIS_MOTION_DONE:
			*done_mask |= (uint8_t)(1u << i);
			break;
		default:
			break;
		}
	}
	if (failed != 0u) {
		return CONTROLLER_AXES_FAILED;
	}
	if (*done_mask == controller_axis_mask(count)) {
		return CONTROLLER_AXES_DONE;
	}
	return CONTROLLER_AXES_WAITING;
}

/**
 * controller_quick_stop_all - 请求所有轴快速停止并断使能
 * @axes: 轴数组
 * @count: 已装配轴数
 *
 * Return: 无。
 */
void controller_quick_stop_all(struct axis *axes, uint8_t count)
{
	uint8_t i;

	if (axes == NULL) {
		return;
	}
	for (i = 0u; i < count; i++) {
		(void)axis_quick_stop(&axes[i]);
	}
}

/**
 * controller_axis_mask - 位图里覆盖全部已装配轴的掩码
 * @count: 已装配轴数
 *
 * Return: 掩码；未装配时返回 0，调用方据此区分“零轴”与“全部完成”。
 */
uint8_t controller_axis_mask(uint8_t count)
{
	if (count == 0u) {
		return 0u;
	}
	if (count >= 8u) {
		return 0xFFu;
	}
	return (uint8_t)((1u << count) - 1u);
}
