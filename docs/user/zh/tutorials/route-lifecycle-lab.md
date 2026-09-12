---
sidebar_position: 2
title: 实验：观察完整路由生命周期
---

# 实验：观察完整路由生命周期

本实验发布一个 `demo.echo` Agent，观察注册、AFIB 出现、续租和撤销。你会把“代码启动了”与“路由器真的认为它可调用”对应起来。

## 你需要

- 已完成 OpenWrt [快速配置](../getting-started/quick-setup.md)；
- 一台能访问 Router 的 Python 3.9+ 主机；
- 已安装 SDK，并拥有注册/调用权限的 token；
- Router 上可使用 LuCI；SSH/ubus 为可选的开发者观察方式。

## 第 1 步：先看到空或现有状态

打开 LuCI 的 **本地 Agent** 与 **能力路由**。若使用 SSH，记录基线：

```sh
ubus call agent agents '{"limit":100}'
ubus call agent routes '{"limit":100}'
ubus call agent stats '{}'
```

结果中可能已有其他 Agent；记住当前 generation 和 `demo.echo` 是否存在。

## 第 2 步：启动 Agent，得到第一个可见结果

在 SDK 仓库目录设置环境并运行官方示例：

```bash
export NEXUS_ROUTER_URL=http://192.168.1.1:7443
export NEXUS_AGENT_TOKEN=replace-with-a-real-token
python examples/friendly_agent.py
```

PowerShell 使用：

```powershell
$env:NEXUS_ROUTER_URL = 'http://192.168.1.1:7443'
$env:NEXUS_AGENT_TOKEN = 'replace-with-a-real-token'
python examples/friendly_agent.py
```

现在 LuCI 应出现 origin `agent://demo/echo-server` 和 intent `demo.echo`。这已经是第一个结果：处理器、监听器、注册与 AFIB 全部连通。

## 第 3 步：解释而不是猜测

再次执行：

```sh
ubus call agent agents '{"limit":100}'
ubus call agent routes '{"limit":100}'
ubus call agent lookup '{"intent":"demo.echo","version":1,"tenant":"demo","source_agent":"agent://demo/caller-1"}'
```

对照观察 `origin`、`route_id`、lease、source、健康状态和 lookup 选择。`route_id` 是本次租约，不应当作永久 Agent ID。

## 第 4 步：发起真实调用

保持 Agent 运行，在另一终端完成[调用第一个 Agent](https://nexilume-ai.github.io/nexus-docs/sdk/quickstart/call-first-agent)。成功响应证明数据面也已通过，而不只是控制面存在路由。

## 第 5 步：观察撤销与兜底过期

在 Agent 终端按 Ctrl+C。高层 SDK 会正常 unregister。刷新 LuCI 或重复 `agents/routes`，对应路由应消失。

若要理解崩溃场景，可重新启动后强制终止 Python 进程。此时没有正常 unregister，路由会保留到 lease 到期后由 Router 清理。不要在生产主机上用强制终止做实验。

## 你学到了什么

你观察了两个平面：`agents/routes/lookup` 是控制面，真实 echo 响应是数据面；正常退出靠显式撤销，异常退出靠租约最终收敛。继续完成[策略选路实验](policy-routing-lab.md)，理解同一能力存在多个候选时如何选择。

## 常见问题

- Agent 启动但 LuCI 无路由：检查 token scope、Router URL、advertise address 和注册错误。
- 路由存在但调用 404：检查 tenant、source Agent、Policy RIB 与 lease 是否仍有效。
- Ctrl+C 后仍短暂可见：刷新 generation；若进程未正常处理信号，等待 lease 到期。
