# bindings/ — 界面与控制的唯一接缝

这一层把控制快照变成界面显示，把按钮动作变成控制命令，并把状态变化送到日志面板。

## 约定

1. 只有 `bindings/` 允许 `#include <control/control.h>`；`shell/`、`ui/`、`theme/`
   不 include 业务头。
2. 界面控件不对外暴露裸指针，绑定层用 `objectName` 查找控件。
3. `AppBinder` 只传递 CAN 接口名，不打开或保存 fd；主站资源由控制层管理。
4. `CanThread` 只创建一个 CANopen 工作线程，退出顺序固定为：停止标志 →
   eventfd 唤醒 → join → `controller_deinit()`。

| 界面动作 / 显示项 | 接哪一层 |
|---|---|
| CANopen 事件循环 | `controller_event_poll()` |
| 顶部状态条 | `controller_state()` / `controller_state_name()` |
| 故障原因 | `controller_fault_code()` / `controller_fault_name()` |
| 轴反馈与 SDO abort | `controller_axis_sample()` / `controller_axis_abort()` |
| 运动、停止、急停按钮 | `controller_move()` / `controller_stop()` / `controller_estop()` |

命令入队后立即通过 eventfd 唤醒工作线程，不必等待下一次基础定时器。UI 只读发布
快照，不访问 CANopenNode、SDO 或 CiA402 内部对象。
