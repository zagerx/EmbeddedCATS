# axis/ — 轴层

把设备能力组织成"轴"：业务看到的最终形态。

## 结构

```text
axis/
├── include/axis/axis.h   公共头（轴命令、轴反馈、单位换算）
└── src/axis.c            轴集合、换算、批量动作与事实汇总
```

## 职责

- 轴标识与「轴 → 节点」映射
- 轴命令：目标值、使能意图、有效期（超时即失效）
- 轴反馈：值 + 有效性 + 时间戳
- 单位换算（指令单位 ↔ 工程单位，如 mm）

## 边界

- 只依赖 `drive` 的公共类型；
- 不做协议细节（不知道 SDO/PDO/控制字）；
- 不做调度与线程管理。

## 已实现

| 能力 | 位置 |
|---|---|
| 轴 = 节点 + 驱动对象 + 单位换算 | `include/axis/axis.h` |
| 轴集合：每轴一个驱动对象 ⇒ **并行**使能、互不阻塞 | `src/axis.c` |
| 单位换算：mm / mm·s⁻¹ / mm·s⁻² → pulse（全链路唯一取整点，double 中间量 + 四舍五入饱和） | `src/axis.c` |
| 运动/停止/急停转发与结论汇总 `axis_take_motion()` | `src/axis.c` |
| 批量动作：`axis_config_all()` / `axis_enable_all()` / `axis_quick_stop_all()` / `axis_poll_all()` | `src/axis.c` |
| 事实查询：`axis_all_configured()` / `axis_all_enabled()` / `axis_any_failed()` | `src/axis.c` |

未实现：换算参数目前是编译期常量（`config/cats_mech.h` 的临时值），
不从驱动器的 6092h 读；STOPPING 下的下使能。
