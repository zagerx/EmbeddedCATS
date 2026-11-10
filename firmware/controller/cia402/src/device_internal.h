/* device.c、device_seq.c 与 device_pp.c 之间的私有接口。 */

#ifndef CIA402_DEVICE_INTERNAL_H
#define CIA402_DEVICE_INTERNAL_H

#include <cia402/device.h>
#include <cia402/pds.h>

#define CIA402_DEVICE_SDO_TIMEOUT_MS 200u
#define CIA402_DEVICE_MON_TIMEOUT_MS 100u
#define CIA402_DEVICE_MON_INTERVAL_MS 50u
#define CIA402_DEVICE_STEP_TIMEOUT_MS 300u
#define CIA402_DEVICE_HALT_TIMEOUT_MS 3000u
#define CIA402_DEVICE_QUICK_STOP_TIMEOUT_MS 5000u
#define CIA402_DEVICE_PP_ARRIVE_TIMEOUT_MS 5000u

int cia402_device_od_write(struct cia402_device *d, uint16_t index, uint8_t sub, uint32_t value,
			   uint8_t size);
int cia402_device_od_read(struct cia402_device *d, uint16_t index, uint8_t sub, uint32_t *value,
			  uint8_t *len);
int cia402_device_write_controlword(struct cia402_device *d, uint16_t value);
int cia402_device_read_statusword(struct cia402_device *d, uint16_t *out);
uint32_t cia402_device_now_ms(struct cia402_device *d);
int cia402_device_cancelled(struct cia402_device *d);

/* 执行控制字序列：故障复位、起点快进、逐步等待，支持取消和超时。 */
int cia402_device_seq_run(struct cia402_device *d, const struct cia402_seq_step *seq, uint8_t count,
			  uint32_t step_timeout_ms);

int cia402_device_do_config(struct cia402_device *d);
int cia402_device_do_enable(struct cia402_device *d);
int cia402_device_do_disable(struct cia402_device *d);
int cia402_device_do_halt(struct cia402_device *d);
int cia402_device_do_quick_stop(struct cia402_device *d);

/*
 * PP 分两段：begin 下发参数并触发目标后立即返回，step 每拍读一次状态字判定到位。
 * 这样服务线程不会为一个轴的到位而卡住另一个轴。
 */
int cia402_device_pp_begin(struct cia402_device *d, int32_t target, int32_t vel, int32_t acc,
			   int32_t dec, uint32_t now_ms);
/* 1 = 还在走；0 = 到位；-1 = 失败、取消或超时 */
int cia402_device_pp_step(struct cia402_device *d, uint32_t now_ms);

#endif /* CIA402_DEVICE_INTERNAL_H */
