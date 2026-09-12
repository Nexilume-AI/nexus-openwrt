---
sidebar_position: 3
title: 配置 IPv6 访问
---

# 配置 IPv6 访问

先区分用途：电脑与虚拟机管理、Agent 私网回调，以及可从互联网访问的 Agent 地址。ULA 不是公网地址，DHCPv6 已配置也不等于上游已分配公网前缀。

## 桌面虚拟机：一键配置

桌面启动器同时设置电脑 `fd6e:6578:7573:246::2/64` 和虚拟机 `fd6e:6578:7573:246::1/64`。访问 [IPv6 LuCI](http://[fd6e:6578:7573:246::1]/)。先设置 root 密码，再在管理员 PowerShell 中重新应用配置：

```powershell
.\configure-ipv6.ps1
```

需要上游 IPv6 时，明确选择已有的 Hyper-V 上游交换机：

```powershell
Get-VMSwitch
.\configure-ipv6.ps1 -Mode Upstream -WanSwitchName 'Default Switch'
```

将示例交换机替换为实际网络；NAT 交换机不保证公网 IPv6。脚本设置独立 WAN 的 DHCPv4/DHCPv6 并报告 `wan6` 状态，不向管理 LAN 广播公网前缀。地址/前缀为空时仍未取得上游分配。自定义安装路径需传相同 `-InstallationDirectory`。此脚本只用于受启动器管理的 Windows/Hyper-V 实例；不要直接用于生产路由器。镜像交付状态见[桌面指南](../getting-started/desktop-vm.md)。

## OpenWrt 托管 Agent 公网地址

进入 **状态 → Agent Routing → Developer mode → Advanced Settings → Public Agent IPv6**。当前源码默认 `public_ipv6_enabled=1`、`public_ipv6_mode=auto`，但缺少有效上游时不会凭空获得公网地址。

| 地址来源 | 条件 |
| --- | --- |
| Automatic | 优先检测 routed/PD 前缀，否则检查上游全局 on-link `/64` |
| routed-prefix | 上游将实际 `/48`–`/64` 路由给 OpenWrt |
| upstream-relay | 使用有效上游 on-link `/64` 和对应接口，通过 NDP 代理承载 `/128` |

这里的 **upstream-relay 是 IPv6/NDP 模式**，与 Cloud Relay、Open Mesh 的应用中继无关。

Agent 通过 `public_ipv6="auto"` 请求分配后，在 Local Agents 检查实际 `/128`、租约和连接描述符。公网调用还需已配置的网关监听、TLS/认证和对应防火墙规则；从外部网络验证实际调用，不能仅凭地址存在或 ping 成功判定。

## 私网 Agent 与主机地址

私网 IPv4/ULA 回调不要求公网 `/64`。Windows Host Alias 为主机拥有的前缀配置地址，是另一种部署路径，见 [Windows IPv6 指南](https://nexilume-ai.github.io/nexus-docs/sdk/guides/windows-ipv6)。不要把文档前缀 `2001:db8::/32` 当成真实公网分配，也不要对互联网开放 LuCI、ubus 或内部诊断入口。
