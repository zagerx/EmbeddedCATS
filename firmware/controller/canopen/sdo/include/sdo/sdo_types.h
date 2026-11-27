/*
 * sdo_types.h — 异步对象访问的公共事务类型
 *
 * 受理与结果分开：sdo_client_start() 返回 true 表示事务已受理，随后必须取走一笔结果。
 * 返回 false 未受理，不改变当前事务与结果槽，也不生成虚假结果。
 *
 * Author: zager
 */

#ifndef SDO_SDO_TYPES_H
#define SDO_SDO_TYPES_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum sdo_dir {
	SDO_DIR_READ = 0,
	SDO_DIR_WRITE
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

enum sdo_client_state {
	SDO_CLIENT_IDLE = 0, /* 没有事务，可以受理新请求 */
	SDO_CLIENT_RUNNING,  /* 已受理，结果未到 */
	SDO_CLIENT_DONE      /* 结果已到，等待取走 */
};

/*
 * 一次对象访问请求。
 * dir == SDO_DIR_READ 时 size / value 被忽略，读取固定 4 字节。
 */
struct sdo_request {
	enum sdo_dir dir;
	uint16_t index;
	uint8_t subindex;
	uint8_t size;   /* 写：1～4 字节 */
	uint32_t value; /* 写：待写值 */
	uint32_t timeout_ms;
};

#ifdef __cplusplus
}
#endif

#endif /* SDO_SDO_TYPES_H */
