'use strict';
'require view';
'require form';
'require uci';
'require ui';
'require agent-router.mode as mode';
'require agent-router.mesh-setup as meshSetup';

function commaList(option, config) {
	option.cfgvalue = function(sectionId) {
		const value = uci.get(config, sectionId, this.option);
		if (Array.isArray(value)) return value;
		return String(value || '').split(',').map(v => v.trim()).filter(Boolean);
	};
	option.write = function(sectionId, value) {
		const joined = (value || []).map(v => String(v).trim()).filter(Boolean).join(',');
		if (joined) return uci.set(config, sectionId, this.option, joined);
		return uci.unset(config, sectionId, this.option);
	};
	option.remove = function(sectionId) {
		return uci.unset(config, sectionId, this.option);
	};
	return option;
}

function routerIdentifier(option) {
	option.validate = function(sectionId, value) {
		const text = String(value || '');
		if (text.length < 1 || text.length > 64 ||
			!/^[a-z0-9](?:[a-z0-9._-]*[a-z0-9])?$/.test(text))
			return _('请输入 1–64 位 Router ID：使用小写字母、数字、点、下划线或连字符，并以字母或数字开头和结尾。');
		return true;
	};
	option.placeholder = 'router-a';
	return option;
}

return view.extend({
	handleSave: function(ev) {
		return this.super('handleSave', [ev]).then(function() {
			return ui.changes.apply(false);
		});
	},


	render() {
		let m, s, o;

		m = new form.Map('agent', _('Agent Router Quick Setup'),
			_('Configure the common single-domain deployment. Advanced ARPX, DNSSEC, Relay, route and capacity controls remain available under Advanced Settings.'));
		s = m.section(form.NamedSection, 'main', 'core', _('Identity and service'));
		s.anonymous = true;

		o = s.option(form.Flag, 'enabled', _('Enable Agent routing'));
		o.rmempty = false;
		o = routerIdentifier(s.option(form.Value, 'router_id', _('Router ID（路由器标识）')));
		o.rmempty = false;
		o.description = _('例如 router-a、beijing.edge-01。保存后会向可信 Agent Router 发布此稳定身份。');
		o = s.option(form.Value, 'domain_id', _('Agent domain'));
		o.datatype = 'hostname';
		o.rmempty = false;
		o.description = _('Routers in the same domain can use zero-configuration LAN admission.');

		s = m.section(form.NamedSection, 'main', 'core', _('LAN discovery'));
		s.anonymous = true;
		o = s.option(form.Flag, 'discovery_enabled', _('Discover Agent routers on LAN'));
		o.rmempty = false;
		o = s.option(form.Flag, 'discovery_publish_enabled', _('Publish this router on LAN'));
		o.rmempty = false;
		o = s.option(form.ListValue, 'lan_auto_promotion_mode', _('LAN admission'));
		o.value('off', _('Manual approval'));
		o.value('same-domain', _('Automatically trust same-domain routers'));
		o.value('allowlist', _('Automatically trust listed routers'));
		o.rmempty = false;
		o.description = _('Manual approval keeps candidates in Peer Trust until an administrator confirms them.');
		o = commaList(s.option(form.DynamicList, 'lan_auto_promotion_allowlist', _('Router allowlist')), 'agent');
		o.depends('lan_auto_promotion_mode', 'allowlist');
		o.placeholder = 'router-branch-01';
		o.description = _('One Router ID per entry. Entries are stored in the existing bounded UCI option.');

		s = m.section(form.NamedSection, 'main', 'core', _('Open Mesh connection'));
		s.anonymous = true;
		o = s.option(form.DummyValue, '_mesh_connection', _('Mesh client'));
		o.renderWidget = function() { return meshSetup.render({ clientOnly: true }); };
		o.write = o.remove = function() {};

		return m.render().then(function(node) {
			return E([], [
				E('link', { 'rel': 'stylesheet', 'href': L.resource('agent-router/agent-router.css') + '?v=#PKG_VERSION' }),
				mode.render('developer'), node
			]);
		});
	}
});
