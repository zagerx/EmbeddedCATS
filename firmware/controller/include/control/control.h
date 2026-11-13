/*
 * control.h — 控制层生命周期、事件推进、命令与只读快照
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

/* 在接口名指定的 SocketCAN 通道上登记节点 1、2 并装配状态机 */
enum controller_result controller_init(const char *can_ifname);

/* 释放协议栈与 CAN 资源；只能在工作线程退出后调用，允许重复调用 */
enum controller_result controller_deinit(void);

/* 等待并处理一轮 CANopen、轴与整机事件 */
enum controller_result controller_event_poll(void);

/* 唤醒等待中的事件循环；用于外部命令和宿主退出 */
void controller_wakeup(void);

enum controller_state controller_state(void);

/*
 * 请求各轴使能。只有 IDLE（已配置、未使能）接受，其余状态直接丢弃。
 * 返回 OK 仅表示入队；BUSY 表示队列满，STATE 表示控制层未初始化。
 */
enum controller_result controller_enable(void);

/*
 * 同时下发 1..CONTROLLER_MAX_AXES 条绝对位置指令（mm、mm/s、mm/s²）：
 * 这批轴一起启动，全部到位才回 HOLDING。
 * 返回 OK 仅表示入队；BUSY 表示队列满，PARAM 表示轴号、条数或参数无效。
 * 只有 HOLDING 接受位置指令，MOVING/HALTING 中的新位置指令被丢弃。
 */
enum controller_result controller_move(const struct controller_move_req *reqs, uint8_t count);

/* 减速停在当前位置并保持使能；事件插到队头。 */
void controller_stop(void);

const char *controller_state_name(enum controller_state s);

/* 请求所有轴 quick stop 并断使能；事件插到队头，ESTOP 只能重启退出。 */
void controller_estop(void);

enum controller_fault controller_fault_code(void);
const char *controller_fault_name(enum controller_fault f);

/* 最近一次失败的 SDO abort；0 表示没有记录到 abort。 */
uint32_t controller_axis_abort(uint8_t axis);

struct controller_axis_sample {
	float position_mm;
	float velocity_mm_s;
	uint16_t statusword;
	uint16_t error_code;
};

enum controller_result controller_axis_sample(uint8_t axis, struct controller_axis_sample *out);

#ifdef __cplusplus
}
#endif

#endif /* CONTROL_CONTROL_H */
