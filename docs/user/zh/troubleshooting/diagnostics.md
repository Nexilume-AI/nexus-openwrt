---
sidebar_position: 0
title: 一键收集诊断
---

# 一键收集诊断

诊断脚本只执行只读命令，并对日志中的常见凭据字段做脱敏。它不会读取完整 UCI 配置、私钥、证书正文、任务 payload 或模型输出。

## 运行

把脚本下载到路由器，检查内容后运行：

```sh
wget -O /tmp/nexus-diagnostics.sh https://YOUR-DOCS-HOST/downloads/nexus-openwrt-diagnostics.sh
chmod 700 /tmp/nexus-diagnostics.sh
/tmp/nexus-diagnostics.sh > /tmp/nexus-diagnostics.txt 2>&1
```

文档站尚未部署时，可从仓库的 `docs-site/static/downloads/` 复制脚本。

## 分享前检查

```sh
grep -Ein 'token|secret|password|authorization|private.key' /tmp/nexus-diagnostics.txt
```

脚本会尽力脱敏，但管理员仍应人工检查输出。不要公开上传系统备份、完整 `/etc/config`、私钥或访问令牌。

## 输出内容

- OpenWrt 版本、目标架构、时间和磁盘/内存概况。
- Nexus 软件包与 init 服务状态。
- `agent` ubus 对象和有界状态方法。
- 关键非敏感 UCI 开关。
- 最近 200 条相关且已脱敏的系统日志。

把输出与问题发生时间、操作步骤、预期结果一起提供给支持人员。
