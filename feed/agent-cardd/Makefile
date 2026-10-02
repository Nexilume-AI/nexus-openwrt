include $(TOPDIR)/rules.mk

PKG_NAME:=agent-cardd
PKG_VERSION:=0.2.0
PKG_RELEASE:=2

PKG_LICENSE:=LicenseRef-Nexus-Additional-Terms-1.0
PKG_MAINTAINER:=Nexus OpenWrt Team

include $(INCLUDE_DIR)/package.mk

define Package/agent-cardd
  SECTION:=net
  CATEGORY:=Network
  SUBMENU:=Routing and Redirection
  TITLE:=Nexus signed Agent Card fetch and verification service
  DEPENDS:=+agentd +uclient-fetch +usign +ca-bundle +jshn
endef

define Package/agent-cardd/description
 Downloads DNSSEC-discovered Agent Cards and detached signatures. It supports
 both a pinned usign key and a signed Directory trust bundle with bounded key
 rotation and revocation, then submits verified semantics to agentd.
endef

define Package/agent-cardd/conffiles
/etc/config/agent_card
/etc/agentd/card-trust.pub
/etc/agentd/directory-root.pub
endef

define Build/Compile
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
endef

define Package/agent-cardd/install
	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) ./files/agent-cardd $(1)/usr/sbin/agent-cardd

	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_BIN) ./files/agent-cardd.init $(1)/etc/init.d/agent-cardd

	$(INSTALL_DIR) $(1)/etc/config
	$(INSTALL_CONF) ./files/agent-cardd.config $(1)/etc/config/agent_card

	$(INSTALL_DIR) $(1)/etc/agentd
	$(INSTALL_DATA) ./files/card-trust.pub.example \
		$(1)/etc/agentd/card-trust.pub.example
	$(INSTALL_DATA) ./files/directory-root.pub.example \
		$(1)/etc/agentd/directory-root.pub.example

	$(INSTALL_DIR) $(1)/lib/upgrade/keep.d
	$(INSTALL_DATA) ./files/agent-cardd.keep \
		$(1)/lib/upgrade/keep.d/agent-cardd
endef

$(eval $(call BuildPackage,agent-cardd))
