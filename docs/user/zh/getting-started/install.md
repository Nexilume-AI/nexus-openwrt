---
sidebar_position: 1
title: 编译 APK、安装与启动
---

# 从源码编译 APK、安装并启动

这条流程在 **Linux x86_64 电脑编译，OpenWrt 路由器安装运行**。以下可复制示例固定使用 OpenWrt **25.12.4、x86/64**；这是本项目的构建基线，不表示最新版本。准备专用的新 SDK，避免混入旧包。CMake 主机测试不会产出 OpenWrt APK。

## 1. 确认路由器版本与架构（路由器 SSH）

```sh
ubus call system board
cat /etc/openwrt_release
apk --print-arch
df -h /overlay /tmp
```

本例要求系统 release 为 `25.12.4`、target 为 `x86/64`、APK architecture 为 `x86_64`。版本、target/subtarget 或架构不同，须使用对应 SDK，不能安装本例产物；24.10 的 opkg 系统也不适用。先保留设备配置备份和现有 SSH 管理通道。依赖下载需要路由器能访问匹配版本的官方软件源。

## 2. 准备 Linux 编译环境（电脑）

推荐 Linux x86_64 或 WSL2 Ubuntu x86_64。WSL2 中源码与 SDK 放在 Linux 家目录，不放 `/mnt/c`、`/mnt/d` 或含空格路径。以下示例在同一终端中执行；只有安装系统依赖使用 sudo，编译使用普通用户。

```sh
sudo apt-get update
sudo apt-get install -y build-essential gawk gettext git libncurses-dev   rsync unzip zlib1g-dev file wget curl python3 python3-setuptools   swig libssl-dev xsltproc zstd openssl ca-certificates
mkdir -p "$HOME/nexus-work"
cd "$HOME/nexus-work"
```

将本仓库源码解压或检出到 `$HOME/nexus-work/nexus-openwrt`。该目录应直接包含 `feed/`、`scripts/` 和 `CMakeLists.txt`，不是上层混合产品目录。当前未发布 GitHub URL，不使用占位仓库执行 `git clone`。

下载官方 SDK 并核对固定 SHA-256，解压目标目录必须尚不存在：

```sh
set -eu
cd "$HOME/nexus-work"
SDK_ARCHIVE=openwrt-sdk-25.12.4-x86-64_gcc-14.3.0_musl.Linux-x86_64.tar.zst
curl --fail --location --proto '=https' --tlsv1.2   -o "$SDK_ARCHIVE" "https://downloads.openwrt.org/releases/25.12.4/targets/x86/64/$SDK_ARCHIVE"
printf '%s  %s\n'   28e004c1be4d215d19c1f12a6aa4c8d8f80689549eb707d0ff5a71f16fa8d05f   "$SDK_ARCHIVE" | sha256sum -c -
test ! -e "${SDK_ARCHIVE%.tar.zst}"
tar --zstd -xf "$SDK_ARCHIVE"
SDK="$HOME/nexus-work/${SDK_ARCHIVE%.tar.zst}"
REPO="$HOME/nexus-work/nexus-openwrt"
test -f "$REPO/feed/agentd/Makefile"
```

SDK 文件名和校验值来自[官方 25.12.4 x86/64 下载目录](https://downloads.openwrt.org/releases/25.12.4/targets/x86/64/)。重新打开终端后需重新设置 `SDK`、`REPO` 变量。

## 3. 编译基础路由器 APK（电脑）

```sh
cd "$REPO"
JOBS=2 sh scripts/build-openwrt-sdk.sh "$SDK"
find "$SDK/bin/packages" -type f -name '*.apk' -print
```

内存不足时改成 `JOBS=1`。脚本会接入本地 feed、更新依赖 feeds、**覆盖 SDK `.config`** 并编译。默认配置 `sdk/p8123-minimal.config` 固定 x86/64。首次构建需要下载依赖，不能把短时间无输出当作失败。

本地 Nexus 包目录为 `$SDK/bin/packages/x86_64/nexus_agent_router`。基础安装由 `luci-app-agent-router` 拉入 `agentd`、`agent-netd`、`agent-gw`、`agent-adapter`、`nexus-agent-roles` 和 `nexus-cloud-connector`；`agent-cardd` 单独编译、按需安装。以实际生成文件为准，不手写版本号。基础步骤不需要 Node.js。

**其他架构**：不要运行默认配置直接替换 SDK。应在对应 SDK 准备与其 target/subtarget 一致的配置，通过 `MINIMAL_CONFIG=/absolute/path/to/target.config JOBS=2 sh scripts/build-openwrt-sdk.sh "$SDK"` 指定；输出架构目录也要相应调整。该路径须另行完成目标设备测试。

## 4. 制作可验证的本地软件源（电脑）

SDK 的 APK 可能未单独签名。使用 SDK 的签名密钥生成 **packages.adb 签名索引**，然后按软件包名安装，由索引验证对应 APK。以下只复制本次新 SDK 的 Nexus feed，输出目录必须是新的：

```sh
set -eu
FEED="$SDK/bin/packages/x86_64/nexus_agent_router"
BUNDLE="$HOME/nexus-work/nexus-feed"
APK="$SDK/staging_dir/host/bin/apk"
test -s "$SDK/private-key.pem"
test -s "$SDK/public-key.pem"
set -- "$FEED"/agentd-*.apk
test -f "$1"
mkdir "$BUNDLE"
cp "$FEED"/*.apk "$BUNDLE/"
(
  cd "$BUNDLE"
  "$APK" mkndx --root "$SDK" --keys-dir "$SDK" --allow-untrusted     --sign "$SDK/private-key.pem" --output packages.adb ./*.apk
)
cp "$SDK/public-key.pem" "$BUNDLE/nexus-build-public.pem"
"$APK" --keys-dir "$BUNDLE" verify "$BUNDLE/packages.adb"
(
  cd "$BUNDLE"
  sha256sum ./*.apk packages.adb nexus-build-public.pem > SHA256SUMS
)
sha256sum "$BUNDLE/nexus-build-public.pem"
```

这里 `mkndx --allow-untrusted` 仅允许构建端读取自己刚编译的未签名 APK，**路由器安装不使用此选项**。记录最后输出的公钥文件 SHA-256，用于路由器端比对。`private-key.pem` 始终留在编译电脑，不复制到路由器或公开仓库。若 SDK 未生成密钥，先按 SDK 的签名配置完成密钥生成；不要跳过验证。

## 5. 复制、验证并安装（电脑 → 路由器）

电脑上，将示例地址替换成你的路由器管理地址；下面目标 `/root/nexus-feed` 必须尚不存在。

```sh
ROUTER=192.168.1.1
ssh "root@$ROUTER" 'test ! -e /root/nexus-feed'
scp -O -r "$BUNDLE" "root@$ROUTER:/root/"
ssh "root@$ROUTER"
```

`scp -O` 使用兼容 OpenWrt Dropbear 的 SCP 协议。进入路由器后执行：

```sh
set -eu
cd /root/nexus-feed
sha256sum -c SHA256SUMS
sha256sum nexus-build-public.pem
```

确认公钥文件摘要与电脑上记录的值一致，再安装公钥及软件包。SHA256SUMS 用来检查传输完整性，不能单独证明来源可信。

```sh
mkdir -p /etc/apk/keys
cp nexus-build-public.pem /etc/apk/keys/nexus-build-public.pem
chmod 0644 /etc/apk/keys/nexus-build-public.pem
apk verify /root/nexus-feed/packages.adb
apk update
apk --repository /root/nexus-feed/packages.adb add --simulate luci-app-agent-router
apk --repository /root/nexus-feed/packages.adb add luci-app-agent-router
apk info -e agentd agent-netd agent-gw agent-adapter nexus-agent-roles   nexus-cloud-connector luci-app-agent-router
```

保留官方软件源，`apk` 会从匹配的软件源解析 libc、TLS、ubus 等依赖。先看模拟结果，再执行安装；不要用 `--force`、`--allow-untrusted` 或跨版本的包解决依赖错误。裸系统未安装完整 LuCI Web 服务时，另执行 `apk add luci`。可选卡服务：`apk --repository /root/nexus-feed/packages.adb add agent-cardd`。

本例临时传入 `--repository`，不改系统默认软件源。保留 `/root/nexus-feed` 供重装；升级时制作新的签名 bundle，并对新索引重复模拟与安装。离线设备还需要完整依赖软件源，本例不是离线安装包。

## 6. 启动服务与首次配置（路由器）

```sh
for service in agent-netd agentd agent-gw agent-adapter; do
  /etc/init.d/"$service" enable
  /etc/init.d/"$service" restart
done
/etc/init.d/rpcd restart
```

启用 init 服务不等于开启所有 UCI 功能。Adapter 在 Agent services 未启用时可能暂不启动，这是配置控制的行为。Cloud、Relay 和 Directory 也不会因为装了基础包就自动连通。

登录路由器现有 LuCI 管理地址，进入 **Status → Agent Routing → User mode**：

1. 在 **Developer mode → Quick Setup** 确认唯一 Router ID 和 Agent domain。
2. 回到 User mode，启用 **Agent services**；需要 LAN 组网时启用 **Router network**。
3. 本地使用不要求 Cloud。连接 Cloud 时再配置 HTTPS 入口和配对码；普通节点保持 **Router Roles → Hosted services → Node only**。
4. 详见[首次配置](quick-setup.md)。不要把 LAN SDK `7446` 或旧 No-JWT `7445` 端口直接开放到 WAN。

## 7. 确认运行成功

在路由器 SSH 执行：

```sh
ubus call agent stats
agentctl agents 10
agentctl routes 10
curl --fail http://127.0.0.1:7788/healthz
logread | grep -E 'agentd|agent-gw|agent-adapter' | tail -n 40
```

默认 gateway 的 loopback HTTP 端口是 `7788`；若主动修改了配置，请使用实际地址。成功标准分两步：先确认 ubus 返回状态、网关 healthz 成功、User mode 的 Agent services 正常；再按[发布与调用 Agent API](../guides/publish-api.md)启动一个真实 Agent，确认租约、能力路由和一次调用响应。刚安装时 Agent/路由为空、没有邻居或 Relay 会话都可能正常，不能据此判断安装失败。

## 可选 Relay / seed 与常见错误

基础编译不会输出 `nexus-agent-router` profile 包，也不会构建 Node Relay / Directory；不能直接照抄 `apk add nexus-agent-router-seed`。这些需要额外选择 `feed/nexus-agent-profiles` 及服务包完成构建。自托管流程见[角色指南](../guides/router-roles.md)，完整镜像流程见[桌面 VM](desktop-vm.md)。

| 问题 | 排查 |
| --- | --- |
| SDK 编译失败 | 从首次错误开始检查；确认普通用户、Linux 无空格路径和网络；内存不足用 JOBS=1 |
| 没有 Nexus APK | 构建必须成功退出；查看 `bin/packages/*/nexus_agent_router`，不要拿 CMake build 目录安装 |
| UNTRUSTED signature | 核对 SDK 公钥与索引是否来自同次构建，确认 `/etc/apk/keys` 下 PEM；不要关闭验证 |
| 架构不匹配或依赖找不到 | 核对 `system board`、`apk --print-arch`、SDK target 和官方仓库版本；不要强装 |
| LuCI 没菜单 | 重新登录、刷新页面，检查 luci-app-agent-router/rpcd；裸系统检查完整 LuCI Web 服务 |
| 服务没有启动 | 查看 UCI 功能开关、User mode 状态和日志；运行 init 不会绕过 disabled 配置 |
| 能看到服务但无法调用 | 检查 Agent 注册、租约、认证以及 Router 到 Agent 回调地址的可达性 |

官方参考：[SDK 使用](https://openwrt.org/docs/guide-developer/toolchain/using_the_sdk)、[APK 管理](https://openwrt.org/docs/guide-user/additional-software/apk)。本章命令经过源码和 APK 工具核对；不能替代你的实际设备安装、升级和调用验收。
