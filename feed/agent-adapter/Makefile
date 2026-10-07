include $(TOPDIR)/rules.mk

PKG_NAME:=agent-adapter
PKG_VERSION:=0.6.0
PKG_RELEASE:=9

PKG_LICENSE:=LicenseRef-Nexus-Additional-Terms-1.0
PKG_MAINTAINER:=Nexus OpenWrt Team
PKG_BUILD_DIR:=$(BUILD_DIR)/$(PKG_NAME)-$(PKG_VERSION)

include $(INCLUDE_DIR)/package.mk

define Package/agent-adapter
  SECTION:=net
  CATEGORY:=Network
  SUBMENU:=Web Servers/Proxies
  TITLE:=Nexus MCP/A2A deterministic ingress adapter
  DEPENDS:=+libuci +libubox +libjson-c +libuhttpd-nossl +libev
  USERID:=agent-adapter=456:agent-adapter=456
endef

define Package/agent-adapter/description
 OpenWrt-native P2.7 protocol normalization component. It validates an exact
 UCI capability mapping and converts bounded MCP tools/call or A2A 1.0
 message:send/message:stream JSON into a Nexus Agent Envelope without
 prompt-based routing. It provides a procd-managed loopback ingress,
 asynchronous bridge to agent-gw, protocol response mapping and an opt-in
 bounded P2.7.3 SSE relay with disconnect propagation.
 P8.9 maps a validated Last-Event-ID into the routed Envelope so MCP and A2A
 clients can reattach to the original retained task without re-execution.
endef

define Package/agent-adapter/conffiles
/etc/config/agent_adapter
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
	$(CP) ../common/agent_adapter_contract.* $(PKG_BUILD_DIR)/
	$(CP) ../common/agent_adapter_ingress_contract.* $(PKG_BUILD_DIR)/
	$(CP) ../common/agent_public_ingress.h $(PKG_BUILD_DIR)/
	$(CP) ../common/agent_invoke_contract.* $(PKG_BUILD_DIR)/
	$(CP) ../common/agent_ipc_protocol.h $(PKG_BUILD_DIR)/
	$(CP) ../common/agent_sse_contract.* $(PKG_BUILD_DIR)/
endef

define Build/Compile
	$(TARGET_CC) \
		$(TARGET_CPPFLAGS) $(TARGET_CFLAGS) \
		-std=c17 -D_GNU_SOURCE -Wall -Wextra \
		-I$(PKG_BUILD_DIR) \
		-o $(PKG_BUILD_DIR)/agent-adapter-map \
		$(PKG_BUILD_DIR)/agent_adapter_map.c \
		$(PKG_BUILD_DIR)/adapter_registry.c \
		$(PKG_BUILD_DIR)/agent_adapter_contract.c \
		$(TARGET_LDFLAGS) -luci -lubox -ljson-c
	$(TARGET_CC) \
		$(TARGET_CPPFLAGS) $(TARGET_CFLAGS) \
		-std=c17 -D_GNU_SOURCE -Wall -Wextra \
		-I$(PKG_BUILD_DIR) \
		-o $(PKG_BUILD_DIR)/agent-adapterd \
		$(PKG_BUILD_DIR)/agent_adapterd.c \
		$(PKG_BUILD_DIR)/adapter_codec.c \
		$(PKG_BUILD_DIR)/adapter_registry.c \
		$(PKG_BUILD_DIR)/agent_adapter_contract.c \
		$(PKG_BUILD_DIR)/agent_adapter_ingress_contract.c \
		$(PKG_BUILD_DIR)/agent_invoke_contract.c \
		$(PKG_BUILD_DIR)/agent_sse_contract.c \
		$(TARGET_LDFLAGS) -luci -lubox -ljson-c -luhttpd -lev
endef

define Package/agent-adapter/install
	$(INSTALL_DIR) $(1)/usr/bin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/agent-adapter-map \
		$(1)/usr/bin/agent-adapter-map
	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/agent-adapterd \
		$(1)/usr/sbin/agent-adapterd

	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_BIN) ./files/agent-adapter.init \
		$(1)/etc/init.d/agent-adapter

	$(INSTALL_DIR) $(1)/etc/config
	$(INSTALL_CONF) ./files/agent-adapter.config \
		$(1)/etc/config/agent_adapter

	$(INSTALL_DIR) $(1)/lib/upgrade/keep.d
	$(INSTALL_DATA) ./files/agent-adapter.keep \
		$(1)/lib/upgrade/keep.d/agent-adapter
endef

define Package/agent-adapter/postinst
#!/bin/sh
[ -n "$${IPKG_INSTROOT}" ] && exit 0
if [ ! -s /etc/config/agent_adapter ] && [ -s /etc/config/agent_adapter.apk-new ]; then
	cp /etc/config/agent_adapter.apk-new /etc/config/agent_adapter
	chmod 0600 /etc/config/agent_adapter
fi
if [ -s /etc/init.d/agent-adapter.apk-new ]; then
	cp -p /etc/init.d/agent-adapter /etc/init.d/agent-adapter.pre-nexus-upgrade 2>/dev/null || true
	mv /etc/init.d/agent-adapter.apk-new /etc/init.d/agent-adapter
	chmod 0755 /etc/init.d/agent-adapter
fi
/etc/init.d/agent-adapter enable
/etc/init.d/agent-adapter restart || true
exit 0
endef

$(eval $(call BuildPackage,agent-adapter))
