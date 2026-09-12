---
sidebar_position: 1
title: 配置私有云节点角色
---

# 配置 Agent 私有云节点角色

每台 Nexus Router 都是 Agent 私有云中的 Node。Relay 和 Directory 是可选服务端角色，不是 Edge、Hub 或 Leaf 模板。

| LuCI 模式 | 用途 | 额外软件包 |
| --- | --- | --- |
| Node only | 普通私有云节点，注册、发布和调用 Agent | 无 |
| Node + Relay | 为其他节点承载跨 NAT Agent 流量 | `nexus-agent-relayd` |
| Node + Directory | 向节点分配可信 Relay | `nexus-agent-directoryd` |
| Node + Relay + Directory | 小型自托管控制节点同时承担两种角色 | 两者都需要 |

## 配置步骤

1. 打开 **状态 → Agent Routing → Developer mode → Router Roles**。
2. 在 **Hosted services** 中选择模式。
3. 查看 **Role status**；Component、Configuration 和 Runtime 都应符合所选角色。
4. 只有维护自定义 JSON 时才展开 **Advanced role configuration** 修改文件路径。
5. Directory 模式填写证书覆盖的 **Public hostname** 和外部可达端口。
6. 点击 **Save & Apply**，确认所选角色显示 Ready。

## 默认文件与服务

| 角色 | 服务 | 默认配置 |
| --- | --- | --- |
| Relay | `/etc/init.d/nexus-relayd` | `/etc/nexus-relayd/relay.json` |
| Directory | `/etc/init.d/nexus-directoryd` | `/etc/nexus-directoryd/directory.json` |

Directory 页面会生成供其他节点填写的 assignment URL：`https://HOST:PORT/v1/open-mesh/assignment`。

## 验证

```sh
/etc/init.d/nexus-relayd running
/etc/init.d/nexus-directoryd running
logread | grep -E 'nexus-relayd|nexus-directoryd' | tail -n 80
```

只检查已选择的角色。组件缺失时安装 LuCI 提示的软件包；配置缺失或含占位值时，应配置有效证书、端点和服务身份；复制示例 JSON 本身不代表服务可用。

## 选择建议

大多数私有云节点使用 Node only。Relay 需要可被参与节点访问的网络入口；Directory 需要稳定域名、TLS 证书和明确的运维责任。只有团队确实承担两项服务时才选择全部角色。

Cloud 社区版自带的 Relay 在 Cloud 服务器运行，与此页自托管角色独立；见 [Cloud Relay](cloud-relay.md)。客户端复制 Directory 页给出的完整端点，不能将 Cloud 隧道地址作为 Open Mesh assignment URL。

## 已提供的 seed 初始化脚本

`nexus-agent-roles` 包提供 `/usr/sbin/nexus-open-mesh-seed-setup`，用于有意部署的自托管 seed。它要求 Node、OpenSSL、Relay/Directory 服务账号及 WAN 防火墙区域，会写入证书、Relay/Directory 配置并设置防火墙，不是普通 Node 的无副作用检测命令。默认本地 Relay/Directory 端口为 `17444` / `18443`，与 Cloud 社区版的 `27444` / `27445` 不同。已有自定义配置应先备份并审查脚本再运行；普通客户端只需复制 seed 提供的 assignment URL。
