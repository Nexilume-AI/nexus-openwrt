include $(TOPDIR)/rules.mk

PKG_NAME:=agentd
PKG_VERSION:=3.1.0
PKG_RELEASE:=33

PKG_LICENSE:=LicenseRef-Nexus-Additional-Terms-1.0
PKG_MAINTAINER:=Nexus OpenWrt Team
PKG_BUILD_DIR:=$(BUILD_DIR)/$(PKG_NAME)-$(PKG_VERSION)

include $(INCLUDE_DIR)/package.mk

define Package/agentd
  SECTION:=net
  CATEGORY:=Network
  SUBMENU:=Routing and Redirection
  TITLE:=Nexus Agent routing control-plane daemon
  DEPENDS:=+libubus +libubox +libuci +libjson-c +libnghttp2 +libmbedtls +procd-ujail +umdns +openssl-util +agent-netd
  USERID:=agentd=455:agentd=455
endef

define Package/agentd/description
 Native OpenWrt dual-plane daemon for the Nexus capability ARIB and intent
 AFIB. P6.1 adds final-target verification of short-lived, body-bound Router
 Forwarding Assertions with replay rejection across Relay and LAN Transit.
 P6.2 adds signed Agent Card authorization for SVCB peer promotion and ARPX
 capability UPDATE admission, including Directory trust bundles and key
 rotation/revocation. P6.2.4 adds bounded Directory replica rotation and
 sticky Relay failover assignments without changing the IP routing plane.
 P8.1 extends the credential-checked native IPC with local Agent lease
 register, renew and unregister operations for the Agent Access Proxy.
 P6.3 adds a native Agent Policy RIB, identity-aware AFIB constraints,
 deterministic traffic-engineering weights and health dampening.
 P7.1 keeps management and IPC alive with fail-closed empty routing state when
 static UCI is invalid, and exposes bounded recovery telemetry for repair.
 P7.2 adds bounded Route ID and intent/version indexes plus capacity telemetry;
 the diagnostic explain path retains complete rejection counters.
 P7.6 adds an honest, bounded local-Agent view grouped from active local
 capability leases without claiming ownership of application transport sockets.
 P8.4 adds IPC v6 exact target-Agent selection across AFIB, LAN Transit and
 Relay paths while retaining score-based selection for untargeted calls.
 P8.12.1 adds router-owned public IPv6 /128 leases and delegates privileged
 address mutations to the prefix-constrained agent-netd helper. P8.12.2 uses
 IPC v8 to bind an authenticated public destination /128 to one exact local
 Agent origin; the authenticated target and intent select a healthy local
 capability route without cross-origin failover.
 P8.27 sends NFA-authorized, Relay-selected HTTPS local routes through a
 credentialed loopback Gateway bridge so the existing lease-bound TLS
 executor remains the sole owner of backend CA and certificate-pin state.
endef

define Package/agentd-bench
  SECTION:=net
  CATEGORY:=Network
  SUBMENU:=Routing and Redirection
  TITLE:=Nexus Agent routing capacity benchmark
  DEPENDS:=+libc
endef

define Package/agentd-bench/description
 Standalone P7.2 benchmark for 10,000-route AFIB population, exact lookup,
 renewal, withdrawal and bounded memory gates. It does not expose a production
 ubus mutation method and does not modify the running agentd table.
endef

define Package/agentd/conffiles
/etc/config/agent
/etc/config/agent_peers
/etc/config/agent_policy
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
	$(CP) ../common/* $(PKG_BUILD_DIR)/
endef

define Build/Compile
	$(TARGET_CC) \
		$(TARGET_CPPFLAGS) $(TARGET_CFLAGS) \
		-std=c17 -D_GNU_SOURCE -Wall -Wextra \
		-I$(PKG_BUILD_DIR) \
		-o $(PKG_BUILD_DIR)/agentd \
		$(PKG_BUILD_DIR)/agentd.c \
		$(PKG_BUILD_DIR)/agent_dynamic_manifest.c \
		$(PKG_BUILD_DIR)/agent_adapter_contract.c \
		$(PKG_BUILD_DIR)/agent_local_agents.c \
		$(PKG_BUILD_DIR)/agent_arpx_protocol.c \
		$(PKG_BUILD_DIR)/agent_peer_transport_contract.c \
		$(PKG_BUILD_DIR)/agent_ipc_protocol.c \
		$(PKG_BUILD_DIR)/agent_public_ipv6.c \
		$(PKG_BUILD_DIR)/agent_ipc_server.c \
		$(PKG_BUILD_DIR)/agent_peer_session.c \
		$(PKG_BUILD_DIR)/agent_peer_listener.c \
		$(PKG_BUILD_DIR)/agent_peer_transport.c \
		$(PKG_BUILD_DIR)/agent_peer_routes.c \
		$(PKG_BUILD_DIR)/agent_discovery.c \
		$(PKG_BUILD_DIR)/agent_cross_discovery.c \
		$(PKG_BUILD_DIR)/agent_discovery_promotion.c \
		$(PKG_BUILD_DIR)/agent_auto_promotion.c \
		$(PKG_BUILD_DIR)/agent_card_authorization.c \
		$(PKG_BUILD_DIR)/agent_card_directory_trust.c \
		$(PKG_BUILD_DIR)/agent_relay_directory.c \
		$(PKG_BUILD_DIR)/agent_mesh_profile.c \
		$(PKG_BUILD_DIR)/agent_relay_bootstrap.c \
		$(PKG_BUILD_DIR)/agent_relay_tunnel.c \
		$(PKG_BUILD_DIR)/agent_relay_invoke.c \
		$(PKG_BUILD_DIR)/agent_forwarding_assertion.c \
		$(PKG_BUILD_DIR)/agent_replay_cache.c \
		$(PKG_BUILD_DIR)/agent_invoke_contract.c \
		$(PKG_BUILD_DIR)/agent_sse_contract.c \
		$(PKG_BUILD_DIR)/agent_recovery.c \
		$(PKG_BUILD_DIR)/peer_table.c \
		$(PKG_BUILD_DIR)/route_table.c \
		$(PKG_BUILD_DIR)/agent_policy.c \
		$(PKG_BUILD_DIR)/static_peers.c \
		$(PKG_BUILD_DIR)/static_policies.c \
		$(PKG_BUILD_DIR)/static_routes.c \
		$(TARGET_LDFLAGS) -lubus -lubox -luci -ljson-c -lnghttp2 \
		-lmbedtls -lmbedx509 -lmbedcrypto
	$(TARGET_CC) $(TARGET_CPPFLAGS) $(TARGET_CFLAGS) -std=c17 -D_GNU_SOURCE \
		-DAGENT_MESH_PROFILE_CLI -Wall -Wextra -I$(PKG_BUILD_DIR) \
		-o $(PKG_BUILD_DIR)/agent-mesh-profile $(PKG_BUILD_DIR)/agent_mesh_profile.c \
		$(TARGET_LDFLAGS) -ljson-c -lmbedcrypto
	$(TARGET_CC) \
		$(TARGET_CPPFLAGS) $(TARGET_CFLAGS) -O2 \
		-std=c17 -D_GNU_SOURCE -Wall -Wextra \
		-I$(PKG_BUILD_DIR) \
		-o $(PKG_BUILD_DIR)/agent-route-bench \
		$(PKG_BUILD_DIR)/agent_route_benchmark.c \
		$(PKG_BUILD_DIR)/route_table.c \
		$(PKG_BUILD_DIR)/agent_policy.c \
		$(TARGET_LDFLAGS)
endef

define Package/agentd/install
	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/agentd $(1)/usr/sbin/agentd
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/agent-mesh-profile $(1)/usr/sbin/agent-mesh-profile

	$(INSTALL_DIR) $(1)/usr/bin
	$(INSTALL_BIN) ./files/agentctl $(1)/usr/bin/agentctl

	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_BIN) ./files/agentd.init $(1)/etc/init.d/agentd

	$(INSTALL_DIR) $(1)/etc/config
	$(INSTALL_CONF) ./files/agentd.config $(1)/etc/config/agent
	$(INSTALL_CONF) ./files/agent_peers.config $(1)/etc/config/agent_peers
	$(INSTALL_CONF) ./files/agent_policy.config $(1)/etc/config/agent_policy

	$(INSTALL_DIR) $(1)/usr/share/rpcd/acl.d
	$(INSTALL_DATA) ./files/agentd.acl.json \
		$(1)/usr/share/rpcd/acl.d/agentd.json

	$(INSTALL_DIR) $(1)/lib/upgrade/keep.d
	$(INSTALL_DATA) ./files/agentd.keep $(1)/lib/upgrade/keep.d/agentd
endef

define Package/agentd-bench/install
	$(INSTALL_DIR) $(1)/usr/bin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/agent-route-bench \
		$(1)/usr/bin/agent-route-bench
endef

define Package/agentd/postinst
#!/bin/sh
[ -n "$${IPKG_INSTROOT}" ] && exit 0
for name in agent agent_peers agent_policy; do
	if [ ! -s "/etc/config/$$name" ] && [ -s "/etc/config/$$name.apk-new" ]; then
		cp "/etc/config/$$name.apk-new" "/etc/config/$$name"
		chmod 0600 "/etc/config/$$name"
	fi
done
if [ -s /etc/init.d/agentd.apk-new ]; then
	cp -p /etc/init.d/agentd /etc/init.d/agentd.pre-nexus-upgrade 2>/dev/null || true
	mv /etc/init.d/agentd.apk-new /etc/init.d/agentd
	chmod 0755 /etc/init.d/agentd
fi
/etc/init.d/agentd enable
/etc/init.d/agentd restart || true
exit 0
endef

$(eval $(call BuildPackage,agentd))
$(eval $(call BuildPackage,agentd-bench))
