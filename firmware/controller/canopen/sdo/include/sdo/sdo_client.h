/*
 * sdo_client.h — 单事务异步 SDO 客户端接口
 *
 * 读写只负责发起，协议推进由 CANopen 工作循环的 step 完成。
 * ACCEPTED 后必须 poll 到 DONE；BUSY / INVALID 不产生结果。
 * 取消也异步收尾，普通取消状态不是调用方的业务操作失败。
 *
 * Author: zager
 */

#ifndef SDO_SDO_CLIENT_H
#define SDO_SDO_CLIENT_H

#include <sdo/sdo_types.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct sdo_client;

/* 启动一笔事务：ACCEPTED 后必须 poll 到 DONE；BUSY / INVALID 不生成结果 */
enum sdo_start_result sdo_client_start(struct sdo_client *s, const struct sdo_request *req);
enum sdo_poll sdo_client_poll(struct sdo_client *s, struct sdo_result *result);
void sdo_client_abort(struct sdo_client *s);
void sdo_client_step(struct sdo_client *s, uint32_t dt_us, uint32_t *timer_next_us);

#ifdef __cplusplus
}
#endif

#endif /* SDO_SDO_CLIENT_H */
