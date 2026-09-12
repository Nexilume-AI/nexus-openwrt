# luci-theme-argon external dependency notice

Nexus OpenWrt does not vendor the Argon source tree. The build downloads the
immutable, checksum-locked `v2.4.6` source archive from the upstream project and
builds it inside a temporary OpenWrt SDK feed.

- Project: `luci-theme-argon`
- Upstream: <https://github.com/jerrykuku/luci-theme-argon>
- Copyright: Jerrykuku and contributors
- License: Apache License 2.0
- Included component: theme only; `luci-app-argon-config` is intentionally not
  included

The build normalizes the package metadata from the values present in the tag
(`2.4.5/20260718`) to the release identity `2.4.6-r1`. It also adds a rendered
HTML color-scheme marker used only by Nexus theme-neutral CSS. No upstream source
is committed to this repository.
