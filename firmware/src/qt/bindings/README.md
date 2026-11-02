# bindings/ — 【预留·暂空】界面与 app 的唯一接缝

这一层现在**没有任何代码**。它是将来把「按钮点击」变成「`cats_app_*` 调用」、
把「app 状态 / 轴反馈」变成「界面显示」的唯一地方。

## 约定

1. 只有 `bindings/` 允许 `#include <app/app.h>`（以及将来 `axis/` 的读接口）。
   `shell/`、`ui/` 一律不许 include 业务头。
2. 界面控件不对外暴露裸指针，绑定层用 `objectName` 取控件（`findChild<QPushButton *>("btn_estop")`），
   或由 `shell/` 提供 `bind(...)` 入口把控件指针交给绑定层。
3. 一个映射表，一处定义：

| 界面动作 / 显示项 | 接哪一层 |
|---|---|
| `btn_connect` / `btn_disconnect` | 打开 / 关闭通道（选后端只在集成层） |
| `btn_start` / `btn_stop` | `cats_app_start()` / `cats_app_stop()` |
| `btn_enable` / `btn_disable` | CiA 402 使能 / 去使能 |
| `btn_reset` | 故障复位 |
| `btn_estop` | 急停（最高优先级） |
| `QTimer` 节拍 | `cats_app_poll()` |
| 轴卡片「反馈」四项 | app / axis 的状态读取接口 |
| 日志面板 | `cats_app_set_log()` 的出口 |

4. 本版（纯界面）不实现任何一条，按钮点了就是没反应。
