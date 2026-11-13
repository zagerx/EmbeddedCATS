/*
 * types.h — DS 402 的枚举与位定义（纯知识，零依赖）
 *
 * 这一层不认识总线、时间、线程、单位；只有标准里写着的东西。
 *
 * enum cia402_pds_state 是**从站（驱动器）PDS 状态**的投影：主站不维护自己的
 * 迁移，只看 6041 解析出来的事实。主机的业务状态在 states/。
 * 这些状态只描述从站，不与整机业务状态混用。
 */

#ifndef CIA402_TYPES_H
#define CIA402_TYPES_H

#ifdef __cplusplus
extern "C" {
#endif

/* 状态机所处状态（由状态字 6041 解析得到） */
enum cia402_pds_state {
	CIA402_NOT_READY = 0,      /* 未准备好（bit6 = 0） */
	CIA402_SWITCH_ON_DISABLED, /* 禁止启动 0x0040 */
	CIA402_READY,              /* 准备启动 0x0021 */
	CIA402_SWITCHED_ON,        /* 已启动   0x0023 */
	CIA402_OP_ENABLED,         /* 可操作   0x0027 */
	CIA402_FAULT,              /* 故障     (sw & 0x4F) == 0x08 / 0x0F */
	CIA402_STATE_UNKNOWN       /* 不匹配任何已知组合 */
};

/* 控制字 6040 的命令值（DS 402 表 13） */
#define CIA402_CW_DISABLE 0x0000u      /* 断电压 / 断使能 */
#define CIA402_CW_QUICK_STOP 0x0002u   /* 快速停止 */
#define CIA402_CW_SHUTDOWN 0x0006u     /* 电压输出 + 允许急停 */
#define CIA402_CW_SWITCH_ON 0x0007u    /* 再置 bit0 = 启动 */
#define CIA402_CW_ENABLE_OP 0x000Fu    /* 允许操作（使能完成） */
#define CIA402_CW_FAULT_RESET 0x0080u  /* bit7：故障复位（上升沿） */
#define CIA402_CW_NEW_SETPOINT 0x0010u /* bit4：PP 新目标 */
#define CIA402_CW_HALT 0x0100u         /* bit8：暂停（减速停） */

/* 状态字 6041 的位（只列本工程实际使用的） */
#define CIA402_SW_TARGET_REACHED 0x0400u /* bit10：PP 到位判据 */

/* 操作模式 6060 的值（只列本工程实际使用的） */
#define CIA402_MODE_PP 1u /* Profile Position */

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CIA402_TYPES_H */
