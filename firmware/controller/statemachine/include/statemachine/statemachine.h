/*
 * 业务状态机：读取心跳和轴事实，下发 NMT 与轴动作请求。
 * 控制层通过 controller_poll() 直接分发事件，本层不直接访问协议。
 */

#ifndef STATEMACHINE_STATEMACHINE_H
#define STATEMACHINE_STATEMACHINE_H

#include <statemachine/state_types.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct cats_axis;
struct controller_sm; /* 不透明：定义在 statemachine/src/statemachine_internal.h */

/* 组合根注入的能力：本层自己不碰 canopen，也不需要知道时钟从哪来 */
struct stm_ops {
	void (*nmt_start)(void *ctx);      /* 广播 Start Remote Node */
	void (*nmt_reset_comm)(void *ctx); /* 广播 Reset Communication */
	uint32_t (*now_ms)(void *ctx);
	void *ctx;
};

/* 一条位置指令（业务单位） */
struct stm_move {
	uint8_t axis;
	float mm;
	float mm_s;
	float mm_s2;
};

void stm_init(struct controller_sm *a, struct cats_axis *axes, uint8_t axis_count,
	      const struct stm_ops *ops);

/* 只读事实 */
enum cats_state stm_state(const struct controller_sm *a);
enum cats_fault stm_fault(const struct controller_sm *a);

/* 名字表（日志/界面用；未知值返回 "?"） */
const char *stm_state_name(enum cats_state s);
const char *stm_fault_name(enum cats_fault f);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* STATEMACHINE_STATEMACHINE_H */
