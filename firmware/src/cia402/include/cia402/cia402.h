/*
 * cia402.h — CiA 402 设备子协议（驱动器语义）
 *
 * 这一层是"402 知识"的唯一所在地：控制字怎么拼、状态字怎么解、使能分几步、
 * 每一步等哪个状态位。上层只拿事实（使能了没有 / 是否失败），
 * 拿不到 0x6040 / 0x6041 这些号码。
 *
 * 本层不知道：
 *   - 机械单位（脉冲当量换算归 axis）；
 *   - 应用状态机（PREPARING / RUNNING… 归 app）；
 *   - 心跳、NMT 运行状态（归 canopen / app）。
 *
 * SDO 通道是"一节点一条"：OD 的 1280h / 1281h 各绑一个站号，节点之间
 * 互不打断。本层只做"我这一步要发一个 SDO，等它回来"，不碰 COB-ID。
 *
 * 文件分工（一个文件一件事）：
 *   cia402.c          对外接口与编排：init / request_* / poll / 事实查询
 *   cia402_config.c   参数配置序列（上电一次：写参数并读回校验）
 *   cia402_enable.c   使能序列（表驱动：写什么、等哪个状态、下一步是谁）
 *   cia402_sdo.c      一次 SDO 事务（发起 / 每拍推进 / 取结果），用上游原语
 *   cia402_state.c    状态字解析 + 状态名（纯函数，不碰总线）
 *   cia402_internal.h 上面几块的层内接口（不对外发布）
 */

#ifndef CIA402_CIA402_H
#define CIA402_CIA402_H

#include <canopen/master.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* DS 402 的 6 个状态（状态字的关键位组合）；UNKNOWN = 不匹配任何已知组合 */
enum cia402_state {
	CIA402_NOT_READY = 0,      /* 未准备好（bit6=0） */
	CIA402_SWITCH_ON_DISABLED, /* 禁止启动（0x0040） */
	CIA402_READY,              /* 准备启动（0x0021） */
	CIA402_SWITCHED_ON,        /* 已启动（0x0023） */
	CIA402_OP_ENABLED,         /* 可操作（0x0027） */
	CIA402_FAULT,              /* 故障（bit3=1） */
	CIA402_STATE_UNKNOWN
};

/* 使能结论（本层对外只留三态；中间走到哪一步不记录） */
enum cia402_enable_state {
	CIA402_EN_NOT_ENABLED = 0, /* 未使能：没请求过 / 已断使能 */
	CIA402_EN_ENABLED,         /* 已到 Operation enabled */
	CIA402_EN_FAILED           /* 使能中途失败（abort 或 402 进故障） */
};

/* 参数配置结论：参数写进去且读回对得上才算 CONFIGURED */
enum cia402_config_state {
	CIA402_CFG_PENDING = 0, /* 还没配 / 正在配 */
	CIA402_CFG_CONFIGURED,  /* 全部参数已写入并校验通过 */
	CIA402_CFG_FAILED       /* 写失败 / 读回值不符 / abort */
};

/* 一次 SDO 事务的进度（层内细节）。DONE / FAILED 是"结果已出、还没被取走" */
enum cia402_sdo_state {
	CIA402_SDO_IDLE = 0, /* 没有在途事务 */
	CIA402_SDO_READING,  /* 读在途 */
	CIA402_SDO_WRITING,  /* 写在途 */
	CIA402_SDO_DONE,     /* 成功，结果待取 */
	CIA402_SDO_FAILED    /* 失败，abort 待取 */
};

/* 使能序列里"当前这条命令"走到哪个相位（层内细节，一条一相位） */
enum cia402_phase {
	CIA402_PHASE_WRITE = 0,  /* 该发写 */
	CIA402_PHASE_WAIT_WRITE, /* 等写完成 */
	CIA402_PHASE_READ,       /* 该发读 */
	CIA402_PHASE_WAIT_READ   /* 等读完成并判状态字 */
};

/* 本层当前在推哪个协议过程。同一时刻只有一个（每条 SDO 通道单线使用） */
enum cia402_activity {
	CIA402_ACT_IDLE = 0,    /* 没有过程在跑 */
	CIA402_ACT_CONFIGURING, /* 参数配置序列（上电一次） */
	CIA402_ACT_ENABLING,    /* 使能序列 */
	CIA402_ACT_MOVING,      /* PP 运动在途 */
	CIA402_ACT_HALTING,     /* 减速停在途（Halt） */
	CIA402_ACT_QUICK_STOP   /* 急停：0x02 → 等减速 → 0x00，一机到底 */
};

/* 上一个过程的结论；被 cia402_take_result() 取走后回 NONE */
enum cia402_result {
	CIA402_RESULT_NONE = 0,
	CIA402_RESULT_DONE,
	CIA402_RESULT_FAILED
};

/* PP 运动在途时的相位（层内）：先写一串参数，再触发 bit4，最后等到位 */
enum cia402_pp_phase {
	CIA402_PP_WRITE = 0,   /* 该写下一条参数 */
	CIA402_PP_WAIT_WRITE,  /* 等写完成 */
	CIA402_PP_WAIT_ARRIVE, /* 等 6041 bit10（目标到达） */
	CIA402_PP_CHECK_POS, /* 读 6064，确认“本来就在目标上”（距离 0 的情况） */
	CIA402_PP_WAIT_POS /* 等 6064 读完成 */
};

/* Halt 在途时的相位（层内）：置 bit8 → 等停稳 → 清 bit8 */
enum cia402_halt_phase {
	CIA402_HALT_SET = 0,   /* 该置 bit8 */
	CIA402_HALT_WAIT_SET,  /* 等置位写完 */
	CIA402_HALT_WAIT_STOP, /* 等停稳 */
	CIA402_HALT_CLEAR,     /* 该清 bit8 */
	CIA402_HALT_WAIT_CLEAR /* 等清位写完 */
};

/* 急停的相位（层内）：先 quick stop，再断使能 */
enum cia402_estop_phase {
	CIA402_ESTOP_QUICK = 0,   /* 该写 6040 = 0x02 */
	CIA402_ESTOP_WAIT_QUICK,  /* 等写完成 */
	CIA402_ESTOP_DECEL,       /* 该读状态字，等驱动器减速到 Switch on disabled */
	CIA402_ESTOP_WAIT_DECEL,  /* 等读完成 */
	CIA402_ESTOP_DISABLE,     /* 该写 6040 = 0x00 */
	CIA402_ESTOP_WAIT_DISABLE /* 等写完成 */
};

struct cia402_node {
	/* ---- 输入：装配时给定，之后只读 ---- */
	struct co_master *master; /* 只借用；SDO 客户端按站号从它取 */
	uint8_t node_id;

	/* ---- 对外事实：上层只该读这几项 ---- */
	enum cia402_config_state cfg_state; /* 配置结论 */
	enum cia402_enable_state en_state;  /* 使能结论 */
	enum cia402_state state;            /* 最近一次读到的状态字解析结果 */
	uint16_t statusword;                /* 最近一次读到的原始状态字 */
	uint32_t abort_code; /* 失败时驱动器回的 abort；0 = 不是 abort 引起的 */
	uint8_t failed; /* 最近一次过程失败过（下一个过程开始时清）；取不走 */

	/* 后台监控读数：50 ms 轮转读 6041 / 6064 / 606C，读不到就保留旧值。
	 * 这些值只给上层看，不参与状态机判断。 */
	struct {
		uint16_t statusword; /* 6041 原始状态字 */
		int32_t position;    /* 6064 实际位置（脉冲） */
		int32_t velocity;    /* 606C 实际速度（脉冲/秒） */
		uint16_t error_code; /* 603F 驱动器错误码（驱动器自己报的） */
		uint32_t last_ms;    /* 上次发起监控读的时刻 */
		uint32_t fail_count; /* 监控读失败次数（诊断用） */
		uint8_t slot;        /* 下一圈读哪个对象（见 monitor.c 轮转表） */
		uint8_t valid; /* bit0 状态字 / bit1 位置 / bit2 速度 / bit3 错误码 */
		uint8_t await; /* 在途事务是否由监控发起 */
	} monitor;

	/* ---- 层内进度：只有 cia402_*.c 读写，上层不要碰 ---- */
	enum cia402_activity activity;      /* 当前在推的协议过程 */
	enum cia402_result result;          /* 上一个过程的结论，待取 */
	uint16_t controlword;               /* 当前控制字（402 状态，写 6040 时用） */
	enum cia402_activity prev_activity; /* 上一拍的 activity，判"卡住"用 */
	uint32_t activity_since_ms;         /* 进入当前 activity 的时刻 */

	struct {
		uint8_t seq;             /* 配置表第几条（下标，恒 < 条数） */
		enum cia402_phase phase; /* 这条走到哪个相位 */
	} cfg;

	struct {
		uint8_t seq;             /* 使能序列第几条（下标，恒 < 条数） */
		enum cia402_phase phase; /* 这条命令走到哪个相位 */
	} en;

	struct { /* PP 运动：目标与速度参数（协议原生单位 = pulse） */
		enum cia402_pp_phase phase;
		uint8_t step;      /* 参数写表下标 */
		uint8_t seen_low;  /* 触发后读到过 bit10 == 0（防旧值误判到位） */
		uint32_t since_ms; /* 进入等待的时刻，用于超时 */
		int32_t target;
		int32_t vel;
		int32_t acc;
		int32_t dec;
	} pp;

	struct { /* 减速停（Halt） */
		enum cia402_halt_phase phase;
		uint32_t since_ms; /* 进入停车的时刻，用于超时 */
	} halt;

	struct { /* 急停：quick stop → 断使能 */
		enum cia402_estop_phase phase;
		uint32_t since_ms;
	} estop;

	enum cia402_sdo_state sdo; /* 当前 SDO 事务 */
	uint8_t sdo_owner;         /* 事务归属：0 = 流程，1 = 监控（见 monitor.c） */
	uint8_t data[4];           /* 最近一次 SDO 读到的原始数据（小端） */
	uint8_t data_len;

	/* ---- SDO 步进需要的时间差 ---- */
	uint32_t last_ms;
	uint8_t started;
};

/* 纯函数：状态字 → 402 状态。可单独测，不碰总线 */
enum cia402_state cia402_parse(uint16_t statusword);

/* ---- 后台监控读数（50 ms 轮转，与状态机共用同一条 SDO 通道） ----
 * valid 的位：bit0 状态字 / bit1 位置 / bit2 速度；对应位为 0 表示还没读到过。
 * 没读到时保留上一次的值，所以显示前先看 valid。 */
int32_t cia402_monitor_position(const struct cia402_node *n);
int32_t cia402_monitor_velocity(const struct cia402_node *n);
uint16_t cia402_monitor_statusword(const struct cia402_node *n);
uint16_t cia402_monitor_error_code(const struct cia402_node *n);
uint8_t cia402_monitor_valid(const struct cia402_node *n);

/* cia402_monitor_valid() 的位 */
enum {
	CIA402_MON_VALID_STATUSWORD = 0x01,
	CIA402_MON_VALID_POSITION = 0x02,
	CIA402_MON_VALID_VELOCITY = 0x04,
	CIA402_MON_VALID_ERROR_CODE = 0x08
};

void cia402_init(struct cia402_node *n, struct co_master *m, uint8_t node_id);

/* 请求配置参数（写模式等，上电一次）：0 = 已受理；1 = 已在配 / 有别的过程在跑 */
int cia402_request_config(struct cia402_node *n);

/* 请求上使能：0 = 已受理；1 = 已经要求过（不重复发） */
int cia402_request_enable(struct cia402_node *n);

/* 每拍推进一步；内部按需发起 SDO 读写。与"是否有请求"无关地调用 */
void cia402_poll(struct cia402_node *n, uint32_t now_ms);

/* 下发一条 PP 绝对位置运动（协议单位：pulse / pulse·s⁻¹ / pulse·s⁻²）
 * 0 = 已受理；1 = 已经有过程在跑（不覆盖）；-1 = 状态不允许 */
int cia402_pp_start(struct cia402_node *n, int32_t target, int32_t vel, int32_t acc, int32_t dec);

/* 减速停在当前位置（6040 bit8 = Halt），保持使能。0 = 已受理 */
int cia402_halt(struct cia402_node *n);

/* 急停：quick stop → 断使能（两段式，由 poll 推进）。0 = 已受理 */
int cia402_quick_stop(struct cia402_node *n);

/* 有没有协议过程在跑（使能 / 运动 / 停车 / 急停） */
int cia402_busy(const struct cia402_node *n);

/* 取走上一个过程的结论：DONE / FAILED / NONE；取走后回 NONE */
enum cia402_result cia402_take_result(struct cia402_node *n);

int cia402_is_configured(const struct cia402_node *n);
int cia402_is_enabled(const struct cia402_node *n);
int cia402_failed(const struct cia402_node *n);

/* 最近一次失败时驱动器回的 SDO abort；0 = 不是 abort 引起的（超时/参数错） */
uint32_t cia402_abort_code(const struct cia402_node *n);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CIA402_CIA402_H */
