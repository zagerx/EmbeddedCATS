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
 * 所有权：CAN 通道由**壳**创建与关闭（shell 才知道自己跑在哪个平台），
 * app 只借用一个 can_io_t，不负责 open/close。所以移植时 app 一行不改。
 */

#ifndef APP_APP_H
#define APP_APP_H

#include <stdint.h>

#include <canopen/can_io.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
	CATS_APP_OK = 0,
	CATS_APP_ERR_BUS,   /* CAN 通道打不开，或收发过程中总线出错 */
	CATS_APP_ERR_STATE, /* 状态不允许：重复 start / 没 start 就 poll */
	CATS_APP_ERR_PARAM  /* 参数不合法：通道名为空 */
} cats_app_result_t;

typedef enum {
	CATS_APP_IDLE = 0, /* 未启动 */
	CATS_APP_RUNNING,  /* 已打开总线，周期推进中 */
	CATS_APP_FAULT     /* 出过错，需 stop 后重新 start */
} cats_app_state_t;

/* 启动：接管调用方给的总线，进入 RUNNING。已在运行则返回 ERR_STATE。
 * bus 的生命周期由调用方（壳）负责：cats_app_stop() 之后才能 can_io_close() */
cats_app_result_t cats_app_start(can_io_t *bus);

/* 推进一次：收发一帧、处理超时、更新状态。timeout_ms 建议 10~20（界面节拍） */
cats_app_result_t cats_app_poll(uint32_t timeout_ms);

/* 停止：不再推进，回到 IDLE。不关闭通道（通道是壳的）。未启动时调用无副作用 */
void cats_app_stop(void);

/* 供界面刷新按钮文字与状态显示 */
cats_app_state_t cats_app_state(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* APP_APP_H */
