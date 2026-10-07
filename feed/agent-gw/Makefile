include $(TOPDIR)/rules.mk

PKG_NAME:=agent-gw
PKG_VERSION:=0.22.0
PKG_RELEASE:=29

PKG_LICENSE:=LicenseRef-Nexus-Additional-Terms-1.0
PKG_MAINTAINER:=Nexus OpenWrt Team
PKG_BUILD_DIR:=$(BUILD_DIR)/$(PKG_NAME)-$(PKG_VERSION)

include $(INCLUDE_DIR)/package.mk

define Package/agent-gw
  SECTION:=net
  CATEGORY:=Network
  SUBMENU:=Web Servers/Proxies
  TITLE:=Nexus Agent controlled data-plane gateway
  DEPENDS:=+agentd +libuhttpd-nossl +libjson-c +libmbedtls +libev \
	+curl +ca-bundle +libustream-mbedtls +stunnel
  USERID:=agent-gw=454:agent-gw=454 agent-edge=459:agent-edge=459
endef

define Package/agent-gw/description
 Nexus Agent data-plane gateway. This version provides a bounded loopback
 HTTP ingress and opt-in invocation endpoints backed by agentd IPC,
 with RS256 JWT/JWKS, transaction-token replay protection and an optional
 stunnel mTLS edge when the optional stunnel package is installed. P2.6
 invocation is restricted to numeric loopback HTTP
 backends and rejects redirects, chunked responses and unbounded bodies.
 P2.7.3 adds an opt-in bounded SSE relay with disconnect propagation. P2.7.4
 adds opt-in TLS 1.3 remote backends with exact DNS-identity-to-IPv4 pinning.
 P2.7.5 adds opt-in bounded candidate-route retry for explicitly idempotent
 requests with an idempotency key and remaining deadline budget.
 P4.4 sends AFIB-selected Relay routes to agentd over bounded IPC. P4.5 adds
 validated SSE START/DATA/END delivery, idle bounds and disconnect
 cancellation across the same outbound Relay tunnel. P6.1 replaces a verified
 ingress JWT with a short-lived RS256 Router Forwarding Assertion bound to
 target, identity, task and exact Envelope digest.
 P6.3 consumes agentd Policy RIB admission metadata and enforces bounded
 per-tenant concurrency and token-bucket rate limits before Invoke dispatch.
 P8.1 adds opt-in Agent Access Proxy register, renew and unregister endpoints;
 agentd remains the sole ARIB/AFIB writer through credential-checked IPC.
 P8.4 preserves optional target_agent addressing through lookup, candidate
 retry, local backend forwarding and outbound Relay Invoke.
 P8.9 retains bounded authenticated task-to-route bindings and pins every
 resume request to the original AFIB route; it never fails over a live task to
 a different Agent where the operation could execute twice.
 P8.12.2 accepts sanitized stunnel PROXY v1 metadata through a bounded local
 bridge and forces HTTP/SSE, MCP and A2A invokes onto the /128-bound route.
 P8.14 optionally returns a configured TLS identity, ingress port and CA bundle
 identifier with every router-managed public IPv6 registration.
 P8.14.1 adds an explicit cleartext HTTP over IPv6 ingress mode. It retains JWT
 verification and exact /128 route binding, and never downgrades from TLS.
 P8.20 publishes bounded, read-only authentication metadata so Python SDKs can
 discover the trusted issuer, audience and operation scopes without exposing
 key paths, cached JWKS material or access tokens.
 P8.21 adds an explicit No JWT mode with a bounded LAN HTTP bridge and permits
 the existing Relay and public /128 invoke paths to use the same anonymous
 gateway policy without weakening JWT mode.
 P8.26 accepts SDK-reported HTTPS Agent identities into a bounded route-bound
 mapping table, renews and removes mappings with Agent leases, verifies the
 configured CA bundle plus the leaf certificate SHA-256, and retains static
 mappings only as an advanced compatibility path.
 P8.27 adds a loopback-only, non-streaming internal invoke endpoint for
 agentd-selected Relay targets. It authenticates a one-way token verifier,
 pins the exact route, endpoint and target to an active dynamic TLS lease,
 and never enters the normal JWT lookup/retry or Relay candidate paths.
endef

define Package/agent-gw/conffiles
/etc/config/agent_gateway
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
		-o $(PKG_BUILD_DIR)/agent-edge-bridge \
		$(PKG_BUILD_DIR)/agent_edge_bridge.c \
		$(PKG_BUILD_DIR)/agent_edge_proxy.c \
		$(PKG_BUILD_DIR)/agent_public_ingress.c \
		$(PKG_BUILD_DIR)/agent_public_ipv6.c \
		$(TARGET_LDFLAGS)
	$(TARGET_CC) \
		$(TARGET_CPPFLAGS) $(TARGET_CFLAGS) \
		-std=c17 -D_GNU_SOURCE -Wall -Wextra \
		-I$(PKG_BUILD_DIR) \
		-o $(PKG_BUILD_DIR)/agent-gw \
		$(PKG_BUILD_DIR)/agent_gateway.c \
		$(PKG_BUILD_DIR)/agent_lan_session.c \
		$(PKG_BUILD_DIR)/agent_forwarding_assertion.c \
		$(PKG_BUILD_DIR)/agent_jwt_verifier.c \
		$(PKG_BUILD_DIR)/agent_jwks.c \
		$(PKG_BUILD_DIR)/agent_auth_policy.c \
		$(PKG_BUILD_DIR)/agent_replay_cache.c \
		$(PKG_BUILD_DIR)/agent_stream_resume_cache.c \
		$(PKG_BUILD_DIR)/agent_public_ingress.c \
		$(PKG_BUILD_DIR)/agent_gateway_contract.c \
		$(PKG_BUILD_DIR)/agent_invoke_contract.c \
		$(PKG_BUILD_DIR)/agent_sse_contract.c \
		$(PKG_BUILD_DIR)/agent_tls_client.c \
		$(PKG_BUILD_DIR)/agent_ipc_protocol.c \
		$(PKG_BUILD_DIR)/agent_rate_limiter.c \
		$(PKG_BUILD_DIR)/agent_tenant_quota.c \
		$(TARGET_LDFLAGS) -luhttpd -lev -ljson-c \
		-lmbedtls -lmbedx509 -lmbedcrypto
	$(TARGET_CC) \
		$(TARGET_CPPFLAGS) $(TARGET_CFLAGS) \
		-std=c17 -D_GNU_SOURCE -Wall -Wextra \
		-I$(PKG_BUILD_DIR) \
		-o $(PKG_BUILD_DIR)/agent-jwks-validate \
		$(PKG_BUILD_DIR)/agent_jwks_validate.c \
		$(PKG_BUILD_DIR)/agent_jwks.c \
		$(PKG_BUILD_DIR)/agent_jwt_verifier.c \
		$(PKG_BUILD_DIR)/agent_auth_policy.c \
		$(PKG_BUILD_DIR)/agent_gateway_contract.c \
		$(PKG_BUILD_DIR)/agent_ipc_protocol.c \
		$(TARGET_LDFLAGS) -ljson-c -lmbedcrypto
endef

define Package/agent-gw/install
	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_DIR) $(1)/usr/libexec
	$(INSTALL_BIN) ./files/lan-auth.sh $(1)/usr/libexec/nexus-agent-lan-auth.sh
	$(INSTALL_BIN) ./files/nexus-agent-ingress-firewall $(1)/usr/libexec/nexus-agent-ingress-firewall
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/agent-gw $(1)/usr/sbin/agent-gw
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/agent-edge-bridge $(1)/usr/sbin/agent-edge-bridge

	$(INSTALL_BIN) $(PKG_BUILD_DIR)/agent-jwks-validate $(1)/usr/sbin/agent-jwks-validate
	$(INSTALL_BIN) ./files/agent-jwks-update $(1)/usr/sbin/agent-jwks-update
	$(INSTALL_BIN) ./files/agent-jwks-refresh $(1)/usr/sbin/agent-jwks-refresh
	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_BIN) ./files/agent-gw.init $(1)/etc/init.d/agent-gw

	$(INSTALL_BIN) ./files/agent-jwks.init $(1)/etc/init.d/agent-jwks
	$(INSTALL_BIN) ./files/agent-edge.init $(1)/etc/init.d/agent-edge
	$(INSTALL_DIR) $(1)/etc/config
	$(INSTALL_CONF) ./files/agent-gw.config $(1)/etc/config/agent_gateway

	$(INSTALL_DIR) $(1)/etc/agent-gw

	$(INSTALL_DIR) $(1)/lib/upgrade/keep.d
	$(INSTALL_DATA) ./files/agent-gw.keep $(1)/lib/upgrade/keep.d/agent-gw
endef

define Package/agent-gw/postinst
#!/bin/sh
[ -n "$${IPKG_INSTROOT}" ] && exit 0
if [ ! -s /etc/config/agent_gateway ] && [ -s /etc/config/agent_gateway.apk-new ]; then
	cp /etc/config/agent_gateway.apk-new /etc/config/agent_gateway
	chmod 0600 /etc/config/agent_gateway
fi
for name in agent-gw agent-jwks agent-edge; do
	if [ -s "/etc/init.d/$$name.apk-new" ]; then
		cp -p "/etc/init.d/$$name" "/etc/init.d/$$name.pre-nexus-upgrade" 2>/dev/null || true
		mv "/etc/init.d/$$name.apk-new" "/etc/init.d/$$name"
		chmod 0755 "/etc/init.d/$$name"
	fi
done
/etc/init.d/agent-gw enable
/etc/init.d/agent-edge enable
/etc/init.d/agent-gw restart || true
/etc/init.d/agent-edge restart || true
exit 0
endef

$(eval $(call BuildPackage,agent-gw))
