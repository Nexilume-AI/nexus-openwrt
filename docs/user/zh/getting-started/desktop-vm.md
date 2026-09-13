---
sidebar_position: 2
title: 在电脑上体验完整 OpenWrt
description: 用独立 Hyper-V 虚拟机运行 OpenWrt、LuCI 和 Nexus Agent Router。
---

# 在电脑上体验完整 OpenWrt

不用先准备路由器硬件：电脑体验路径运行完整 OpenWrt 虚拟机，保留 LuCI、Agent Router 和首次配置向导。生产设备仍按[安装与构建](install.md)部署，支持范围见[支持矩阵](support-matrix.md)。

## 两种部署路径

| 路径 | 适合用途 | 需要准备 |
| --- | --- | --- |
| OpenWrt 设备 | 持续运行、真实网络与设备集成 | 匹配设备的固件或 feed 软件包 |
| 电脑虚拟机 | 学习、演示、开发和单节点调用验证 | 干净预装 VHDX、校验清单和启动脚本 |

本指南使用 Windows x86-64 与 Hyper-V，虚拟机默认分配 1 GiB 内存、2 个 vCPU。电脑需启用硬件虚拟化、Hyper-V 及其 PowerShell 管理模块；创建和管理虚拟机时使用管理员 PowerShell。此启动器仅适用于 Hyper-V。

## 1. 准备环境和体验包

在管理员 PowerShell 中检查 Hyper-V 和 SSH 工具：

```powershell
Get-Command Get-VM, Get-VMSwitch, ssh.exe
Get-VMSwitch
```

缺少 Hyper-V 命令时，在 Windows“启用或关闭 Windows 功能”中启用 Hyper-V 和管理工具，按系统提示重启后重新检查。IPv6 配置脚本还需要 Windows OpenSSH 客户端。

准备包含以下文件的体验包，并解压到本地普通目录：

```text
桌面体验包/
  desktop-image.json
  <与清单匹配的镜像>.vhdx
  start-nexus-openwrt.ps1
  configure-ipv6.ps1
  README.md
```

`desktop-image.json` 记录镜像文件名、SHA-256 和网络配置。**源码 ZIP 不包含 VHDX**：只有源码时，先按仓库 `deploy/desktop/BUILD.md` 构建镜像，再用 `deploy/desktop/prepare-bundle.py` 生成体验包。不要把正在使用的路由器磁盘当作发布镜像，也不要仅将任意 VHDX 改名后套用清单。

为原始镜像及独立运行副本预留磁盘空间。使用前核对体验包来源和提供者公布的校验值；脚本内部的校验只能确认文件与清单一致。

## 2. 检查并启动

在解压目录打开管理员 PowerShell，先检查体验包，再启动：

```powershell
.\start-nexus-openwrt.ps1 -Action Check
.\start-nexus-openwrt.ps1
```

等待系统启动，打开 [LuCI 管理页](http://192.168.246.1/)。先设置独立 root 密码，再进入 **状态 → Agent Routing → User mode**，按[创建信任域和第一个节点](quick-setup.md)初始化。设备身份、密钥和信任域在本次安装中创建，镜像不能预置共用凭据。

首次启动会创建专用交换机和虚拟机，再从原始镜像复制独立运行磁盘。不要修改原始镜像。若等待后页面仍无法打开，在 Hyper-V 管理器中打开 `Nexus-OpenWrt-Desktop` 的控制台查看启动信息。

默认安装目录为 `%LOCALAPPDATA%\Nexus\OpenWrtDesktop`，镜像会复制到独立可写磁盘。首次启动可用 `-InstallationDirectory` 指定新目录，之后所有命令使用同一路径。

例如首次选择自己的安装目录和资源：

```powershell
.\start-nexus-openwrt.ps1 -InstallationDirectory 'D:\NexusDesktop' -MemoryMiB 2048 -Processors 2
```

该命令是首次启动的替代方式；目录应为新的专用目录。后续命令都加上相同的 `-InstallationDirectory 'D:\NexusDesktop'`，不要把已有安装误当成新的实例。下面示例使用默认目录。

r5 镜像在首次启动和每次重启时自动从 Hyper-V 校准 UTC，离线也可使用。请保持 Windows 宿主机时间准确；如果使用旧 r4 镜像，先在 LuCI 系统设置中同步时间，再配对 Cloud。

## 3. 查看状态、停止和再次启动

```powershell
.\start-nexus-openwrt.ps1 -Action Status
.\start-nexus-openwrt.ps1 -Action Stop
.\start-nexus-openwrt.ps1 -Action Start
```

Stop 请求正常关机并保留数据；不会自动强制断电。Check 只验证发布镜像和配置，不代表业务服务健康。

## 4. 连接电脑上的 Agent

启动器创建专用 Internal 交换机：电脑管理地址 `192.168.246.2/24`，虚拟机 `192.168.246.1/24`。默认不接 WAN、不桥接物理网卡，不向主机添加默认网关/DNS，也不由虚拟机分发 DHCP/RA。

在电脑运行 Python Agent 时，按 SDK 指南使用可由虚拟机访问的 `192.168.246.2` 监听地址；只监听 `127.0.0.1` 无法被虚拟机回调。需要主机防火墙规则时，仅放行选定 Agent 端口和虚拟机来源地址。

桌面镜像首次开机自动准备本机独立的认证校验公钥，启用 JWT 校验，并关闭公网入站。无需预置 Cloud 凭据即可使用本地 SDK 会话；Agent 使用 `http://192.168.246.1:7446` 和 `auth="auto"`。

在 LuCI User mode 启用 **Agent services**，然后按[发布与调用 Agent API](../guides/publish-api.md)在电脑启动 Agent 并发起调用。依次确认 Local Agents 中有租约、Capability Routes 中有能力，以及调用方收到实际响应。刚启动时空列表正常，不能只用 VM 的 Running 状态判断服务可用。

默认隔离网络不能访问互联网。需要 Cloud/Relay 或下载软件包时，先设置 root 密码，再按下一节选择提供上游网络的交换机；保留专用管理 LAN。

r5 体验包附带 SDK 0.46.2。在运行电脑上的 Python Agent 前，可在所选 Python 环境安装本地 wheel：

```powershell
python -m pip install .\nexus_agent_sdk-0.46.2-py3-none-any.whl
```

等待 Cloud 发布时可使用 `wait_for_cloud(timeout=300)`。节点恢复期间的 `unavailable` 是暂时未就绪，SDK 会继续等待；连接器通常每 120 秒同步一次。

## 5. 配置本地 IPv6 与上游网络

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

## 6. 连接检查与排错

- LuCI 可登录，Agent Router 页面加载，首次设置完成。
- Python Agent 的能力出现在 Capability Routes，认证调用返回正确结果。
- 重启后配置保留，正常 Stop/Start 可用；电脑原有联网方式未改变。
- **缺少清单或镜像**：核对本节的体验包文件列表。源码目录需要先完成镜像构建和打包；`Check` 不会下载或生成镜像。
- **权限不足**：确认管理员 PowerShell 和 Hyper-V 管理权限。
- **网段/名称冲突**：启动器拒绝覆盖，不能通过删除现有路由来绕过；该固定配置一次只支持一个体验实例。
- **创建中断**：保留对象和状态用于诊断，未完成配置的 VM 不会自动继续启动。

电脑上可进一步检查管理连接：

```powershell
Test-NetConnection 192.168.246.1 -Port 80
Test-NetConnection 192.168.246.1 -Port 22
Test-NetConnection 'fd6e:6578:7573:246::1' -Port 80
```

管理端口连通只证明可以访问虚拟机；Agent 调用仍需验证注册、认证和回调地址。上游没有分配 IPv6 时，本地 ULA 管理仍可使用。

## 7. 保留数据或移除虚拟机

`Stop` 会保留运行磁盘，之后 `Start` 继续使用原来的配置。需要迁移或重装时，先正常关机并备份安装目录中的运行磁盘和 `desktop-state.json`。

移除时，在 Hyper-V 管理器确认目标为 `Nexus-OpenWrt-Desktop`，移除该虚拟机及它专用的 Internal 交换机；确认不再需要数据后再删除对应安装目录。不要删除其他虚拟机、交换机或电脑现有路由。
