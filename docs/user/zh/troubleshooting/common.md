---
sidebar_position: 1
title: 常见问题
---

# 常见问题

## LuCI 中没有 Agent Router

确认 `luci-app-agent-router` 已安装；退出并重新登录 LuCI。仍未出现时，检查软件包是否与当前 OpenWrt 分支和架构匹配。

## 服务无法启动

```bash
logread | grep -E 'agentd|agent-gw|agent-adapter'
uci show agent
```

优先处理配置解析错误、端口占用、证书路径错误和缺失依赖。

## Agent 已启动但页面中不可见

确认 Agent 使用了正确的路由器 URL和认证信息；检查主机到路由器的网络与 DNS；查看 Agent 端异常。注册成功后仍无路由时，检查租约、能力名称和策略 RIB。

## 邻居可见但没有导入路由

可见性不代表已信任。确认对等信任状态、导入策略和对端的导出策略。如果刚撤销或重新建立信任，等待状态刷新后再检查。

## IPv6 可 ping 但不能调用

依次确认监听地址、TCP 端口、防火墙、TLS 主机名和认证。`ping` 成功只证明 ICMPv6 路径可用。
