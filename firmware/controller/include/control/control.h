/*
 * 控制层公共接口。CAN 通道由宿主创建，主站与控制对象由控制层持有。
 *
 * 调用约束：
 *   - controller_init/controller_poll：仅控制线程；
 *   - controller_io_poll：仅 I/O 线程，独占总线，约 1 ms 一拍；
 *   - controller_svc_poll：仅服务线程，非阻塞推进设备状态机；
 *   - 状态查询与指令接口：任意线程。
 *
 * 控制层不创建线程。跨线程指令通过事件队列传递，查询读取发布快照。
 */

#ifndef CONTROL_CONTROL_H
#define CONTROL_CONTROL_H

#include <control/state_types.h>

#include <stdint.h>

struct can_io;

#ifdef __cplusplus
extern "C" {
#endif

/* 一条位置指令，单位为 mm、mm/s、mm/s² */
struct controller_move_req {
	uint8_t axis;
	float mm;
	float mm_s;
	float mm_s2;
};

/*
 * 业务状态使用层次状态机，与 NMT、DS402 状态相互独立。
 * PREPARING → CONFIGURING → ENABLING → HOLDING ⇄ MOVING；
 * MOVING 可经 HALTING 回 HOLDING；FAULT/ESTOP 为终态。
 * 初始化时在控制层持有的主站对象上登记节点 1、2；bus 由宿主提供。
 */
enum controller_result controller_init(struct can_io *bus);

/* 推进一次业务状态机并发布快照；非阻塞。 */
enum controller_result controller_poll(void);

/* 收帧、推进各节点 SDO 通道并发帧；非阻塞。 */
void controller_io_poll(void);

/* 非阻塞推进配置、使能、运动和停止等设备状态机。 */
void controller_svc_poll(void);

enum controller_state controller_state(void);

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

/* 服务线程反馈读取的最新发布样本；未读到过的字段为 0。 */
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
