/*
 * sdo_client_fake.c — 提供控制层纯逻辑自测的空 SDO 客户端
 *
 * 被测控制器只调用 cia402_init(NULL)，不会发起事务。
 * 同名假实现满足直接函数链接，不引入 CANopenLinux 驱动与硬件依赖。
 *
 * Author: zager
 */

#include <sdo/sdo_client.h>

/**
 * sdo_client_start - 拒绝测试中的意外事务
 *
 * Return: INVALID；不生成事务结果。
 */
enum sdo_start_result sdo_client_start(struct sdo_client *s, const struct sdo_request *req)
{
	(void)s;
	(void)req;
	return SDO_START_INVALID;
}

/**
 * sdo_client_poll - 查询空测试通道
 *
 * Return: IDLE；结果输出保持不变。
 */
enum sdo_poll sdo_client_poll(struct sdo_client *s, struct sdo_result *result)
{
	(void)s;
	(void)result;
	return SDO_POLL_IDLE;
}

/**
 * sdo_client_abort - 忽略空测试通道的取消
 * @s: 未使用的客户端对象
 */
void sdo_client_abort(struct sdo_client *s)
{
	(void)s;
}
