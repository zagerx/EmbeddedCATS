# qt/ — 集成层（本机形态：Qt6 桌面界面）

开发机上的**图形壳**：把 `app` 的能力接到界面上，方便没有 orin-agx、没有上位机系统时手动操作与观察。

- 语言：**C++ / Qt6 Widgets**（本机 Qt 6.4）
- 与 `lyos_node/` **同级、二选一**：两个壳都只依赖 `app/include/app/app.h`
- 是唯一在装配时选择具体 backend 的地方（当前 socketcan）
- **不含业务逻辑**：不 include `canopen` / `cia402` / `axis` / `backends` 的任何头

## 与 app 的分工

界面只做三件事：改按钮文字、画出 `app` 的状态、按 10–20 ms 定时调 `cats_app_poll()`。
"发什么帧、超时多少、状态机怎么走"全在 `app` 及以下，界面不知道，也不许知道。

## 第一版范围

一个窗口 + 一个「启动」按钮：

- 点「启动」→ `cats_app_start("can0")` → 按钮变「停止」，`QTimer` 开始周期性调 `cats_app_poll()`
- 点「停止」→ 停定时器 + `cats_app_stop()`
- 界面只显示启动结果（成功 / 失败原因）

## 文件

| 文件 | 内容 |
|---|---|
| `main.cpp` | 进程入口：`QApplication` + 窗口 + 事件循环 |
| `mainwindow.h/.cpp` | 一个「启动 / 停止」按钮 + 一行状态；`QTimer` 20 ms 调 `cats_app_poll()` |

## 构建与运行

```bash
bash ~/.yakut/setup_dual.sh                      # can0 / can1 都 up，标准 CAN 1M
cmake -S firmware -B firmware/build
cmake --build firmware/build -j
./firmware/build/cats_qt
```

> `app` 目前是骨架：`poll` 还空转，「启动」按钮只验证
> “选后端 → 打开 can0 → 交给 app → 能干净地停下来”这条路径。
