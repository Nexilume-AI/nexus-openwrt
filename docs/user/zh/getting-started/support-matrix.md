---
sidebar_position: 1
title: 支持矩阵
---

# 支持矩阵

本页区分“项目声明的构建基线”和“已经完成真实目标验收的环境”。没有测试证据的组合不会标成已支持。

## OpenWrt

| 项目 | 状态 | 说明 |
| --- | --- | --- |
| OpenWrt 25.12.x | 构建基线 | 软件包以该分支 API 和依赖为基线 |
| OpenWrt 25.12.4 x86/64 | 已验证 | SDK 构建和 Hyper-V 多路由运行验收通过 |
| OpenWrt 25.12.x ARM64/MIPS | 待验证 | 源码可交叉编译不等于设备运行已验收 |
| OpenWrt 24.10 及更早版本 | 未声明支持 | 依赖、LuCI API 和包管理器可能不同 |
| 第三方衍生固件 | 待厂商验证 | 需要核对内核、libubus、LuCI 与防火墙差异 |

OpenWrt 25.12 参考目标使用 `apk` 包；旧版固件常见的 `opkg` 不代表本项目已支持旧分支。

## 浏览器与管理入口

- LuCI 使用当前 OpenWrt 自带浏览器支持范围。
- 推荐使用仍在安全支持期内的 Chrome、Edge 或 Firefox。
- JavaScript 被禁用时，Agent Router 页面无法工作。

## Python SDK

| 项目 | 支持范围 |
| --- | --- |
| Python | 3.9 及以上，来自 `pyproject.toml` 契约 |
| 基础安装 | 无第三方运行时依赖 |
| FastMCP | `fastmcp>=3.0,<4` |
| A2A | `a2a-sdk>=1.1,<2` |
| Windows IPv6 | Windows + `pywin32>=306` |

## 部署前检查

在新设备或新架构上，先确认：

```sh
ubus call system board
df -h
free
apk --version
```

然后使用匹配的 OpenWrt SDK完成干净构建，安装后运行[一键诊断](../troubleshooting/diagnostics.md)。物理型号在被加入本表前，需要单独记录固件版本、架构、安装结果、服务状态和至少一次 Agent 调用。
