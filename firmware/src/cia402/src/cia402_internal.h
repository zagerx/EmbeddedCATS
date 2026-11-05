/*
 * cia402_internal.h — 本层内部的接口（不对外发布）
 *
 * 和 app 的 hsm 一个路子：**动作各自一个文件，编排单独一个文件**。
 *   cia402.c          编排 + 对外接口
 *   cia402_config.c   参数配置序列（上电一次：写参数并读回校验）
 *   cia402_enable.c   使能序列（写什么、等哪个状态、下一步）
 *   cia402_sdo.c      一次 SDO 事务（发起 / 每拍推进 / 取结果）
 *   cia402_state.c    状态字解析（纯函数）
 *
 * 这里只声明后两者给编排用的入口，不放任何数据定义（数据在 cia402.h）。
 */

#ifndef CIA402_INTERNAL_H
#define CIA402_INTERNAL_H

#include <cia402/cia402.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 把最近一次 SDO 读到的数据当状态字收下（原始值 + 解析结果） */
void cia402_take_statusword(struct cia402_node *n);

/* 一个过程结束：回到 IDLE、摆好结论；失败时钉住 failed 事实（不会被取走） */
void cia402_finish(struct cia402_node *n, enum cia402_result r);

/* 抢占：把在途的 SDO 事务丢掉（安全动作用，不能排在别的过程后面等） */
void cia402_abort_inflight(struct cia402_node *n);

/* ---- cia402_sdo.c：一次 SDO 事务 ---- */

/* 发起一次读/写：0 = 已发出（之后每拍调 cia402_sdo_step），1 = 通道忙（在途事务
 * 未结束，本拍不发、不破坏在途事务，调用方保持原相位），-1 = 发不出去 */
int cia402_sdo_read(struct cia402_node *n, uint16_t index, uint8_t sub);

/* 同上，但可指定等待上限（监控读用短的，避免拖住状态机） */
int cia402_sdo_read_to(struct cia402_node *n, uint16_t index, uint8_t sub, uint16_t timeout_ms);

int cia402_sdo_write(struct cia402_node *n, uint16_t index, uint8_t sub, uint32_t value,
		     uint8_t size);

/* 推进在途事务；结束时把结果摆进 n->sdo（DONE / FAILED）等调用方取走 */
void cia402_sdo_step(struct cia402_node *n, uint32_t diff_us);

/* 取结果：1 = 成功，-1 = 失败，0 = 还在途。取走后事务回 IDLE。
 * 只对“流程自己发起”的事务有效：监控读在途时一律返回 0。 */
int cia402_sdo_result(struct cia402_node *n);

/* SDO 事务归属：只有 PROCESS 发起的事务能被 cia402_sdo_result() 看见 */
#define CIA402_SDO_OWNER_PROCESS 0u
#define CIA402_SDO_OWNER_MONITOR 1u

/* ---- cia402_monitor.c：后台监控读数 ---- */

/* 每拍调用：有在途监控读就收结果，否则按 50 ms 节流轮转发起一次读 */
void cia402_monitor_poll(struct cia402_node *n, uint32_t now_ms);

/* ---- 402 的对象号与位（只在本层内部出现） ---- */
#define CIA402_IDX_CONTROLWORD 0x6040u
#define CIA402_IDX_STATUSWORD 0x6041u
#define CIA402_IDX_MODE 0x6060u        /* 操作模式（写） */
#define CIA402_IDX_MODE_ACTUAL 0x6061u /* 操作模式显示（读回校验用） */
#define CIA402_IDX_TARGET_POSITION 0x607Au
#define CIA402_IDX_POSITION_ACTUAL 0x6064u
#define CIA402_IDX_ACTUAL_VELOCITY 0x606Cu /* 实际速度（读，监控用） */
#define CIA402_IDX_PROFILE_VELOCITY 0x6081u
#define CIA402_IDX_PROFILE_ACCELERATION 0x6083u
#define CIA402_IDX_PROFILE_DECELERATION 0x6084u

/* 控制字位（DS 402 命令字） */
#define CIA402_CW_DISABLE 0x0000u      /* 断使能 */
#define CIA402_CW_QUICK_STOP 0x0002u   /* 快速停止 */
#define CIA402_CW_SHUTDOWN 0x0006u     /* 电压输出 + 允许急停 */
#define CIA402_CW_SWITCH_ON 0x0007u    /* 再置 bit0 = 启动 */
#define CIA402_CW_ENABLE_OP 0x000Fu    /* 允许操作（使能完成态） */
#define CIA402_CW_NEW_SETPOINT 0x0010u /* bit4：PP 新目标 */
#define CIA402_CW_HALT 0x0100u         /* bit8：暂停（减速停） */

/* 状态字位（DS 402；PP 模式） */
#define CIA402_SW_TARGET_REACHED 0x0400u /* bit10：已到位（到位判据用这个） */

/* 写控制字（6040）并记住当前值；0 = 已发出 */
int cia402_write_controlword(struct cia402_node *n, uint16_t value);

/* ---- cia402_config.c：参数配置序列 ---- */

/* 从第一条开始（只改本层游标，不发帧） */
void cia402_config_start(struct cia402_node *n);

/* 每拍推进一步：按当前相位发写 / 发读 / 比对读回值 */
void cia402_config_step(struct cia402_node *n);

/* ---- cia402_enable.c：使能序列 ---- */

/* 从第一条命令开始（只改本层游标，不发帧） */
void cia402_enable_start(struct cia402_node *n);

/* 每拍推进一步：按当前相位发写 / 发读 / 判状态字 */
void cia402_enable_step(struct cia402_node *n);

/* ---- cia402_pp.c：一条 PP 绝对位置运动 ---- */

/* 受理一条运动（只记参数，不发帧）；0 = 已受理 */
int cia402_pp_begin(struct cia402_node *n, int32_t target, int32_t vel, int32_t acc, int32_t dec);

/* 每拍推进：写参数 → 触发 bit4 → 等到位（bit10） */
void cia402_pp_step(struct cia402_node *n);

/* ---- cia402_stop.c：减速停 + 急停 ---- */

int cia402_halt_begin(struct cia402_node *n);
void cia402_halt_step(struct cia402_node *n);

int cia402_quick_stop_begin(struct cia402_node *n);
void cia402_quick_stop_step(struct cia402_node *n); /* 0x02 → 等减速到 disabled → 0x00 */

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CIA402_INTERNAL_H */
