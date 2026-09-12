---
sidebar_position: 2
title: 在电脑上体验完整 OpenWrt
description: 用独立 Hyper-V 虚拟机运行 OpenWrt、LuCI 和 Nexus Agent Router。
---

# 在电脑上体验完整 OpenWrt

不用先准备路由器硬件：电脑体验路径运行完整 OpenWrt 虚拟机，保留 LuCI、Agent Router 和首次配置向导。生产设备仍按[安装与构建](install.md)部署，支持范围见[支持矩阵](support-matrix.md)。

:::info 当前交付状态
启动脚本、镜像制作流程和离线校验测试已经提供。**预装桌面镜像尚未发布，完整虚拟机开机验收待完成**，因此暂时不是可直接下载的一键安装包。已有 Hyper-V 路由器测试不能代替新桌面镜像验收。
:::

## 两种部署路径

| 路径 | 适合用途 | 交付方式 |
| --- | --- | --- |
| OpenWrt 设备 | 持续运行、真实网络与设备集成 | 匹配设备的固件或 feed 软件包 |
| 电脑虚拟机 | 学习、演示、开发和单节点调用验证 | 干净预装 VHDX、校验清单和启动脚本 |

首个电脑启动器面向启用 Hyper-V 的 Windows x86-64，需 Hyper-V PowerShell 模块和首次创建所需的管理员权限。默认 1 GiB 内存、2 个 vCPU。Linux/macOS、其他虚拟化软件和无 Hyper-V 环境暂未提供一键启动器。

## 拿到体验包后的启动步骤

维护者按源码中的 `deploy/desktop/BUILD.md` 制作并验收干净镜像。体验包应包含 `desktop-image.json`、匹配的 VHDX、`start-nexus-openwrt.ps1` 和 README；只有源码脚本时不能直接启动。

核对发布来源和校验值后，在解压目录的管理员 PowerShell 中执行：

```powershell
.\start-nexus-openwrt.ps1 -Action Check
.\start-nexus-openwrt.ps1
```

等待系统启动，打开 [LuCI 管理页](http://192.168.246.1/)。先设置独立 root 密码，再进入 **状态 → Agent Routing → User mode**，按[创建信任域和第一个节点](quick-setup.md)初始化。设备身份、密钥和信任域在本次安装中创建，镜像不能预置共用凭据。

默认安装目录为 `%LOCALAPPDATA%\Nexus\OpenWrtDesktop`，镜像会复制到独立可写磁盘。首次启动可用 `-InstallationDirectory` 指定新目录，之后所有命令使用同一路径。

```powershell
.\start-nexus-openwrt.ps1 -Action Status
.\start-nexus-openwrt.ps1 -Action Stop
.\start-nexus-openwrt.ps1 -Action Start
```

Stop 请求正常关机并保留数据；不会自动强制断电。Check 只验证发布镜像和配置，不代表业务服务健康。

## 电脑与虚拟机的网络

启动器创建专用 Internal 交换机：电脑管理地址 `192.168.246.2/24`，虚拟机 `192.168.246.1/24`。默认不接 WAN、不桥接物理网卡，不向主机添加默认网关/DNS，也不由虚拟机分发 DHCP/RA。

在电脑运行 Python Agent 时，按 SDK 指南使用可由虚拟机访问的 `192.168.246.2` 监听地址；只监听 `127.0.0.1` 无法被虚拟机回调。需要主机防火墙规则时，仅放行选定 Agent 端口和虚拟机来源地址。

先完成本机注册和调用，再根据体验包 README 显式添加 NAT WAN 适配器，以测试 Cloud/Relay。先设置密码，再在 LuCI 按新网卡的 MAC 识别接口并配置 DHCP-client/WAN 防火墙区域。NAT 出站连接不代表公网 IPv6 入站或真实 LAN 发现已通过测试。

## 一键配置 IPv6

启动器同时配置专用网络的 IPv6：虚拟机 `fd6e:6578:7573:246::1/64`，电脑 `fd6e:6578:7573:246::2/64`。可访问 [IPv6 LuCI 管理页](http://[fd6e:6578:7573:246::1]/)。这些是本地 ULA 地址，不是公网地址。

体验包包含 `configure-ipv6.ps1`。先在 LuCI 设置 root 密码，保持虚拟机运行，再在管理员 PowerShell 中执行以下命令，重新应用本地 IPv6 配置：

```powershell
.\configure-ipv6.ps1
```

脚本通过 Windows OpenSSH 登录虚拟机，首次连接需核对主机指纹，并按提示输入 root 密码或使用已有 SSH 认证；不会保存密码。如果启动时指定了安装目录，此脚本也需传入相同的 `-InstallationDirectory`。

需要向上游申请 IPv6 地址时，选择已有的 Hyper-V 上游交换机：

```powershell
Get-VMSwitch
.\configure-ipv6.ps1 -Mode Upstream -WanSwitchName 'Default Switch'
```

将示例名称替换为实际提供上游网络的交换机。脚本添加独立 WAN 网卡并配置 DHCPv4、DHCPv6 和 WAN 防火墙区域。已有 WAN 时可省略 `-WanSwitchName`；无法唯一识别空闲网卡时，先在 LuCI 核对 MAC，再用 `-WanDevice eth1` 指定。管理 LAN 保持独立。

脚本输出 `wan6` 实际状态；地址或前缀为空表示上游尚未分配，不能视为公网 IPv6 配置成功。NAT 交换机不保证提供公网 IPv6。此命令不会向管理 LAN 广播公网前缀；公网 Agent 入站还需相应服务配置及外部连通性验证。

## 验证与排错

- LuCI 可登录，Agent Router 页面加载，首次设置完成。
- Python Agent 的能力出现在 Capability Routes，认证调用返回正确结果。
- 重启后配置保留，正常 Stop/Start 可用；电脑原有联网方式未改变。
- **缺少镜像**：等待已验收体验包，或由维护者使用 BUILD.md 制作；不要复制正在使用的路由器磁盘。
- **权限不足**：确认管理员 PowerShell 和 Hyper-V 管理权限。
- **网段/名称冲突**：启动器拒绝覆盖，不能通过删除现有路由来绕过；该固定配置一次只支持一个体验实例。
- **创建中断**：保留对象和状态用于诊断，未完成配置的 VM 不会自动继续启动。

该方案暂不承担生产吞吐量、公网 IPv6、硬件驱动或多机互联的验收结论。

## 开源发布范围

OpenWrt 应独立于 Cloud 社区版发布。公开源码只包含审查过的路由组件、构建脚本及对应文档，不能直接上传混合工作目录中的测试磁盘、密钥、运行日志或企业版文档。Nexus 自有源码遵循项目 LICENSE/NOTICE；镜像内 OpenWrt 和第三方软件保留各自许可证和对应源码要求。
