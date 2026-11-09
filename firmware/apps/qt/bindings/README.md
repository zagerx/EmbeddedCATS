# bindings/ — 界面与 app 的唯一接缝

这一层把「app 状态」变成「界面显示」、把「app 日志」送到日志面板。

**当前不绑定任何按钮**：启动由 `cats_control_init()` 完成，之后状态机自行推进。

## 约定

1. 只有 `bindings/` 允许 `#include <control/control.h>`（以及将来 `axis/` 的读接口）。
   `shell/`、`ui/` 一律不许 include 业务头。
2. 界面控件不对外暴露裸指针，绑定层用 `objectName` 取控件（`findChild<QPushButton *>("btn_estop")`），
   或由 `shell/` 提供 `bind(...)` 入口把控件指针交给绑定层。
3. 一个映射表，一处定义：

| 界面动作 / 显示项 | 接哪一层 |
|---|---|
| 控制节拍（`CanThread`，1 ms） | `cats_control_poll()` |
| 顶部状态条 | `cats_control_state()` / `cats_control_state_name()` |
| 故障原因 | `cats_control_fault_code()` / `cats_control_fault_name()` |
| 通道开/关 | `socketcan_open()` / `can_io_close()`（选后端只在集成层） |

已实现的：节拍、状态条、通道开与 app 初始化。
尚未实现的：轴卡片反馈、各按钮（当前一律不绑定）、日志面板内容（等 app 重新提供日志）。
