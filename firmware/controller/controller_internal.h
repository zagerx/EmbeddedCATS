/*
 * controller_internal.h — 控制层私有上下文与状态图契约
 *
 * controller.c 持有对象并提供推进入口，controller_actions.c 提供状态图动作，
 * controller_axes.c 提供整机级轴集合动作与汇总判定，
 * control_api.c 翻译对外 API 并发布事实，states/ 实现状态图。
 *
 * Author: zager
 */

#ifndef CONTROL_INTERNAL_H
#define CONTROL_INTERNAL_H

#include <control/control.h>

#include <master/master.h>
#include <axis/axis.h>
#include <cia402/device.h>
#include <transport/sdo_client.h>

#include "evt_queue.h"

#include <pthread.h>

/* 控制层与状态图之间的事件 */
enum controller_sig {
	CONTROLLER_SIG_TICK = SM_SIG_USER,
	CONTROLLER_SIG_MOVE,
	CONTROLLER_SIG_STOP,
	CONTROLLER_SIG_ESTOP,
	CONTROLLER_SIG_ENABLE /* IDLE 收到它才进 ENABLING */
};

/* 控制器的对象实体、状态图上下文和跨线程发布值集中在这里 */
struct controller {
	struct master *master;
	struct axis axes[CONTROLLER_AXIS_COUNT];
	uint8_t axis_count;

	uint8_t hb_ok;
	struct controller_move_cmd move;
	uint8_t move_done;
	struct sm_hsm hsm;
	uint32_t state_since_ms;
	enum controller_fault fault_code;
	struct controller_evtq evtq;

	pthread_mutex_t lock;
	int published_state;
	int published_fault;
	struct controller_axis_sample published_axes[CONTROLLER_AXIS_COUNT];
	uint32_t published_abort[CONTROLLER_AXIS_COUNT];
};

/* 状态图注册表，由 states/states.c 定义；名称映射是对外接口（见 control.h） */
extern const sm_state_t states[CONTROLLER_STATE_COUNT];
sm_ret_t initial(struct sm_hsm *const me, const struct sm_event *const e);

/* 状态图可直接使用主站的 NMT 与节点查询；协议栈细节仍在 canopen/ 内 */

/* 总线侧单调毫秒计数；未接管主站时返回 0 */
uint32_t controller_now_ms(struct controller *ctl);

/* 装配状态图并注入主站与时钟能力（仅初始化阶段调用） */
void controller_stm_init(struct controller *app);

/* 控制层单例（由 controller.c 创建；同层另一文件通过它拿上下文） */
struct controller *control_ctx(void);

/* 把其他线程可以看的值发布出去（只在控制线程里调） */
void controller_publish(void);

/* axis_no 取此值表示"全部轴"；它不是轴身份，故不进枚举 */
#define AXIS_ALL UINT8_MAX

/*
 * 整机级轴集合动作（controller_axes.c）：只负责下发，不看每轴结果，
 * 拒绝与失败由设备层记录，状态图用 controller_any_failed() /
 * controller_all_configured() 判断。
 * 返回：0 已下发；-1 轴号越界、未装配或参数为空。
 */
int controller_config_movemode(struct controller *ctl, uint8_t axis_no);

/* 请求所有轴使能：0 = 已下发；-1 = 未装配或参数为空 */
int controller_enable_all(struct controller *ctl);

/* 急停所有轴：quick stop → 断使能（由各自的异步推进器走完） */
void controller_quick_stop_all(struct controller *ctl);

/* 汇总判定：1 = 满足条件；0 = 不满足或未装配 */
int controller_all_configured(const struct controller *ctl);
int controller_all_enabled(const struct controller *ctl);
int controller_any_failed(const struct controller *ctl);

#endif /* CONTROL_INTERNAL_H */
