include $(TOPDIR)/rules.mk

LUCI_TITLE:=Nexus Agent Router management UI
LUCI_DEPENDS:=+luci-base +agentd +agent-netd +agent-gw +agent-adapter +nexus-agent-roles +nexus-cloud-connector
LUCI_PKGARCH:=all

PKG_VERSION:=3.1.0
PKG_RELEASE:=21
PKG_LICENSE:=Apache-2.0
PKG_MAINTAINER:=Nexus OpenWrt Team

define Package/luci-app-agent-router/conffiles
/etc/config/agent
/etc/config/agent_peers
/etc/config/agent_policy
/etc/config/nexus_cloud
endef

define Package/luci-app-agent-router/postinst
#!/bin/sh
chmod 0755 "$${IPKG_INSTROOT}/usr/libexec/rpcd/nexus-agent-ui" 2>/dev/null || true
[ -n "$${IPKG_INSTROOT}" ] || /etc/init.d/rpcd reload >/dev/null 2>&1 || true
exit 0
endef

include $(TOPDIR)/feeds/luci/luci.mk

# call BuildPackage - OpenWrt buildroot signature
