# Nexus OpenWrt

OpenWrt 上的 Agent 注册、发现、能力路由与调用网关，包含 LuCI 管理界面、Cloud 连接客户端及可选的自托管 Relay / Directory。

Agent registration, discovery and capability routing on OpenWrt, with LuCI management and optional self-hosted Relay / Directory services.

## 当前发布范围 / Release scope

这是独立源码发布候选，面向 OpenWrt 25.12.x；当前镜像制作基线为 25.12.4 x86/64。其他架构需要使用匹配 SDK 构建与设备验收。此仓库不包含 Nexus Cloud Server、TokenBank 或独立 Python SDK。

桌面 Hyper-V 启动器和 IPv6 配置脚本已提供，**预装镜像和真实开机验收尚未完成**。源码测试通过不等于所有路由器型号或桌面镜像已通过生产验收。请勿将内部使用过的磁盘、配置或签名私钥作为 Release 附件。

## 用户入口

- [中文手册](docs/user/zh/index.md) / [English manual](docs/user/en/index.md)
- [安装路径](docs/user/zh/getting-started/choose-path.md) / [双路由器教程](docs/user/zh/tutorials/two-router.md)
- [Cloud Relay](docs/user/zh/guides/cloud-relay.md) / [自托管角色](docs/user/zh/guides/router-roles.md)
- [电脑虚拟机启动与 IPv6](deploy/desktop/README.md) / [制作干净镜像](deploy/desktop/BUILD.md)

LuCI 入口：**Status → Agent Routing → User mode**。先连接 Cloud 或配置 Router network，再启用 Agent services。Cloud Direct IPv6 / Relay 客户端与自托管 Open Mesh seed 是两种不同部署方式。Relay 和 Directory 示例配置包含不可运行的签名密钥占位符，部署时必须生成自己的密钥和证书；OpenWrt seed 的设置流程见角色指南。

## 编译 APK → 安装 → 运行

首次安装请按[完整中文教程](docs/user/zh/getting-started/install.md)或[English instructions](docs/user/en/getting-started/install.md)顺序执行，包含官方 SDK 下载地址、校验值、签名和故障排查。电脑使用 Linux x86_64，路由器示例固定 OpenWrt 25.12.4 x86/64；**默认配置不能直接用于 ARM**。

1. **确认设备**：在路由器执行 `ubus call system board`、`apk --print-arch`，选择匹配版本和 target/subtarget 的 SDK。
2. **电脑编译**：按教程安装依赖，将源码和新 SDK 放在 Linux 无空格路径；在仓库根目录执行：

   ```sh
   JOBS=2 sh scripts/build-openwrt-sdk.sh /absolute/path/to/openwrt-sdk
   ```

   脚本覆盖 SDK `.config`；默认输出在 SDK 的 `bin/packages/x86_64/nexus_agent_router/`。CMake 主机测试不会产出 APK。
3. **制作签名软件源并传输**：按教程将新生成的 APK 打包成 `nexus-feed`，用 SDK 私钥签名 `packages.adb`，只复制 APK、索引、公钥和 SHA256SUMS 到路由器。私钥留在电脑。
4. **路由器安装**：先按教程比对公钥摘要、导入 `/etc/apk/keys` 并验证索引，再执行：

   ```sh
   apk update
   apk --repository /root/nexus-feed/packages.adb add --simulate luci-app-agent-router
   apk --repository /root/nexus-feed/packages.adb add luci-app-agent-router
   ```

   保留匹配的官方软件源供依赖解析；不关闭签名检查。LuCI 包会拉入基础路由组件和 Cloud connector。完整 LuCI Web 服务未安装时另执行 `apk add luci`。
5. **启动与验证**：按教程启用 `agent-netd`、`agentd`、`agent-gw`、`agent-adapter`，重载 rpcd；进入 **Status → Agent Routing → User mode**，确认唯一 Router ID 并启用 **Agent services**。使用 `ubus call agent stats`、`curl --fail http://127.0.0.1:7788/healthz` 检查服务，再完成一个真实 Agent 的注册和调用。

基础 helper 不构建 `nexus-agent-router` profile 元包及 Node/Relay/Directory；不能直接安装尚未生成的 `nexus-agent-router-seed`。基础 Cloud 客户端无需自托管 Relay。进阶角色见角色指南，完整 VM 镜像见制作指南；这些需要额外构建和验收。

## 源码验证 / Host tests

Linux 上需要 C17 编译器、CMake ≥ 3.16、Python 3、Node.js ≥ 20 和 OpenSSL 开发库及 CLI。例如 Debian/Ubuntu：

```sh
sudo apt-get update
sudo apt-get install build-essential cmake ninja-build python3 libssl-dev openssl
# Separately install Node.js >= 20; check node --version.
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DNEXUS_INTEGRATION_TESTS=OFF
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
npm --prefix relay test
python3 -m unittest discover -s deploy/desktop -p test_bundle.py
```

这些测试覆盖主机上的路由核心、配置契约及 Relay/Directory 协议；不启动完整 OpenWrt。`NEXUS_INTEGRATION_TESTS=OFF` 明确排除依赖独立 SDK 和内部实验室编排的测试。完整 CMake 契约保留这些入口供主仓库集成验证。使用 Debug 构建，避免 C `assert` 检查被优化配置关闭。

## 贡献与许可证

[CONTRIBUTING](CONTRIBUTING.md) · [SECURITY](SECURITY.md) · [CHANGELOG](CHANGELOG.md) · [发布检查](RELEASING.md)

Nexus 自有代码采用 [Apache-2.0](LICENSE)，具体边界见 [NOTICE](NOTICE)。第三方代码和构建依赖保留原许可证，见 [THIRD_PARTY](THIRD_PARTY.md)。特别是 Node 的 OpenWrt 构建配方保留 GPL v2，不能把整张 OpenWrt 固件标为仅 Apache-2.0。源码包的逐文件 SHA-256 在 `SOURCE-MANIFEST.json`；该清单用于追溯导出，不是签名或安全审计证明。
