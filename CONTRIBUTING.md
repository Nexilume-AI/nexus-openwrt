# Contributing

欢迎中文或英文 Issue / Pull Request。问题报告请包含目标型号、OpenWrt 和包版本、复现步骤、预期与实际行为；日志须移除设备凭据和个人信息。安全漏洞请遵循 [SECURITY.md](SECURITY.md)。

1. 从新分支修改源码，说明用户可观察到的变化和涉及的部署模式。
2. 执行 README 中的 Debug CMake、Relay 和桌面打包测试。对 LuCI、UCI 或包安装行为的修改还应在匹配的 OpenWrt 设备上验证，注明未覆盖的环境。
3. 更新相关中英文用户文档；包行为改变时更新相应 `PKG_RELEASE`。
4. 保留第三方版权与许可证，说明新增依赖来源、版本和校验值。提交内容应是你有权按对应文件许可证贡献的代码；不要附带 Cloud 数据、真实证书私钥、设备备份、磁盘或构建日志。

维护者按改动范围审查正确性、接口兼容性和测试证据。当前未设响应时限或长期版本支持承诺。CI 主机测试不替代硬件或 VM 验收。

## Contribution licensing

Nexus-authored changes are distributed under the Apache License 2.0 (modified).
Read [LICENSE](LICENSE), [LICENSING.md](LICENSING.md) and the
[Nexus Contributor License Agreement](CONTRIBUTOR_LICENSE_AGREEMENT.md).
Every contributing author must explicitly accept that agreement for their PR
before merge; maintainers must record the acceptance as described there.
Contributors retain copyright while permitting commercial use, dual licensing
and future relicensing. Historical contributions and third-party code are not
automatically subject to the new grant. Preserve all upstream notices.

许可咨询：cary.nexilume@outlook.com。每位贡献者须对本 PR 明确同意贡献者协议；
仅勾选模板或由维护者代为声明不构成其他作者的同意。
