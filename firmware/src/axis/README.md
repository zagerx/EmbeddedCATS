# axis/ — 轴层

把设备能力组织成"轴"：业务看到的最终形态。

## 结构

```text
axis/
├── include/axis/   公共头（轴命令、轴反馈、单位换算）
└── src/
```

## 职责

- 轴标识与「轴 → 节点」映射
- 轴命令：目标值、使能意图、有效期（超时即失效）
- 轴反馈：值 + 有效性 + 时间戳
- 单位换算（指令单位 ↔ 工程单位，如 mm）

## 边界

- 只依赖 `cia402` 的公共类型；
- 不做协议细节（不知道 SDO/PDO/控制字）；
- 不做调度与线程管理。

## 已实现

| 能力 | 位置 |
|---|---|
| 轴 = 节点 + 402 推进器 + 单位换算 | `include/axis/axis.h` |
| 轴集合：每轴一条 SDO 通道 ⇒ **并行**使能、互不阻塞 | `src/axis.c` |
| 单位换算：µm / µm·s⁻¹ / µm·s⁻² → pulse（64 位中间量，防溢出） | `src/axis.c` |
| 运动/停止/急停转发与结论汇总 `axis_take_motion()` | `src/axis.c` |
| 批量动作：`axis_config_all()` / `axis_enable_all()` / `axis_quick_stop_all()` / `axis_poll_all()` | `src/axis.c` |
| 事实查询：`axis_all_configured()` / `axis_all_enabled()` / `axis_any_failed()` | `src/axis.c` |

未实现：单位换算（`6092h` 脉冲当量）、目标位置与到位判定、STOPPING 下的下使能。
