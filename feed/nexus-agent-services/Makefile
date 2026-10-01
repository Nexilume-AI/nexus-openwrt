NEXUS_ROLE_PACKAGE_MAKEFILE:=$(lastword $(MAKEFILE_LIST))
include $(TOPDIR)/rules.mk

PKG_NAME:=nexus-agent-services
PKG_VERSION:=1.2.0
PKG_RELEASE:=2
PKG_LICENSE:=LicenseRef-Nexus-Community-1.0
PKG_MAINTAINER:=Nexus OpenWrt Team
PKG_BUILD_DIR:=$(BUILD_DIR)/$(PKG_NAME)-$(PKG_VERSION)
NEXUS_ROLE_PACKAGE_DIR:=$(realpath $(dir $(NEXUS_ROLE_PACKAGE_MAKEFILE)))
NEXUS_PROJECT_DIR:=$(realpath $(NEXUS_ROLE_PACKAGE_DIR)/../..)

include $(INCLUDE_DIR)/package.mk

define Package/nexus-agent-roles
  SECTION:=net
  CATEGORY:=Network
  SUBMENU:=Routing and Redirection
  TITLE:=Nexus Agent router role configuration
  PKGARCH:=all
endef

define Package/nexus-agent-relayd
  SECTION:=net
  CATEGORY:=Network
  SUBMENU:=Routing and Redirection
  TITLE:=Nexus Agent Relay server role
  DEPENDS:=+nexus-agent-roles +nexus-node-runtime +openssl-util
  PKGARCH:=all
  USERID:=nexus-relay=457:nexus-relay=457
endef

define Package/nexus-agent-directoryd
  SECTION:=net
  CATEGORY:=Network
  SUBMENU:=Routing and Redirection
  TITLE:=Nexus Agent Directory server role
  DEPENDS:=+nexus-agent-roles +nexus-node-runtime +openssl-util
  PKGARCH:=all
  USERID:=nexus-directory=458:nexus-directory=458
endef

define Package/nexus-agent-relayd/description
 Optional gateway-class Relay runtime. It runs the P6.2.4 mTLS HTTP/2 Relay
 and multi-Relay federation service under procd. The Node.js dependency makes
 this package appropriate for x86/64 and other storage-rich targets.
endef

define Package/nexus-agent-directoryd/description
 Optional gateway-class Directory runtime. It serves mTLS Relay assignments,
 HA replica identity and signed Agent Card trust artifacts under procd.
endef

define Package/nexus-agent-roles/conffiles
/etc/config/nexus_roles
endef

define Package/nexus-agent-relayd/conffiles
/etc/nexus-relayd/relay.json
endef

define Package/nexus-agent-directoryd/conffiles
/etc/nexus-directoryd/directory.json
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)/relay $(PKG_BUILD_DIR)/directory
	$(CP) $(NEXUS_PROJECT_DIR)/relay/*.js $(PKG_BUILD_DIR)/relay/
	$(CP) $(NEXUS_PROJECT_DIR)/relay/relay.config.example.json $(PKG_BUILD_DIR)/relay/relay.json
	$(CP) $(NEXUS_PROJECT_DIR)/directory/nexus-directory.js $(PKG_BUILD_DIR)/directory/
	$(CP) $(NEXUS_PROJECT_DIR)/directory/directory.config.example.json $(PKG_BUILD_DIR)/directory/directory.json
endef

define Build/Compile
endef

define Package/nexus-agent-roles/install
	$(INSTALL_DIR) $(1)/etc/config
	$(INSTALL_CONF) ./files/nexus_roles.config $(1)/etc/config/nexus_roles
	$(INSTALL_DIR) $(1)/etc/uci-defaults
	$(INSTALL_BIN) ./files/99-nexus-agent-roles-profile $(1)/etc/uci-defaults/99-nexus-agent-roles-profile
	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) ./files/nexus-open-mesh-seed-setup $(1)/usr/sbin/nexus-open-mesh-seed-setup
endef

define Package/nexus-agent-relayd/install
	$(INSTALL_DIR) $(1)/usr/lib/nexus-relayd
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/relay/*.js $(1)/usr/lib/nexus-relayd/
	$(INSTALL_DIR) $(1)/etc/nexus-relayd
	$(INSTALL_CONF) $(PKG_BUILD_DIR)/relay/relay.json $(1)/etc/nexus-relayd/relay.json
	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_BIN) ./files/nexus-relayd.init $(1)/etc/init.d/nexus-relayd
	$(INSTALL_DIR) $(1)/var/lib/nexus-relayd
endef

define Package/nexus-agent-directoryd/install
	$(INSTALL_DIR) $(1)/usr/lib/nexus-directory $(1)/usr/lib/relay
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/directory/nexus-directory.js $(1)/usr/lib/nexus-directory/
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/relay/relay-ticket.js $(1)/usr/lib/relay/
	$(INSTALL_DIR) $(1)/etc/nexus-directoryd
	$(INSTALL_CONF) $(PKG_BUILD_DIR)/directory/directory.json $(1)/etc/nexus-directoryd/directory.json
	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_BIN) ./files/nexus-directoryd.init $(1)/etc/init.d/nexus-directoryd
	$(INSTALL_DIR) $(1)/var/lib/nexus-directoryd
endef

$(eval $(call BuildPackage,nexus-agent-roles))
$(eval $(call BuildPackage,nexus-agent-relayd))
$(eval $(call BuildPackage,nexus-agent-directoryd))
