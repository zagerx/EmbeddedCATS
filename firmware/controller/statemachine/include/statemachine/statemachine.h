/*
 * statemachine.h — 本机业务状态机（对外接口）
 *
 * 只做一件事：**拿事实、做决定、下请求**。
 *   - 输入事实：心跳是否正常、各轴是否已配置/已使能/有故障/运动结论、轴样本
 *   - 输出动作：NMT 启停（由组合根注入的回调）、轴的配置/使能/运动/停
 *
 * 本层不知道：402 状态字、对象号、SDO/PDO、CAN 帧、线程。
 * 它跑在控制线程里，每拍被 stm_poll() 推进一步。
 */

#ifndef STATEMACHINE_STATEMACHINE_H
#define STATEMACHINE_STATEMACHINE_H

#include <statemachine/state_types.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct cats_axis;
struct stm; /* 不透明：定义在 statemachine/src/statemachine_internal.h */

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

/* 业务命令类别（线程安全入队，由控制线程下一拍消费） */
enum stm_cmd {
	STM_CMD_MOVE = 0, /* 排到队尾 */
	STM_CMD_STOP,     /* 插到队头（减速停） */
	STM_CMD_ESTOP     /* 插到队头（急停） */
};

/* 单实例：一个进程一套业务状态机 */
struct stm *stm_instance(void);
void stm_init(struct stm *a, struct cats_axis *axes, uint8_t axis_count, const struct stm_ops *ops);

/* 推进一次（控制线程）：hb_ok 是本拍的心跳事实 */
void stm_poll(struct stm *a, uint8_t hb_ok, uint32_t now_ms);

/* 入队一条命令：CATS_OK / CATS_ERR_BUSY（队列满）/ CATS_ERR_PARAM */
enum cats_result stm_command(struct stm *a, enum stm_cmd kind, const struct stm_move *mv);

/* 只读事实 */
enum cats_state stm_state(const struct stm *a);
enum cats_fault stm_fault(const struct stm *a);

/* 名字表（日志/界面用；未知值返回 "?"） */
const char *stm_state_name(enum cats_state s);
const char *stm_fault_name(enum cats_fault f);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* STATEMACHINE_STATEMACHINE_H */
