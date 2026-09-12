---
sidebar_position: 1
title: Support matrix
---

# Support matrix

This page separates the project's build baseline from environments with recorded target acceptance. An untested combination is not presented as supported.

## OpenWrt

| Item | Status | Notes |
| --- | --- | --- |
| OpenWrt 25.12.x | Build baseline | Package APIs and dependencies target this branch |
| OpenWrt 25.12.4 x86/64 | Verified | SDK builds and multi-router Hyper-V runtime acceptance passed |
| OpenWrt 25.12.x ARM64/MIPS | Validation required | A successful cross-build is not device runtime acceptance |
| OpenWrt 24.10 and earlier | Not supported by contract | Dependencies, LuCI APIs, and package managers may differ |
| Vendor-derived firmware | Vendor validation required | Check kernel, libubus, LuCI, and firewall differences |

The OpenWrt 25.12 reference target uses `apk`. The `opkg` commonly found on older releases does not imply support for those branches.

## Browser and administration

- LuCI inherits the browser support policy of the installed OpenWrt release.
- Use a security-supported Chrome, Edge, or Firefox release.
- Agent Router pages require JavaScript.

## Python SDK

| Item | Supported range |
| --- | --- |
| Python | 3.9 or later, per `pyproject.toml` |
| Base install | No third-party runtime dependency |
| FastMCP | `fastmcp>=3.0,<4` |
| A2A | `a2a-sdk>=1.1,<2` |
| Windows IPv6 | Windows with `pywin32>=306` |

## Preflight on a new target

```sh
ubus call system board
df -h
free
apk --version
```

Use the matching OpenWrt SDK for a clean build, install the packages, and run [Collect diagnostics](../troubleshooting/diagnostics.md). Before adding a physical model to this table, record its firmware, architecture, installation result, service state, and at least one successful agent call.
