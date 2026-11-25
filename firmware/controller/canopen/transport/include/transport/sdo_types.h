/*
 * sdo_types.h — 异步对象访问的公共事务类型
 *
 * 受理与结果分开：只有 ACCEPTED 表示事务已经发起，随后必须取走一笔结果。
 * BUSY / INVALID 未受理，不改变当前事务与结果槽，也不生成虚假结果。
 *
 * Author: zager
 */

#ifndef TRANSPORT_SDO_TYPES_H
#define TRANSPORT_SDO_TYPES_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum sdo_dir {
	SDO_DIR_READ = 0,
	SDO_DIR_WRITE
};

enum sdo_start_result {
	SDO_START_ACCEPTED = 0, /* 已受理：之后必须 poll 到 DONE */
	SDO_START_BUSY,         /* 有事务在途或结果未取走：本次未受理 */
	SDO_START_INVALID       /* 参数无效或客户端未装配：本次未受理 */
};

enum sdo_status {
	SDO_STATUS_OK = 0,        /* 对端正常应答 */
	SDO_STATUS_FAIL = -1,     /* 已受理事务以对端 abort 或协议错误结束 */
	SDO_STATUS_TIMEOUT = -2,  /* 通道期限到，已按取消路径收尾 */
	SDO_STATUS_CANCELLED = -3 /* 上层取消，已按协议或本地期限收尾 */
};

struct sdo_result {
	int8_t status; /* enum sdo_status */
	uint32_t value;
	uint32_t abort_code;
};

enum sdo_poll {
	SDO_POLL_DONE = 0, /* 已取走结果，通道回到空闲 */
	SDO_POLL_WAITING,  /* 在途事务尚无结果，或结果尚未取走 */
	SDO_POLL_IDLE      /* 没有事务或可取结果 */
};

#ifdef __cplusplus
}
#endif

#endif /* TRANSPORT_SDO_TYPES_H */
