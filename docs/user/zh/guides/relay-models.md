# Relay 设备登录与模型注册

使用 OpenWrt 的 `nexilume` Python SDK（0.52.1+），无需安装 `nexus_cli`。
SDK 注册的是已运行、带鉴权的 HTTPS 模型接口，不会自动创建模型服务、
开放入站端口或建立网络隧道。已有 Relay 推理传输可以继续使用。

## 安装

需要 Linux/OpenWrt、Python 3.9+ 和可写的私人用户目录。
以下从开源仓库安装，不依赖该版本已经发布到 PyPI：

```bash
python3 -m venv ~/.local/share/nexus-relay-venv
~/.local/share/nexus-relay-venv/bin/python -m pip install \
  'git+https://github.com/Nexilume-AI/nexus-agent-sdk-python.git@main'
export PATH="$HOME/.local/share/nexus-relay-venv/bin:$PATH"
nexus-relay --version
```

生产环境建议将 `main` 替换为审核后的提交 SHA。没有 venv/pip 的 OpenWrt
镜像可用系统 Python 工具安装 SDK wheel，或从源码目录设置 `PYTHONPATH=src`，
使用 `python3 -m nexus_agent.relay_cli`。

## 登录与注册

```bash
export NEXUS_RELAY_CLOUD=https://cloud.nexilume.com
export NEXUS_RELAY_DEVICE=my-model-host
nexus-relay login --model my-model-v1 \
  --endpoint https://models.example.com/v1 \
  --protocol openai-compatible --name 'My model'
# 浏览器登录后核对设备码、模型及接口，选择自己的 Organization 并确认。
nexus-relay model register my-model-v1 --prompt-endpoint-key
nexus-relay status
```

协议可选 `openai-compatible` 或 `system_one`。已有接口的密钥仅需通过隐藏输入
导入一次，之后直接 `nexus-relay model register my-model-v1`。SDK 自动保存凭据
和注册记录，不需要手填 key-file/state-file，也不接受明文 `--key` 参数。
自动化可从密钥管理器通过 `--endpoint-key-stdin` 导入。

Cloud 校验真实模型目录与健康状态，创建私有 Provider → Source → Model Pool →
Router。重复注册复用资源 ID；不会强制健康、扩大授权或恢复管理员停用的资源。
调用者在 Cloud 导出 Router API Key，通过 Router API 推理；设备凭据不能用于推理。

无浏览器设备可用 `login --no-wait` 后在另一台电脑确认，再执行 `login --resume`。
短码有效十分钟，确认后只可兑换一次；登录等待默认最多 600 秒，Ctrl+C 保留待确认请求。
多模型可用 `login --models-json models.json`，数组中每项包含
`name`、`model`、`endpoint`、`protocol`，最多 32 个不同模型，文件不含密钥。
新增模型或更换接口需要重新明确确认。

## 刷新、撤销与故障恢复

```bash
nexus-relay refresh
nexus-relay revoke          # 交互确认
nexus-relay revoke --yes    # 自动化中明确确认
```

- 默认 Cloud 为 `https://cloud.nexilume.com`，设备名为主机名，也可用 `--cloud`、`--device`。
- 使用相同系统用户、Cloud 地址、设备名和 `XDG_DATA_HOME`，才能复用绑定。
- 存储在 `$XDG_DATA_HOME/nexilume/relay/<identity>/device.json`，或用户目录下的
  `~/.local/share/nexilume/relay/<identity>/device.json`；目录 0700、文件 0600，带锁和原子写入。
- 访问令牌十分钟；刷新令牌七天且每次轮换；总授权最多三十天。SDK 在请求前自动刷新，
  CLI 不安装后台守护进程。长期设备可同用户每四分钟执行一次 `status`；鉴权失败应停止并重新绑定。
- 权限、成员身份、密码或授权状态变更会重新验证，过期或撤销不能靠重试恢复。
- 浏览器 `/api/v1/auth/relay/page/` 中的 **Your Relay devices** 可撤销遗失设备。
  撤销禁用该设备注册的 Sources/Providers，阻止新调用；不会取消已经发送到独立后端的推理。
- 注册断线后先查询 `status`，再重复同一注册；令牌兑换/轮换成功但响应丢失时，可能需要重新登录。
- `--json` 输出机器可读结果；退出码 0 为成功/主动取消/明确不等待，1 为服务错误，2 为参数或存储错误，
  3 为等待超时，130 为中断；同时检查返回的 `status`。

不要将凭据目录提交仓库、写入日志或放入未加密备份。模型目录缺失或不健康时，
先修复接口地址、模型 ID、网络与接口鉴权。完整 Python API 及英文说明见
[SDK Relay 指南](https://github.com/Nexilume-AI/nexus-agent-sdk-python/blob/main/docs/relay-models.md)。
