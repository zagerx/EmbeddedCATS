/*
 * control.h — 控制层生命周期、事件推进、命令与只读信息
 *
 * CANopen 工作线程调用 init 和 event_poll，退出并 join 后调用 deinit。
 * event_poll 阻塞等待 CAN、定时器或外部命令；业务 poll 本身不阻塞。
 * 查询与指令接口可由 UI 调用，命令入队后通过 eventfd 唤醒工作线程。
 *
 * Author: zager
 */

#ifndef CONTROL_CONTROL_H
#define CONTROL_CONTROL_H

#include <control/state_types.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct controller_move_req {
	uint8_t axis;
	float mm;
	float mm_s;
	float mm_s2;
};

/* 单轴反馈：位置 mm，速度 mm/s，状态字与错误码为驱动器原始值 */
struct controller_feedback {
	float position;
	float velocity;
	uint16_t statusword;
	uint16_t error_code;
};

/* 上报给 Qt 等外部的信息；一次加锁复制，保证属于同一时刻 */
struct controller_info {
	enum controller_state state;
	struct controller_feedback feedback[CONTROLLER_AXIS_COUNT];
	struct controller_fault fault;
	uint32_t abort[CONTROLLER_AXIS_COUNT]; /* 每轴最新 abort，实时诊断用 */
};

/* 在接口名指定的 SocketCAN 通道上登记节点 1、2 并装配状态机 */
enum controller_result controller_init(const char *can_ifname);

/* 释放协议栈与 CAN 资源；只能在工作线程退出后调用，允许重复调用 */
enum controller_result controller_deinit(void);

/* 等待并处理一轮 CANopen、轴与整机事件 */
enum controller_result controller_event_poll(void);

/* 唤醒等待中的事件循环；用于外部命令和宿主退出 */
void controller_wakeup(void);

/* 读取完整控制器信息；未初始化时返回 STATE */
enum controller_result controller_info_get(struct controller_info *out);

/*
 * 请求各轴使能。只有 IDLE（已配置、未使能）接受，其余状态直接丢弃。
 * 返回 OK 仅表示入队；BUSY 表示队列满，STATE 表示控制层未初始化。
 */
enum controller_result controller_enable(void);

/*
 * 同时下发 1..CONTROLLER_AXIS_COUNT 条绝对位置指令（mm、mm/s、mm/s²）：
 * 这批轴一起启动，全部到位才回 HOLDING。
 * 返回 OK 仅表示入队；BUSY 表示队列满，PARAM 表示轴号、条数或参数无效。
 * 只有 HOLDING 接受位置指令，MOVING/HALTING 中的新位置指令被丢弃。
 */
enum controller_result controller_move(const struct controller_move_req *reqs, uint8_t count);

/* 暂停在当前位置并保持使能；事件插到队头。 */
void controller_halt(void);

/*
 * 请求全部轴回原点（原点开关法）。只有 HOLDING 接受，其余状态直接丢弃。
 * 回零期间是原子的：暂停与位置指令一律丢弃，只响应急停与故障。
 * 方法号与三档速度是机械常量（见 cats_mech.h），调用方不传参数。
 */
enum controller_result controller_home(void);

/* 同上，只回一个轴；调试单轴时用，轴号越界返回 PARAM。 */
enum controller_result controller_home_axis(uint8_t axis);

/* 请求所有轴 quick stop 并断使能；事件插到队头，ESTOP 只能重启退出。 */
void controller_estop(void);

/*
 * 解除急停：驱动器断使能（6040 = 0x00），控制器回 IDLE；已锁存的故障不动，
 * 若本来就有故障，下一个节拍由 ROOT 迁到 FAULT。
 * 只有 ESTOP 状态接受，其余状态丢弃。
 */
void controller_clear_estop(void);

#ifdef __cplusplus
}
#endif

#endif /* CONTROL_CONTROL_H */
