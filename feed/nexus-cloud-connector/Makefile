include $(TOPDIR)/rules.mk

PKG_NAME:=nexus-cloud-connector
PKG_VERSION:=1.1.0
PKG_RELEASE:=43
PKG_LICENSE:=Apache-2.0
PKG_MAINTAINER:=Nexus OpenWrt Team

include $(INCLUDE_DIR)/package.mk

define Package/nexus-cloud-connector
  SECTION:=net
  CATEGORY:=Network
  SUBMENU:=Routing and Redirection
  TITLE:=Nexus Cloud connector for Direct IPv6 and Relay Agents
  DEPENDS:=+agentd +agent-gw +agent-adapter +curl +ca-bundle +jshn +ubus +uci +openssl-util
  PKGARCH:=all
endef

define Package/nexus-cloud-connector/description
 Enrolls a Nexus OpenWrt router with Nexus Server, reads bounded local Agent
 and route/Relay snapshots from agentd, and maintains renewable cloud Agent
 registrations through Direct IPv6 or an outbound NAT-safe Relay tunnel.
endef

define Package/nexus-cloud-connector/conffiles
/etc/config/nexus_cloud
endef

define Build/Compile
endef

define Package/nexus-cloud-connector/install
	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) ./files/nexus-cloud-connectord $(1)/usr/sbin/nexus-cloud-connectord
	$(INSTALL_BIN) ./files/nexus-cloud-tls-check $(1)/usr/sbin/nexus-cloud-tls-check
	$(INSTALL_DIR) $(1)/usr/libexec/rpcd
	$(INSTALL_BIN) ./files/nexus-cloud-rpcd $(1)/usr/libexec/rpcd/nexus-cloud
	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_BIN) ./files/nexus-cloud.init $(1)/etc/init.d/nexus-cloud
	$(INSTALL_DIR) $(1)/etc/config
	$(INSTALL_CONF) ./files/nexus-cloud.config $(1)/etc/config/nexus_cloud
	$(INSTALL_DIR) $(1)/lib/upgrade/keep.d
	$(INSTALL_DATA) ./files/nexus-cloud.keep $(1)/lib/upgrade/keep.d/nexus-cloud
endef

$(eval $(call BuildPackage,nexus-cloud-connector))
