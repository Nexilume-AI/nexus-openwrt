'use strict';
'require view';
'require form';
'require ui';
'require agent-router.mode as mode';

function routerIdentifier(option) {
	option.validate = function(sectionId, value) {
		const text = String(value || '');
		if (text.length < 1 || text.length > 64 ||
			!/^[a-z0-9](?:[a-z0-9._-]*[a-z0-9])?$/.test(text))
			return _('请输入 1–64 位标识：使用小写字母、数字、点、下划线或连字符，并以字母或数字开头和结尾。');
		return true;
	};
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
		m = new form.Map('agent_peers', _('Static ARPX Peers'),
			_('Explicit peer inventory. Dynamic LAN, Agent Card and Directory-managed peers are runtime state and are not written here.'));
		s = m.section(form.GridSection, 'peer', _('Configured peers'));
		s.addremove = true;
		s.anonymous = false;
		s.nodescriptions = true;

		o = s.option(form.Flag, 'enabled', _('Enabled'));
		o.rmempty = false; o.default = o.enabled;
		o = routerIdentifier(s.option(form.Value, 'peer_id', _('Peer ID')));
		o.placeholder = 'peer-router-b'; o.rmempty = false;
		o = routerIdentifier(s.option(form.Value, 'router_id', _('Router ID')));
		o.placeholder = 'router-b'; o.rmempty = false;
		o = s.option(form.Value, 'domain_id', _('Domain'));
		o.datatype = 'hostname'; o.rmempty = false;
		o = s.option(form.Value, 'endpoint', _('ARPX endpoint'));
		o.placeholder = 'https://router.example:7444/arpx/v1'; o.rmempty = false;
		o = s.option(form.Value, 'connect_ipv4', _('Underlay IPv4'));
		o.datatype = 'ip4addr';
		o = s.option(form.ListValue, 'role', _('Role'));
		o.value('peer', _('Peer')); o.value('reflector', _('Reflector')); o.value('relay', _('Relay'));
		o.rmempty = false;
		o = s.option(form.Value, 'graceful_restart_seconds', _('Restart grace'));
		o.datatype = 'range(5,300)'; o.default = '30'; o.rmempty = false;

		return m.render().then(function(node) {
			return E([], [
				E('link', { 'rel': 'stylesheet', 'href': L.resource('agent-router/agent-router.css') + '?v=#PKG_VERSION' }),
				mode.render('developer'), node
			]);
		});
	}
});
