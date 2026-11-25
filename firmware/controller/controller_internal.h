/*
 * controller_internal.h — 控制层私有上下文与状态图契约
 *
 * controller.c 持有对象并提供推进入口，controller_actions.c 提供状态图动作，
 * controller_axis.c 提供整机级轴集合动作与汇总判定，
 * control_api.c 翻译对外 API 并发布事实，states/ 实现状态图。
 *
 * Author: zager
 */

#ifndef CONTROL_INTERNAL_H
#define CONTROL_INTERNAL_H

#include <control/control.h>

#include <master/master.h>
#include <controller_axis/controller_axis.h>
#include <sdo/sdo_client.h>

#include "evt_queue.h"

#include <pthread.h>

/* 控制层与状态图之间的事件 */
enum controller_sig {
	CONTROLLER_SIG_TICK = SM_SIG_USER,
	CONTROLLER_SIG_MOVE,
	CONTROLLER_SIG_HALT,
	CONTROLLER_SIG_ESTOP,
	CONTROLLER_SIG_ESTOP_CLEAR, /* 急停终态下唯一接受的命令 */
	CONTROLLER_SIG_ENABLE,      /* IDLE 收到它才进 ENABLING */
	CONTROLLER_SIG_HOME         /* HOLDING 收到它才进 HOMING */
};

/* 控制器的对象实体、状态图上下文和上报信息集中在这里 */
struct controller {
	struct master *master;
	struct axis axes[CONTROLLER_AXIS_COUNT];
	uint8_t axis_count;

	struct controller_move_cmd move;
	uint8_t move_done;
	uint8_t axis_done;      /* 解除急停时等待的设备位图 */
	uint8_t axis_active;    /* 本状态已受理、仍在等待的轴位图 */
	uint8_t estop_clearing; /* 非 0：已发出解除急停请求，等回执齐了再迁 IDLE */
	uint8_t node_ready;     /* 非 0：驱动器已重启完毕，允许发起对象访问 */
	uint8_t home_mask;      /* 位图：本次回零涉及哪些轴 */

	struct sm_hsm hsm;
	uint32_t state_since_ms;
	struct controller_evtq evtq;

	pthread_mutex_t info_lock;
	struct controller_info info;
};

/* 状态图注册表，由 states/states.c 定义；本层只发布枚举值，不产出字符串 */
extern const sm_state_t states[CONTROLLER_STATE_COUNT];
sm_ret_t initial(struct sm_hsm *const me, const struct sm_event *const e);

/* 状态图可直接使用主站的 NMT 与节点查询；协议栈细节仍在 canopen/ 内 */

/* 总线侧单调毫秒计数；未接管主站时返回 0 */
uint32_t controller_now_ms(struct controller *ctl);

/* 空闲通道上推进各轴反馈读取；由工作线程调用 */
void controller_feedback_poll(struct controller *ctl, uint32_t now_ms);

/* 控制器重新初始化时复位各轴反馈轮询游标 */
void controller_feedback_reset(void);

/* 装配状态图并注入主站与时钟能力（仅初始化阶段调用） */
void controller_stm_init(struct controller *app);

/* 控制层单例（由 controller.c 创建；同层另一文件通过它拿上下文） */
struct controller *control_ctx(void);

/*
 * 锁存首个故障：stage / reason / first_axis / abort_code 只在无故障时写入，
 * axis_mask 每次调用都累积。axis 不在已装配范围时按“未定位”记录。
 */
void controller_set_fault(struct controller *ctl, enum controller_state stage,
			  enum controller_fault_reason reason, uint8_t axis, uint32_t abort_code);
/* 一次集合动作可能同拍失败多轴：完整保留失败位图，主快照仍由首个故障决定 */
void controller_set_axes_fault(struct controller *ctl, enum controller_state stage,
			       const struct controller_axes_failure *failure);

/* 把 cia402 的失败归类映射为业务故障原因 */
enum controller_fault_reason controller_fault_reason_of(enum cia402_fail fail);

/* 每轮末尾将状态与 abort 汇入上报信息 */
void controller_info_update(struct controller *ctl);

#endif /* CONTROL_INTERNAL_H */
