---
sidebar_position: 0
title: Collect diagnostics
---

# Collect diagnostics

The collector runs read-only commands and redacts common credential fields in logs. It does not read complete UCI files, private keys, certificate bodies, task payloads, or model output.

## Run

Download the script to the router, review it, and run it:

```sh
wget -O /tmp/nexus-diagnostics.sh https://YOUR-DOCS-HOST/downloads/nexus-openwrt-diagnostics.sh
chmod 700 /tmp/nexus-diagnostics.sh
/tmp/nexus-diagnostics.sh > /tmp/nexus-diagnostics.txt 2>&1
```

Before the site is deployed, copy it from `docs-site/static/downloads/` in the repository.

## Review before sharing

```sh
grep -Ein 'token|secret|password|authorization|private.key' /tmp/nexus-diagnostics.txt
```

Redaction is best effort; an administrator must still review the report. Never publish a system backup, complete `/etc/config`, private keys, or access tokens.

## Contents

- OpenWrt release, target architecture, time, disk, and memory summary.
- Nexus packages and init service state.
- The `agent` ubus object and bounded status methods.
- A safe subset of non-secret UCI switches.
- The latest 200 related, redacted system log lines.

Provide the report together with the incident time, exact steps, and expected result.
