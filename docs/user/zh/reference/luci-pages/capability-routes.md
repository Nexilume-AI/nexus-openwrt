---
sidebar_position: 6
title: Capability Routes
---

# Capability Routes

该页显示从 ARIB 选择并安装到 AFIB 的路由，最多读取 200 条。

| 普通列 | 含义 |
| --- | --- |
| Intent | 能力名称和版本 |
| Origin / next hop | 原始 Agent 与本地、Peer 或 Relay 下一跳 |
| State | 健康状态和 hop 数 |
| Details | route ID、来源、完整 path、endpoint、剩余租期和原始 JSON |

打开 **Advanced columns** 后还会显示 Route ID/source、tenant/endpoint，以及 latency、cost、trust、load。可按文字、来源和健康状态筛选；高级列开关保存在浏览器本地存储，不修改 Router 配置。

路由存在不保证调用必然成功：还要验证入口认证、Agent 回调、协议映射和超时。多条候选最终只有策略和评分选中的项进入 AFIB。
