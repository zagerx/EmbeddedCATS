/*
 * app.h — 应用层对外契约（框架级：只有声明，实现随功能逐步补齐）
 *
 * 集成层（apps/qt、apps/lyos_node）在**业务上只依赖这一个头**：
 * 不许 include cia402 / axis 的头；通道与主站由壳按平台创建，
 * 所以 backends / canopen 的平台头只允许出现在壳的装配处，不得下传。
 *
 * 时序模型：**控制线程轮询（1 ms）+ 界面线程只读**。
 *   集成层起一个控制线程，按 1 ms 绝对节拍调 cats_app_poll()，由它驱动一次
 *   总线收发与状态推进；Qt 主线程只跑界面，不碰总线。
 *   好处：节拍不受重绘/对话框影响，而内核层依然不碰线程（纲领"内核平台无关"）。
 *
 * 线程约定（**必须遵守**）：
 *   写者：只有控制线程可以调 cats_app_init() / cats_app_poll()。
 *   读者：任意线程可调 cats_app_state() / cats_app_fault_code()，读到的是
 *         控制线程每拍发布的原子值——不加锁、也不会阻塞写者。
 *   急停：cats_app_estop() 任意线程可调，只置请求，
 *         由控制线程在下一拍（≤ 1 ms）执行迁移。
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
 *   ENABLING  使能中：NMT 启动 + 各轴进 PP 并上使能（各轴并行）
 *   READY     就绪：已使能、没有运动在途，可接受指令
 *   MOVING    运动中：一条 PP 运动在途（收到新目标则覆盖）
 *   STOPPING  停止中：收到停止指令，减速停在当前位置（保持使能）
 *   FAULT     故障：通信/协议/运动类错误，停住
 *   ESTOP     急停：quick stop → 断使能，只能重启程序
 */
enum cats_run_state {
	CATS_RUN_PREPARING = 0,
	CATS_RUN_ENABLING,
	CATS_RUN_READY,
	CATS_RUN_MOVING,
	CATS_RUN_STOPPING,
	CATS_RUN_FAULT,
	CATS_RUN_ESTOP,
	CATS_RUN_STATE_COUNT /* 状态个数，states[] 表的大小 */
};

/* 故障码：进 FAULT 时确定，之后不再变；界面与现场排查用它分辨原因 */
enum cats_app_fault {
	CATS_FAULT_NONE = 0,
	CATS_FAULT_BOOTUP_TIMEOUT, /* PREPARING 里静默期过后仍等不到心跳 */
	CATS_FAULT_HEARTBEAT_LOST, /* 运行期节点心跳超时 */
	CATS_FAULT_ENABLE_TIMEOUT, /* ENABLING 里超过 ENABLE_TIMEOUT_MS 还没使能上 */
	CATS_FAULT_ENABLE_FAILED,  /* ENABLING 里驱动器回了 abort 或 402 进故障 */
	CATS_FAULT_MOTION_FAILED,  /* MOVING 里 abort / 超时 / 402 进故障 */
	CATS_FAULT_STOP_FAILED     /* STOPPING 里停不下来（超时或 abort） */
};

/* 初始化：绑定主站并建状态机。建完即处于 PREPARING，之后由 poll 自行推进 */
enum cats_app_result cats_app_init(struct co_master *master);

/*
 * 推进一次：收帧 + 推进状态机。控制线程按 1 ms 调用，节拍由壳掌握，
 * 所以不需要参数（内核侧全是非阻塞调用，本函数不会阻塞）。
 * **只能在一个线程里调用**（见文件头的线程约定）。
 */
enum cats_app_result cats_app_poll(void);

/* 供界面刷新状态显示 */
enum cats_run_state cats_app_state(void);

/*
 * 位置指令（绝对位置）。业务单位：位置 µm，速度 µm/s，加速度 µm/s²。
 * 任意线程可调，只记一条待执行指令（单槽，覆盖上一条），由控制线程执行。
 * 返回 0 = 已记下；-1 = 参数不合法 / 换算未配置。
 * 注意：能否执行由状态机决定（只在 READY / MOVING 里生效），返回 0 不代表已在动。
 */
int cats_app_move_to(uint8_t axis, int32_t um, int32_t um_s, int32_t um_s2);

/*
 * 停止指令：减速停在当前位置并保持使能（可继续接指令）。任意线程可调。
 */
void cats_app_stop(void);

/*
 * 装配层给换算参数：每转多少脉冲（驱动器 6092h）、每转多少微米（丝杠导程）。
 * 未配置时位置指令一律拒绝（宁可不动，也不算错位置）。
 * 0 = 成功；-1 = 轴号越界。
 */
int cats_app_set_axis_scale(uint8_t axis, int32_t pulses_per_rev, int32_t um_per_rev);

/* 状态名："PREPARING" / "RUNNING" / …；越界返回 "?"。界面与日志共用同一份，别各写一份 */
const char *cats_app_state_name(enum cats_run_state s);

/*
 * 急停：任意线程可调，只置请求，控制线程在下一拍（≤ 1 ms）执行迁移。
 * 进入后停住，不自动退出。
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
