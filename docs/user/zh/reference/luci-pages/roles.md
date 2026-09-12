---
sidebar_position: 4
title: Router Roles
---

# Router Roles

每台 Router 都是 Node；Relay 和 Directory 是可选服务端角色。

| 字段 | 可选值/作用 | 使用条件 |
| --- | --- | --- |
| Hosted services | Node only；Node + Relay；Node + Directory；全部角色 | 普通 Router 选 Node only |
| Relay configuration file | `nexus-relayd` JSON，默认 `/etc/nexus-relayd/relay.json` | 仅承载 Relay 时修改 |
| Directory public hostname | 其他 Router 使用且被证书 SAN 覆盖的主机名 | 仅承载 Directory 时填写 |
| Directory public port | 外部可达端口，默认 `8443` | 与端口映射和防火墙一致 |
| Directory configuration file | 默认 `/etc/nexus-directoryd/directory.json` | 仅自维护 JSON 时修改 |

状态卡分别显示角色是否选中、组件是否安装、配置文件是否存在以及服务是否运行。选择角色不会自动安装缺失软件包，也不会自动创建生产 TLS 配置。保存后若仍为 **Ready to start**，查看系统日志。

这里管理的是本路由器托管的 Open Mesh 服务。连接 Cloud Relay 使用 [Nexus Cloud](../../guides/cloud-relay.md)，不是切换服务端角色。
