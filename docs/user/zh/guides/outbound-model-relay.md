# 通用模型 Relay 转发

SDK 0.53.0 将模型转发整合到设备码登录和注册流程，不依赖 nexus_cli，也不要求用户维护 key-file 或 state-file。源代码安装需 Linux/OpenWrt、Python 3.11+ 及 relay 可选依赖；PyPI 旧版 0.52.1 不包含 serve 命令。

```bash
python -m pip install 'nexilume[relay] @ git+https://github.com/Nexilume-AI/nexus-agent-sdk-python.git@main'
export NEXUS_RELAY_DEVICE='11 model relay'
nexus-relay login --models-json models.json --no-wait
# 在网页确认设备、模型、上游和正确的 Organization
nexus-relay login --resume
nexus-relay model register MODEL
nexus-relay serve
# 另一终端使用同一个 device 名称
nexus-relay model activate MODEL
```

models.json 的格式见[英文完整指南](https://github.com/Nexilume-AI/nexus-agent-sdk-python/blob/main/docs/outbound-model-relay.md)。已有 Provider 填写 provider_id、transport: outbound、全部现有 Source 的模型 ID 和原始上游 HTTPS 地址。不同 Organization 使用不同设备身份。

register 只准备绑定；activate 经新通道核对模型目录后才切换。Provider、Source、Router 的 ID、现有 Key 和 Router 绑定保持不变，并发修改会阻止切换。接入新 Provider 时省略 provider_id，通过隐藏提示导入上游 Key 一次。

serve 自动刷新设备凭据和重连，支持 JSON、SSE、取消及背压。流没有固定总时长，空闲超时为 180 秒；中断的生成不会自动重放，也不会伪装为成功完成。每个设备最多并发 8 次请求，请求上限 16 MiB。

refresh 手动轮换凭据；revoke --yes 撤销设备。新调用立即受阻，现有通道通常在 22 秒内关闭。撤销不删除已有 Provider 或改回直连；重新绑定需管理员检查并解除旧绑定。

两端均校验模型范围、上游地址、协议和 Provider。TLS 校验保持开启，不跟随重定向，不允许私网、保留地址上游。Cloud 继续执行 Router 权限、计量、审计和模型能力下发；设备状态与日志不输出 Key。

Cloud 部署需要配套代码、数据库迁移，以及一个统一的本地 broker，通过现有 TLS 入口公开 /api/model-relay/。完整启动命令、反向代理要求和限制见英文指南。无需为 Kimi、MiniMax 等供应商分别编写代理。
