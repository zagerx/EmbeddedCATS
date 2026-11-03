/*
 * app.h — 应用层对外契约（框架级：只有声明，实现随功能逐步补齐）
 *
 * 集成层（apps/qt、apps/lyos_node）**只允许** include 这一个头，
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
 *   PREPARING 准备中：复位节点并等 boot-up
 *   RUNNING   运行中：周期推进，可发运动指令
 *   STOPPING  停止中：下使能 → NMT pre-op（出口逻辑待补）
 *   FAULT     故障：通信/协议类错误，停住
 *   ESTOP     急停：安全态，只能由 cats_app_estop() 强制进入，停住
 */
enum cats_run_state {
	CATS_RUN_PREPARING = 0,
	CATS_RUN_RUNNING,
	CATS_RUN_STOPPING,
	CATS_RUN_FAULT,
	CATS_RUN_ESTOP,
	CATS_RUN_STATE_COUNT /* 状态个数，states[] 表的大小 */
};

/* 故障码：进 FAULT 时确定，之后不再变；界面与现场排查用它分辨原因 */
enum cats_app_fault {
	CATS_FAULT_NONE = 0,
	CATS_FAULT_BOOTUP_TIMEOUT, /* PREPARING 里静默期过后仍等不到心跳 */
	CATS_FAULT_HEARTBEAT_LOST, /* RUNNING / STOPPING 里节点心跳超时 */
};

/* 初始化：绑定主站并建状态机。建完即处于 PREPARING，之后由 poll 自行推进 */
enum cats_app_result cats_app_init(struct co_master *master);

/*
 * 推进一次：收帧 + 推进状态机。壳按 10~20 ms 周期性调用，节拍由壳掌握，
 * 所以不需要参数（内核侧全是非阻塞调用，本函数不会阻塞）。
 */
enum cats_app_result cats_app_poll(void);

/* 供界面刷新状态显示 */
enum cats_run_state cats_app_state(void);

/* 状态名："PREPARING" / "RUNNING" / …；越界返回 "?"。界面与日志共用同一份，别各写一份 */
const char *cats_app_state_name(enum cats_run_state s);

/*
 * 急停：任何状态下都立即生效（安全要求），进入后停住，不自动退出。
 * 由调用方（未来 lyos 的 cmd topic、或外部 STO 检测）触发。
 * 行为：停 SYNC + 各轴 quick stop，然后切到 ESTOP。
 */
void cats_app_estop(void);

/* 最近一次故障的原因；不在 FAULT 时为 CATS_FAULT_NONE */
enum cats_app_fault cats_app_fault_code(void);

/* 故障名："BOOTUP_TIMEOUT" / "HEARTBEAT_LOST"…；越界返回 "?" */
const char *cats_app_fault_name(enum cats_app_fault f);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* APP_APP_H */
