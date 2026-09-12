---
sidebar_position: 2
title: 发布与调用 Agent API
---

# 发布与调用 Agent API

Agent 把一个或多个能力注册到路由器。调用方通过路由器发现能力并发起调用，路由器再把请求转发到合适的 Agent。

## 发布流程

1. 在 **Agent API 与协议** 页面启用所需入口。
2. 为 Agent 准备认证信息。
3. 使用 SDK 声明能力、监听地址和健康状态。
4. 启动 Agent，等待注册完成。
5. 在 **本地 Agent** 中确认租约有效，在 **能力路由** 中确认路由已生成。

推荐从 [Python SDK 的第一个 Agent](https://nexilume-ai.github.io/nexus-docs/sdk/quickstart/first-agent)开始。MCP 工具可使用 [FastMCP 集成](https://nexilume-ai.github.io/nexus-docs/sdk/integrations/fastmcp)，A2A Agent 可使用 [A2A 集成](https://nexilume-ai.github.io/nexus-docs/sdk/integrations/a2a)。

## 调用与流式结果

普通调用返回一个 JSON 结果；长任务可以使用 SSE 流式返回进度和最终结果。调用方应设置超时、处理认证失败，并只在服务端声明支持续传时使用恢复标识。详见 [SDK 流式调用](https://nexilume-ai.github.io/nexus-docs/sdk/guides/streaming)。

## 下线

正常停止时让 SDK 撤销租约。异常退出后，路由器会在租约到期后移除路由。不要依赖无限期租约掩盖不可用的 Agent。

## User mode 与 LAN SDK

当前 User mode 的 **Agent services** 同时启用注册、调用、LAN SDK listener、LAN 回调和 adapter。LAN SDK listener 默认地址为 `0.0.0.0:7446`；原生软件包中的功能开关默认值不等于用户启用该功能后的状态。旧的 No JWT LAN listener `7445` 是另一独立入口，不能与 `7446` 混称。调用方应使用已启用入口及其认证流程；确认主机回调地址可从 Router 访问。
