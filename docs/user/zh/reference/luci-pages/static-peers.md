---
sidebar_position: 11
title: Static Peers
---

# Static Peers

静态清单只用于显式 ARPX Peer。LAN、Agent Card 和 Directory 管理的动态 Peer 不写入此表。

| 字段 | 作用/约束 |
| --- | --- |
| Enabled | 是否加载该条目 |
| Peer ID | 本 Router 使用的 Peer 标识，1–64 位受限字符 |
| Router ID | 对端稳定 Router 身份 |
| Domain | 对端管理域，hostname 格式 |
| ARPX endpoint | 对端 HTTPS ARPX URL，例如 `https://router.example:7444/arpx/v1` |
| Underlay IPv4 | 可选的固定 IPv4；留空使用 DNS |
| Role | `peer`、`reflector` 或 `relay` |
| Restart grace | 5–300 秒，默认 30 秒 |

固定 IPv4 只覆盖底层连接地址，HTTPS 仍应按 endpoint 主机名验证证书。不要把动态发现候选重复录入静态表，否则会增加身份和生命周期冲突。
