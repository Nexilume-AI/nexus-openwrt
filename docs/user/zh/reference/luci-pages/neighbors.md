---
sidebar_position: 7
title: Neighbors & Discovery
---

# Neighbors & Discovery

只读页面把“已建立 Peer”和“尚未准入的发现候选”分开展示。

| 区域 | 关键列 | 含义 |
| --- | --- | --- |
| ARPX neighbors | Router/peer、endpoint、transport/state、learned routes/sequence | 已配置或已提升的 Peer 会话与学习路由数 |
| LAN DNS-SD candidates | Router/domain、LAN 地址、接口、admission、lease | 本地多播发现的 Router 候选 |
| Cross-domain SVCB candidates | Router/domain、target:port、DNSSEC、admission、lease | DNSSEC 验证后的跨域候选 |

**Promoted** 表示候选已被准入为动态 Peer，**Eligible** 只表示符合自动准入条件，**Observed** 表示仅发现。发现本身不会创建 capability route；先到 [Peer Trust](peer-trust.md) 审核，再观察 ARPX session 和 learned routes。
