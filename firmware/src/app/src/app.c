/*
 * app.c — 应用层骨架（**还没有真正收发**）
 *
 * 现在只做三件事：记住总线指针、维护状态、把非法调用挡回去。
 * 下一步往 cats_app_poll() 里填"每拍发什么、收什么"，其余函数基本不用改。
 */

#include <app/app.h>

#include <stddef.h>

static struct {
  can_io_t *bus; /* 只借用，不拥有；由壳负责 open/close */
  cats_app_state_t state;
} s_app = {NULL, CATS_APP_IDLE};

cats_app_result_t cats_app_start(can_io_t *bus) {
  if (bus == NULL) {
    return CATS_APP_ERR_PARAM;
  }
  if (s_app.state == CATS_APP_RUNNING) {
    return CATS_APP_ERR_STATE;
  }
  s_app.bus = bus;
  s_app.state = CATS_APP_RUNNING;
  return CATS_APP_OK;
}

cats_app_result_t cats_app_poll(uint32_t timeout_ms) {
  if (s_app.state != CATS_APP_RUNNING) {
    return CATS_APP_ERR_STATE;
  }
  (void)timeout_ms;
  /* TODO(下一拍)：NMT 启动节点 → SDO 探测 0x1000 → 状态上抛 */
  return CATS_APP_OK;
}

void cats_app_stop(void) {
  s_app.bus = NULL; /* 不调用 can_io_close：通道是壳的 */
  s_app.state = CATS_APP_IDLE;
}

cats_app_state_t cats_app_state(void) { return s_app.state; }
