/*
 * root.c — 组合根：装配全部对象 + 对外接口（**不含状态逻辑**）
 *
 * 职责：把对外 API 翻译成状态机事件，其余一概不管。
 *   状态逻辑在 statemachine/src/states/state_*.c（一状态一文件），
 *   状态机机制在 hsm/src/sm_hsm.c，状态注册在 states.c，
 *   内部上下文在 root_internal.h。
 *   本文件**不创建线程**，节拍由宿主编排。
 */

#include <control/control.h>

#include "root_internal.h"

static struct cats_control s_ctl = {.lock = PTHREAD_MUTEX_INITIALIZER};

/* ---- 注入给状态机的能力：它不认识 canopen，也不自己取时钟 ---- */

static void nmt_start_cb(void *ctx)
{
	struct cats_control *ctl = ctx;

	(void)master_set_state(ctl->master, CO_NMT_ALL_NODES, CO_NMT_START);
}

static void nmt_reset_cb(void *ctx)
{
	struct cats_control *ctl = ctx;

	(void)master_set_state(ctl->master, CO_NMT_ALL_NODES, CO_NMT_RESET_COMMUNICATION);
}

static uint32_t now_ms_cb(void *ctx)
{
	struct cats_control *ctl = ctx;
	struct can_io *bus = master_bus(ctl->master);

	return bus != NULL ? can_io_now_ms(bus) : 0u;
}

uint32_t now_ms(struct cats_control *ctl)
{
	return now_ms_cb(ctl);
}

const char *cats_control_state_name(enum cats_state s)
{
	return stm_state_name(s);
}

const char *cats_control_fault_name(enum cats_fault f)
{
	return stm_fault_name(f);
}

/* 把"其他线程可以看"的值发布出去。**只在控制线程里调用** */
static void publish_readonly(struct cats_control *app)
{
	int state = CATS_STATE_PREPARING;
	struct cats_axis_sample samples[CATS_MAX_AXES];
	uint8_t i;

	state = (int)stm_state(app->stm);
	/* 换算在锁外做（axis_sample 只读本线程写的字段，不做 I/O） */
	for (i = 0; i < CATS_MAX_AXES; i++) {
		struct axis_sample s;

		axis_sample(&app->axes[i], &s);
		samples[i].position_mm = s.position_mm;
		samples[i].velocity_mm_s = s.velocity_mm_s;
		samples[i].statusword = s.statusword;
		samples[i].error_code = s.error_code;
		samples[i].valid = s.valid;
	}
	pthread_mutex_lock(&app->lock);
	app->published_state = state;
	app->published_fault = (int)stm_fault(app->stm);
	for (i = 0; i < CATS_MAX_AXES; i++) {
		app->published_axes[i] = samples[i];
	}
	pthread_mutex_unlock(&app->lock);
}

/* ---- 对外接口 ---- */

enum cats_result cats_control_init(struct master *master)
{
	uint8_t i;

	if (master == NULL) {
		return CATS_ERR_PARAM;
	}
	if (s_ctl.master != NULL) {
		return CATS_ERR_STATE; /* 只初始化一次 */
	}

	s_ctl.master = master;
	/* 轴 = 主站上已登记的从站，站点号只在装配层声明一次，这里读回来 */
	s_ctl.axis_count = master_node_count(master) < CATS_MAX_AXES ? master_node_count(master)
								     : CATS_MAX_AXES;
	for (i = 0; i < s_ctl.axis_count; i++) {
		uint8_t node_id = master_node_id(master, i);
		struct co_sdo *sdo = master_sdo(master, node_id);

		/* 每轴一条 SDO 通道：I/O 线程执行，服务线程阻塞等待 */
		od_sdo_init(&s_ctl.sdo[i], sdo);
		s_ctl.od[i] = od_sdo_iface(&s_ctl.sdo[i]);
		drive_init(&s_ctl.axes[i].drv, sdo, &s_ctl.od[i], node_id);
		axis_init(&s_ctl.axes[i], node_id);
	}

	/* 初态：initial() 返回 SM_TRAN(&st_preparing)，内核把 sm_top → ROOT → PREPARING
	 * 的 ENTRY 链走一遍，广播复位从 PREPARING 的 ENTRY 里发出 */
	{
		struct stm_ops ops;

		ops.nmt_start = nmt_start_cb;
		ops.nmt_reset_comm = nmt_reset_cb;
		ops.now_ms = now_ms_cb;
		ops.ctx = &s_ctl;
		s_ctl.stm = stm_instance();
		stm_init(s_ctl.stm, s_ctl.axes, s_ctl.axis_count, &ops);
	}

	publish_readonly(&s_ctl);
	/* 线程由宿主起（见 control.h 的三个步骤说明） */
	return CATS_OK;
}

/*
 * 心跳：本函数只把**事实**算好存进 ctl->hb_ok（每拍一次，所有状态读同一个值）。
 *
 * “掉线就进 FAULT”的判定不在这里，而在 st_online（运行期父状态）：
 * 它是 ONLINE 子状态的公共规则，写成层次比写成状态白名单可靠——
 * 新增一个运行期状态时不需要回来改任何名单。
 * FAULT / ESTOP 挂在 root 下，自然也拿不到这条规则。
 */
static uint8_t heartbeat_fact(const struct cats_control *ctl)
{
	uint8_t ok = ctl->axis_count > 0 ? 1u : 0u;
	uint8_t i;

	/* 轴数为 0 说明没装配上：没有可确认的节点，不能报健康 */
	for (i = 0; i < ctl->axis_count; i++) {
		if (master_node_alive(ctl->master, ctl->axes[i].node_id) != 0) {
			ok = 0u;
			break;
		}
	}
	return ok;
}

enum cats_result cats_control_poll(void)
{
	uint32_t now;

	if (s_ctl.master == NULL) {
		return CATS_ERR_STATE;
	}
	now = now_ms(&s_ctl);

	/* 1. 收帧在 I/O 线程、402/SDO 推进在服务线程：本拍只读事实 */
	/* 2. 事实交给业务状态机：它自己消费事件、走一拍节拍 */
	stm_poll(s_ctl.stm, heartbeat_fact(&s_ctl), now);

	publish_readonly(&s_ctl);
	return CATS_OK;
}

void cats_control_estop(void)
{
	if (s_ctl.master == NULL) {
		return;
	}
	/* 插到队头：优先于已排队的所有命令 */
	(void)stm_command(s_ctl.stm, STM_CMD_ESTOP, NULL);
}

enum cats_result cats_control_move_to(uint8_t axis, float mm, float mm_s, float mm_s2)
{
	struct stm_move mv;

	if (s_ctl.master == NULL || axis >= s_ctl.axis_count) {
		return CATS_ERR_PARAM;
	}
	/* NaN 进到整数换算就是未定义行为：在入口就拦住（±inf 由轴层饱和处理） */
	if (mm != mm || mm_s != mm_s || mm_s2 != mm_s2) {
		return CATS_ERR_PARAM;
	}

	mv.axis = axis;
	mv.mm = mm;
	mv.mm_s = mm_s;
	mv.mm_s2 = mm_s2;
	/* 排到队尾：多条指令按顺序保留；队列满则本条被丢弃 */
	return stm_command(s_ctl.stm, STM_CMD_MOVE, &mv);
}

void cats_control_stop(void)
{
	if (s_ctl.master == NULL) {
		return;
	}
	/* 停止也插队：排在已有序命令前面 */
	(void)stm_command(s_ctl.stm, STM_CMD_STOP, NULL);
}

enum cats_fault cats_control_fault_code(void)
{
	enum cats_fault f;

	pthread_mutex_lock(&s_ctl.lock);
	f = (enum cats_fault)s_ctl.published_fault;
	pthread_mutex_unlock(&s_ctl.lock);
	return f;
}

enum cats_state cats_control_state(void)
{
	enum cats_state s;

	pthread_mutex_lock(&s_ctl.lock);
	s = (enum cats_state)s_ctl.published_state;
	pthread_mutex_unlock(&s_ctl.lock);
	return s;
}

uint32_t cats_control_axis_abort(uint8_t axis)
{
	if (axis >= s_ctl.axis_count) {
		return 0U;
	}
	return axis_abort_code(&s_ctl.axes[axis]);
}

enum cats_result cats_control_axis_sample(uint8_t axis, struct cats_axis_sample *out)
{
	if (out == NULL || axis >= CATS_MAX_AXES) {
		return CATS_ERR_PARAM;
	}
	pthread_mutex_lock(&s_ctl.lock);
	*out = s_ctl.published_axes[axis];
	pthread_mutex_unlock(&s_ctl.lock);
	return CATS_OK;
}

/* ---- 推进：本层只提供"推进一步"，线程与节拍归宿主 ---- */

void cats_control_io_poll(void)
{
	uint32_t now;
	uint8_t i;

	if (s_ctl.master == NULL) {
		return;
	}
	now = now_ms(&s_ctl);

	/* 一拍三件事，顺序有意义：
	 *   ① 收：应答落到各从站的事务槽里
	 *   ② 搬：服务线程的请求提交给协议 / 结果搬回 od_sdo
	 *   ③ 发：该发的帧当拍发出去（放在 ② 之后，请求不用多等一拍） */
	master_recv_step(s_ctl.master, now);
	for (i = 0u; i < s_ctl.axis_count; i++) {
		od_sdo_io_step(&s_ctl.sdo[i]);
	}
	master_send_step(s_ctl.master, now);
}

void cats_control_svc_poll(void)
{
	uint32_t now;
	uint8_t i;

	if (s_ctl.master == NULL) {
		return;
	}
	now = now_ms(&s_ctl);
	for (i = 0u; i < s_ctl.axis_count; i++) {
		drive_service(&s_ctl.axes[i].drv, now);
	}
}
