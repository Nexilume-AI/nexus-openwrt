include $(TOPDIR)/rules.mk

PKG_NAME:=nexus-luci-theme-profile
PKG_VERSION:=1.0.0
PKG_RELEASE:=2
PKG_LICENSE:=LicenseRef-Nexus-Additional-Terms-1.0
PKG_MAINTAINER:=Nexus OpenWrt Team

include $(INCLUDE_DIR)/package.mk

define Package/nexus-luci-theme-profile
  SECTION:=luci
  CATEGORY:=LuCI
  SUBMENU:=6. Themes
  TITLE:=Nexus LuCI theme selection and fallback profile
  DEPENDS:=+luci-theme-argon +luci-theme-bootstrap
  PKGARCH:=all
endef

define Package/nexus-luci-theme-profile/description
 Selects the externally built Argon theme on first installation while retaining
 the OpenWrt Bootstrap theme as a boot-time and package-removal fallback.
endef

define Build/Compile
endef

define Package/nexus-luci-theme-profile/install
	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) ./files/nexus-theme-guard $(1)/usr/sbin/nexus-theme-guard
	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_BIN) ./files/nexus-theme-guard.init $(1)/etc/init.d/nexus-theme-guard
	$(INSTALL_DIR) $(1)/etc/uci-defaults
	$(INSTALL_BIN) ./files/99-nexus-luci-theme-profile $(1)/etc/uci-defaults/99-nexus-luci-theme-profile
endef

define Package/nexus-luci-theme-profile/postinst
#!/bin/sh
[ -n "$${IPKG_INSTROOT}" ] || /usr/sbin/nexus-theme-guard --install || true
exit 0
endef

define Package/nexus-luci-theme-profile/prerm
#!/bin/sh
[ -n "$${IPKG_INSTROOT}" ] || /usr/sbin/nexus-theme-guard --fallback || true
exit 0
endef

$(eval $(call BuildPackage,nexus-luci-theme-profile))
