---
sidebar_position: 7
title: 可靠性、恢复与可观测性
---

# 可靠性、恢复与可观测性

Agent 网络的可靠性目标不是“永不失败”，而是让失败有边界、状态会收敛、操作者能解释发生了什么，并且不会为了恢复而重复执行有副作用的任务。

## 不同故障由不同机制收敛

| 故障 | 收敛机制 |
| --- | --- |
| Agent 正常退出 | SDK unregister，立即撤销 |
| Agent 崩溃/主机断电 | lease 到期 |
| 瞬时健康抖动 | 连续失败/恢复阈值 |
| Router peer 短断线 | stale + graceful timeout |
| Router 重启丢失租约 | SDK 续租 404 后健康重注册 |
| 流连接短断 | 固定 task/route + SSE 游标恢复 |
| IP underlay 不可达 | AFIB 重新编译或调用失败 |

这些机制不能混为一谈。比如重新注册可以恢复丢失的控制状态，却不应在本地处理器已经不健康时发生。

## 有界恢复

系统把路由、邻居、tenant quota、请求/响应、SSE 事件和恢复历史都限制在明确容量内。容量满时关闭式失败，而不是占满内存。恢复历史默认位于内存，只承诺短暂断线恢复，不承诺跨 Agent 或 Router 重启。

流恢复还有三个不变量：同一个稳定 task ID、相同请求指纹、第一次选择的 route ID。满足这些条件时只重放游标之后的事件，处理器不重新执行；不满足时返回冲突或历史过期，而不是猜测。

## 幂等与候补重试

幂等是“同一操作执行多次仍产生相同外部效果”。只有 Envelope 同时声明 `idempotent=true`、`allow_retry=true` 并带幂等键，策略又允许，系统才可能在后端尚未返回任何字节时尝试候补。支付、写入或设备控制等操作不能仅因为连接断开就默认重做。

## 观察而不读取业务内容

LuCI 与 ubus 可以展示：

- ARIB/AFIB 数量、generation 与排除原因；
- 邻居、Relay 隧道、租约和恢复计数；
- policy ID、健康 streak、调用状态和错误类别；
- route ID、task ID、延迟、负载与字节计数。

它们不应记录 Prompt、工具参数、模型输出、token 或完整业务 payload。诊断脚本也只采集 allowlist 状态并脱敏日志。

## 建议的排障顺序

1. 先确认 IP underlay：接口、DNS、时间和 endpoint 可达性。
2. 再确认控制面：服务、注册、lease、peer 与 AFIB generation。
3. 再查看策略解释：身份、policy、约束和被排除原因。
4. 最后检查数据面：认证、deadline、后端 TLS、响应上限和流游标。

这个顺序避免在路由根本不存在时反复调试业务处理器。实际命令见[诊断中心](../troubleshooting/diagnostics.md)，错误含义见[常见问题](../troubleshooting/common.md)。
