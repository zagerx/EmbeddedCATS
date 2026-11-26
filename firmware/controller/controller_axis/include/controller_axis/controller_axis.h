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

/* 轴号参数里表示“全部已装配轴” */
#define CONTROLLER_AXIS_ALL UINT8_MAX

/* 全部轴一次集合操作的共同结论；完成位图由调用方跨拍保存 */
enum controller_axes_result {
	CONTROLLER_AXES_WAITING = 0,
	CONTROLLER_AXES_DONE,
	CONTROLLER_AXES_FAILED
};

/* 集合动作只收集真实失败；取消没有操作结局，不进入这里 */
struct controller_axes_failure {
	uint8_t mask;              /* 真正失败的轴位图 */
	uint8_t first_axis;        /* 本拍首个失败轴号 */
	struct axis_failure first; /* 首个失败轴的归类 */
};

/* 配置运动模式：0 = 已下发；-1 = 轴号越界、未装配或参数为空 */
int controller_config_movemode(struct axis *axes, uint8_t count, uint8_t axis_no);

/* 请求所有轴使能：0 = 全部受理；-1 = 未装配或至少一轴未受理 */
int controller_enable_all(struct axis *axes, uint8_t count);

/* 请求指定轴回原点：0 = 全部受理；-1 = 轴号越界、未装配或至少一轴未受理 */
int controller_home_axes(struct axis *axes, uint8_t count, uint8_t axis_no);

/* 解除所有轴急停：写 6040 = 0x0F 让驱动器回可操作；0 = 全部受理 */
int controller_clear_estop_all(struct axis *axes, uint8_t count);

/* 用同一时刻推进每个轴的设备状态机；时钟由调用方提供 */
void controller_axes_poll(struct axis *axes, uint8_t count, uint32_t now_ms);

/* 汇总本次等待轴；取消只结束等待，DONE 表示等待集合清空，不表示每轴成功 */
enum controller_axes_result controller_axes_collect(struct axis *axes, uint8_t count,
						    uint8_t *done_mask,
						    struct controller_axes_failure *failure);

/* 急停所有轴：quick stop → 断使能（由各自的异步推进器走完） */
int controller_quick_stop_all(struct axis *axes, uint8_t count);

/* 位图里覆盖全部已装配轴的掩码；未装配时返回 0 */
uint8_t controller_axis_mask(uint8_t count);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CONTROLLER_AXIS_H */
