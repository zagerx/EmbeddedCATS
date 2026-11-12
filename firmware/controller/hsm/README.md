# hsm/ — 通用层次状态机内核

**零业务依赖**：不 include `axis/`、`cia402/`、`states/`，代码里不出现
`cats_` / `stm_` 这类业务前缀（R7 边界检查会验）。

QP 风格的层次状态机：状态是函数，`SM_SUPER(&父状态)` 自动上抛，迁移由内核
按最近公共祖先（LCA）计算 EXIT / ENTRY 链。

## 结构

```text
hsm/
├── include/hsm/sm_hsm.h   对外接口（sm_hsm_init / dispatch / state，SM_* 宏）
└── src/sm_hsm.c           内核实现
```

## 谁用

只有 `states/`（本机业务状态图）。但它本身不认识业务，任何需要
"层次状态机"的模块都能直接链 `cats_hsm`，不需要那份业务状态图。

## 本层不知道

- 有哪些状态、状态之间怎么走；
- "轴"、"使能"、"故障码"这些词；
- 时间、线程、I/O。
