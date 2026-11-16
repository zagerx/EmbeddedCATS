/*
 * controller_axis.h — 整机级轴集合动作与结果汇总
 *
 * 只遍历单轴接口，不访问 CAN、CiA 402 对象或控制器私有状态。
 * 轴对象由组合根持有，所有接口仅由 CANopen 工作线程调用。
 *
 * Author: zager
 */

#ifndef CONTROLLER_AXIS_H
#define CONTROLLER_AXIS_H

#include <controller_axis/axis.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 全部轴一次集合操作的共同结论；完成位图由调用方跨拍保存 */
enum controller_axes_result {
	CONTROLLER_AXES_WAITING = 0,
	CONTROLLER_AXES_DONE,
	CONTROLLER_AXES_FAILED
};

/* 配置运动模式：0 = 已下发；-1 = 轴号越界、未装配或参数为空 */
int controller_config_movemode(struct axis *axes, uint8_t count, uint8_t axis_no);

/* 请求所有轴使能：0 = 已下发；-1 = 未装配或参数为空 */
int controller_enable_all(struct axis *axes, uint8_t count);

/* 用同一时刻推进每个轴的设备状态机；时钟由调用方提供 */
void controller_axes_poll(struct axis *axes, uint8_t count, uint32_t now_ms);

/* 消费全部轴的新回执，并将 DONE 保留在 done_mask */
enum controller_axes_result controller_axes_collect(struct axis *axes, uint8_t count,
						    uint8_t *done_mask);

/* 急停所有轴：quick stop → 断使能（由各自的异步推进器走完） */
void controller_quick_stop_all(struct axis *axes, uint8_t count);

/* 位图里覆盖全部已装配轴的掩码；未装配时返回 0 */
uint8_t controller_axis_mask(uint8_t count);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CONTROLLER_AXIS_H */
