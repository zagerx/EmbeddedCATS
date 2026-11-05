/*
 * control.h — 控制层对外契约（整机控制主体 cats_control）
 *
 * 集成层（apps/qt、apps/lyos_node）在**业务上只依赖这一个头**：
 * 不许 include cia402 / axis 的头；通道与主站由壳按平台创建，
 * 所以 backends / canopen 的平台头只允许出现在壳的装配处，不得下传。
 *
 * 时序模型：**控制线程轮询（1 ms）+ 界面线程只读**。
 *   集成层起一个控制线程，按 1 ms 绝对节拍调 cats_control_poll()，由它驱动一次
 *   总线收发与状态推进；Qt 主线程只跑界面，不碰总线。
 *   好处：节拍不受重绘/对话框影响，而内核层依然不碰线程（纲领"内核平台无关"）。
 *
 * 线程约定（**必须遵守**）：
 *   写者：只有控制线程可以调 cats_control_init() / cats_control_poll()。
 *   读者：任意线程可调 cats_control_state() / cats_control_fault_code()。
 *   指令：cats_control_move_to() / cats_control_stop() / cats_control_estop() 任意线程可调，
 *         **封装成事件入队**，由控制线程在下一拍（≤ 1 ms）逐个消费。
 *         停止/急停插到队头，普通位置指令排在队尾。
 *
 * 跨线程只共享这几项（事件队列、状态、故障码、各轴实时样本），由**一把 pthread
 * mutex** 保护；临界区里只有几条赋值/结构体拷贝，不含 I/O、状态机动作或循环。
 * 实时样本由 cia402 的 50 ms 后台监控读得到（与状态机串行共用同一条 SDO 通道）。
 * 实际竞争接近零（界面 50 ms 读一次、控制线程 1 ms 写一次）。
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
 *   struct cats_control  应用层 = 整机控制主体（主站 + 轴 + 状态机 + 指令槽）
 *   enum cats_state      它的运行状态（PREPARING / ENABLING / READY / …）
 */

#ifndef CONTROL_CONTROL_H
#define CONTROL_CONTROL_H

#include <stdint.h>

#include <canopen/master.h>

#ifdef __cplusplus
extern "C" {
#endif

enum cats_result {
	CATS_OK = 0,
	CATS_ERR_STATE, /* 状态不允许：重复 start / 没 start 就 poll */
	CATS_ERR_PARAM, /* 参数不合法：通道名为空 / 轴号越界 */
	CATS_ERR_BUSY   /* 事件队列已满：这条指令没进队，请稍后重试 */
};

/*
 * 状态机模型：**QP 风格的层次状态机（HSM）**，内核在 hsm/sm_hsm.c。
 *   每个状态一个函数（states/state_*.c，一状态一文件），自己处理事件、自己决定迁移；
 *   不认识的返回 SM_SUPER(&父状态) 自动上抛；
 *   迁移由内核算 LCA：EXIT 到最近公共祖先，再从那边 ENTRY 下来；
 *   ENTRY/EXIT 也是事件，进入/离开状态的动作有固定位置。
 *   父状态用来收敕公共规则（st_online 判心跳，st_root 判故障码与急停）。
 *
 * 运行状态（控制层，与 NMT 状态、402 状态三者不混）：
 *   PREPARING 准备中：复位节点并等 boot-up
 *   ENABLING  使能中：NMT 启动 + 各轴进 PP 并上使能（各轴并行）
 *   HOLDING   保持中：已使能、没有运动在途，可接受指令（保持力矩）
 *   MOVING    运动中：一条 PP 运动在途（收到新目标则覆盖）
 *   HALTING   减速停在途：6040 bit8 = Halt，停在当前位置（保持使能）
 *   FAULT     故障：通信/协议/运动类错误，停住（终态）
 *   ESTOP     急停：quick stop → 断使能，只能重启程序（终态）
 *
 * 名字不用 READY / STOPPING：READY 在 CiA 402 里是 "Ready to switch on"，
 * STOPPING 容易被读成"停机/关机"，而这里做的是 Halt。
 */
enum cats_state {
	CATS_STATE_PREPARING = 0,
	CATS_STATE_CONFIGURING,
	CATS_STATE_ENABLING,
	CATS_STATE_HOLDING,
	CATS_STATE_MOVING,
	CATS_STATE_HALTING,
	CATS_STATE_FAULT,
	CATS_STATE_ESTOP,
	CATS_STATE_COUNT /* 状态个数，states[] 表的大小 */
};

/* 故障码：进 FAULT 时确定，之后不再变；界面与现场排查用它分辨原因 */
enum cats_fault {
	CATS_FAULT_NONE = 0,
	CATS_FAULT_BOOTUP_TIMEOUT, /* PREPARING 里静默期过后仍等不到心跳 */
	CATS_FAULT_HEARTBEAT_LOST, /* 运行期节点心跳超时 */
	CATS_FAULT_CONFIG_TIMEOUT, /* CONFIGURING 里超时仍未配完 */
	CATS_FAULT_CONFIG_FAILED,  /* CONFIGURING 里写参数被 abort 或读回值不符 */
	CATS_FAULT_ENABLE_TIMEOUT, /* ENABLING 里超过 ENABLE_TIMEOUT_MS 还没使能上 */
	CATS_FAULT_ENABLE_FAILED,  /* ENABLING 里驱动器回了 abort 或 402 进故障 */
	CATS_FAULT_MOTION_FAILED,  /* MOVING 里 abort / 超时 / 402 进故障 */
	CATS_FAULT_STOP_FAILED     /* STOPPING 里停不下来（超时或 abort） */
};

/* 初始化：绑定主站并建状态机。建完即处于 PREPARING，之后由 poll 自行推进 */
enum cats_result cats_control_init(struct co_master *master);

/*
 * 推进一次：收帧 + 推进状态机。控制线程按 1 ms 调用，节拍由壳掌握，
 * 所以不需要参数（内核侧全是非阻塞调用，本函数不会阻塞）。
 * **只能在一个线程里调用**（见文件头的线程约定）。
 */
enum cats_result cats_control_poll(void);

/* 供界面刷新状态显示 */
enum cats_state cats_control_state(void);

/*
 * 位置指令（绝对位置）。业务单位：位置 mm，速度 mm/s，加速度 mm/s²（float）。
 * 任意线程可调：封装成 CATS_SIG_MOVE 事件排到队尾，多条指令按顺序保留。
 * 返回值 CATS_OK = 已入队；CATS_ERR_BUSY = 队列满（本指令被丢弃）；
 *      CATS_ERR_PARAM = 轴号越界 / 换算未配 / 参数非有限值 / 未初始化。
 * 注意：能否执行由状态机决定（HOLDING 里生效；运动中收到的命令被丢弃），
 *       入队成功不代表已在动。
 */
enum cats_result cats_control_move_to(uint8_t axis, float mm, float mm_s, float mm_s2);

/*
 * 停止指令：减速停在当前位置并保持使能（可继续接指令）。任意线程可调。
 * 事件插到队头（优先于已排队的位量指令），永不失败。
 */
void cats_control_stop(void);

/* 状态名："PREPARING" / "RUNNING" / …；越界返回 "?"。界面与日志共用同一份，别各写一份 */
const char *cats_control_state_name(enum cats_state s);

/*
 * 急停：任意线程可调。事件插到队头（优先于已排队的所有命令），永不失败。
 * 进入后停住，不自动退出。
 * 由调用方（lyos 的 cmd topic、或外部 STO 检测）触发。
 * 行为：停 SYNC + 各轴 quick stop，然后切到 ESTOP。
 */
void cats_control_estop(void);

/* 最近一次故障的原因；不在 FAULT 时为 CATS_FAULT_NONE */
enum cats_fault cats_control_fault_code(void);

/* 故障名："BOOTUP_TIMEOUT" / "HEARTBEAT_LOST"…；越界返回 "?" */
const char *cats_control_fault_name(enum cats_fault f);

/* 该轴最近一次失败时驱动器回的 SDO abort；0 = 不是 abort 引起的（超时/参数错）。
 * 进 FAULT 后保留，方便现场排查；轴号越界返回 0。 */
uint32_t cats_control_axis_abort(uint8_t axis);

/*
 * 实时反馈样本（由 cia402 的 50 ms 后台监控读数换算而来）。
 * valid 的位：bit0 状态字 / bit1 位置 / bit2 速度；位为 0 表示“还没读到过”，
 * 界面应显示占位符而不是把 0 当成真实值。
 */
struct cats_axis_sample {
	float position_mm;
	float velocity_mm_s;
	uint16_t statusword;
	uint16_t error_code; /* 603F：驱动器错误码（控制器自己的故障不在此） */
	uint8_t valid;
};

#define CATS_SAMPLE_VALID_STATUSWORD 0x01u
#define CATS_SAMPLE_VALID_POSITION 0x02u
#define CATS_SAMPLE_VALID_VELOCITY 0x04u
#define CATS_SAMPLE_VALID_ERROR_CODE 0x08u

/* 取该轴最新样本：0 = 写入了 out；CATS_ERR_PARAM = 轴号越界 / out 为空。
 * 任意线程可调（与状态/故障码同一把锁，只拷几个字段）。 */
enum cats_result cats_control_axis_sample(uint8_t axis, struct cats_axis_sample *out);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CONTROL_CONTROL_H */
