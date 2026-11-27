/*
 * sdo_client.h — 单事务异步 SDO 客户端接口
 *
 * 读写只负责发起，协议推进由 CANopen 工作循环的 step 完成。
 * start 返回 true 后必须等到 DONE 并 take_result；返回 false 不产生结果。
 * 取消也异步收尾，普通取消状态不是调用方的业务操作失败。
 *
 * Author: zager
 */

#ifndef SDO_SDO_CLIENT_H
#define SDO_SDO_CLIENT_H

#include <sdo/sdo_types.h>

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct sdo_client;

/* true = 已受理，必须等到 DONE 并 take_result；false = 未受理，无结果，原因由本模块记日志 */
bool sdo_client_start(struct sdo_client *s, const struct sdo_request *req);
/* 纯查询，不改变通道 */
enum sdo_client_state sdo_client_get_state(struct sdo_client *s);
/* 仅在 DONE 时取走结果并释放通道；其他状态不做任何事 */
void sdo_client_take_result(struct sdo_client *s, struct sdo_result *result);
void sdo_client_abort(struct sdo_client *s);
void sdo_client_step(struct sdo_client *s, uint32_t dt_us, uint32_t *timer_next_us);

#ifdef __cplusplus
}
#endif

#endif /* SDO_SDO_CLIENT_H */
