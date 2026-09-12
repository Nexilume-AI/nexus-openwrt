---
sidebar_position: 8
title: Peer Trust
---

# Peer Trust

该页执行有状态、会影响路由的信任操作。

| 区域/字段 | 作用 |
| --- | --- |
| Pending trust | 展示 LAN 或 SVCB 候选、endpoint、证据和剩余租期 |
| Review & trust → Peer ID | 为动态 Peer 指定本地稳定 ID |
| Review & trust → Graceful restart | 5–300 秒；短暂中断时保留受控的 stale 路由窗口 |
| Trusted dynamic peers | 查看准入来源、租期并撤销 Peer |
| Authorized Agent Cards | 查看 issuer、key、能力、revision 并撤销 Card |
| Directory Card trust keys | 查看签名信任包中的 Router/key/status/SHA-256 |

批准时 `agentd` 会校验候选 generation；对话框打开后候选已变化，则操作失败且不修改 live peer table。撤销动态 Peer 会移除其学习能力；自动准入仍开启时它可能再次出现，所以还要调整 LAN admission 或 allowlist。没有 Authorized Card 的 DNSSEC 候选只有“地址真实性”证据，不等于业务授权。

OpenWrt 自托管 Open Mesh 与 Cloud Relay 使用独立连接状态和信任配置。Cloud（含社区版）的接入见[Cloud Relay](../../guides/cloud-relay.md)，自托管 seed 见[节点角色](../../guides/router-roles.md)。
