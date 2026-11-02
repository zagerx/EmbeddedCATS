/*
 * app.h — 应用层对外契约（框架级：只有声明，实现随功能逐步补齐）
 *
 * 集成层（src/qt、src/lyos_node）**只允许** include 这一个头，
 * 不许 include canopen / cia402 / axis / backends 里的任何头。
 *
 * 时序模型：**单线程轮询**。
 *   GUI 用 QTimer、lyos 用主循环，按 10~20 ms 周期性调 cats_app_poll()，
 *   由它去驱动一次总线收发与状态推进。
 *   好处：CAN 的等待不会卡住界面，而内核层依然不碰线程（纲领"内核平台无关"）。
 *
 * 为什么是单例：一个进程只管一条 CAN 总线、一组轴。用静态实例既不动态分配内存，
 * 也不把内部结构（sdo 客户端、节点表…）暴露到墙外。
 *
 * 所有权：CAN 通道由**壳**创建与关闭（壳才知道自己跑在哪个平台）。
 * 主站 struct co_master 也由壳持有，app 只借用指针；app 不负责 open/close。
 * 因此移植到 orin-agx 时，app 一行不改。
 *
 * 命名（方案 A，**不用 typedef**）：
 *   struct co_master     内核层 = 协议意义上的主站（总线 + 节点 + NMT 状态）
 *   enum cats_run_state  应用层 = 整个程序的运行状态（IDLE / RUNNING / FAULT）
 */

#ifndef APP_APP_H
#define APP_APP_H

#include <stdint.h>

#include <canopen/master.h>

#ifdef __cplusplus
extern "C" {
#endif

enum cats_app_result {
	CATS_APP_OK = 0,
	CATS_APP_ERR_BUS,   /* CAN 通道打不开，或收发过程中总线出错 */
	CATS_APP_ERR_STATE, /* 状态不允许：重复 start / 没 start 就 poll */
	CATS_APP_ERR_PARAM  /* 参数不合法：通道名为空 */
};

/*
 * 状态机模型：**QP 风格——状态即函数指针**。
 *   每个状态一个函数（states_*.c，一状态一文件），自己处理事件、自己决定迁移；
 *   迁移由 hsm_dispatch() 统一完成"旧状态 EXIT → 换指针 → 新状态 ENTRY"；
 *   ENTRY/EXIT 也是事件，进入/离开状态的动作有固定位置。
 *
 * 运行状态（应用层，与 NMT 状态、402 状态三者不混）：
 *   IDLE      空闲，通道未动
 *   STARTING  启动中：等 boot-up → NMT → 配 PDO（界面可取消）
 *   RUNNING   运行中：周期推进，可发运动指令
 *   STOPPING  停止中：下使能 → NMT pre-op → 回 IDLE
 *   FAULT     故障：通信/协议类错误，可复位
 *   ESTOP     急停：安全态，只能人工解除，绝不自动重试
 */
enum cats_run_state {
	CATS_RUN_IDLE = 0,
	CATS_RUN_STARTING,
	CATS_RUN_RUNNING,
	CATS_RUN_STOPPING,
	CATS_RUN_FAULT,
	CATS_RUN_ESTOP,
	CATS_RUN_STATE_COUNT /* 状态个数，states[] 表的大小 */
};

/* 外部事件：界面/上位机的主动输入。条件类（超时等）不是事件 */
enum cats_event {
	CATS_EV_START, /* 点「启动」 */
	CATS_EV_STOP,  /* 点「停止」 */
	CATS_EV_RESET, /* 点「复位」 */
	CATS_EV_TICK   /* 每拍定时到（QTimer 驱动） */
};

/* 启动：等价于注入 CATS_EV_START。master 的生命周期由调用方（壳）负责 */
enum cats_app_result cats_app_start(struct co_master *master);

/* 推进一次：等价于注入 CATS_EV_TICK。timeout_ms 建议 10~20（界面节拍） */
enum cats_app_result cats_app_poll(uint32_t timeout_ms);

/* 停止：等价于注入 CATS_EV_STOP。不释放主站与通道（那是壳的） */
void cats_app_stop(void);

/* 供界面刷新按钮文字与状态显示 */
enum cats_run_state cats_app_state(void);

/* 状态名："IDLE" / "STARTING" / …；越界返回 "?"。界面与日志共用同一份，别各写一份 */
const char *cats_app_state_name(enum cats_run_state s);

/* 统一事件入口：所有状态变化都从这里进。tick 由 cats_app_poll 代发 */
enum cats_app_result cats_app_event(enum cats_event ev);

/*
 * 急停：**不走事件队列**，任何状态下都立即生效（安全要求）。
 * 只做接口预留，由调用方（未来 lyos 的 cmd topic、或外部 STO 检测）触发。
 * 行为：停 SYNC + 各轴 quick stop，然后切到 ESTOP；只能 cats_app_reset 人工解除。
 */
void cats_app_estop(void);

/* 复位：FAULT → STOPPING，ESTOP → IDLE（其余状态忽略） */
enum cats_app_result cats_app_reset(void);

/* 状态切换日志：迁移时打印 "IDLE -> STARTING"。由集成层注入，NULL 表示静默 */
void cats_app_set_log(void (*log_fn)(const char *line));

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* APP_APP_H */
