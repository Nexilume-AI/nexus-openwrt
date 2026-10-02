include $(TOPDIR)/rules.mk

PKG_NAME:=agent-netd
PKG_VERSION:=0.2.0
PKG_RELEASE:=3

PKG_LICENSE:=LicenseRef-Nexus-Additional-Terms-1.0
PKG_MAINTAINER:=Nexus OpenWrt Team
PKG_BUILD_DIR:=$(BUILD_DIR)/$(PKG_NAME)-$(PKG_VERSION)

include $(INCLUDE_DIR)/package.mk

define Package/agent-netd
  SECTION:=net
  CATEGORY:=Network
  SUBMENU:=Routing and Redirection
  TITLE:=Restricted Nexus public IPv6 address executor
  DEPENDS:=+libubus +libubox +procd +jsonfilter
endef

define Package/agent-netd/description
 Privileged, deliberately narrow data-plane helper for P8.12/P8.18. It adds
 and removes deterministic /128 leases from a dedicated nexus-agent interface.
 In automatic mode it discovers a delegated prefix or a verified global
 on-link /64 at runtime. It owns exact NDP proxy entries for active leases only
 and cannot change the default route or forwarding policy.
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
	$(CP) ../common/agent_public_ipv6.* $(PKG_BUILD_DIR)/
endef

define Build/Compile
	$(TARGET_CC) $(TARGET_CPPFLAGS) $(TARGET_CFLAGS) \
		-std=c17 -D_GNU_SOURCE -Wall -Wextra \
		-I$(PKG_BUILD_DIR) \
		-o $(PKG_BUILD_DIR)/agent-netd \
		$(PKG_BUILD_DIR)/agent-netd.c \
		$(PKG_BUILD_DIR)/agent_public_ipv6.c \
		$(TARGET_LDFLAGS) -lubus -lubox
endef

define Package/agent-netd/install
	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/agent-netd $(1)/usr/sbin/agent-netd
	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_BIN) ./files/agent-netd.init $(1)/etc/init.d/agent-netd
	$(INSTALL_DIR) $(1)/usr/libexec
	$(INSTALL_BIN) ./files/nexus-agent-public-ipv6 \
		$(1)/usr/libexec/nexus-agent-public-ipv6
	$(INSTALL_DIR) $(1)/etc/hotplug.d/iface
	$(INSTALL_BIN) ./files/95-nexus-agent-ipv6-relay \
		$(1)/etc/hotplug.d/iface/95-nexus-agent-ipv6-relay
endef

$(eval $(call BuildPackage,agent-netd))
