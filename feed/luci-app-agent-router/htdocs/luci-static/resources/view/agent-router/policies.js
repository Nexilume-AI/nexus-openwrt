'use strict';
'require view';
'require form';
'require ui';
'require agent-router.mode as mode';

return view.extend({
	handleSave: function(ev) {
		return this.super('handleSave', [ev]).then(function() {
			return ui.changes.apply(false);
		});
	},


	render() {
		let m, s, o;
		m = new form.Map('agent_policy', _('Agent Policy RIB'),
			_('Identity-aware AFIB admission and deterministic route scoring. Rules compile atomically; a failed candidate does not replace the active policy.'));

		s = m.section(form.NamedSection, 'main', 'core', _('Defaults & health dampening'));
		s.addremove = false;
		o = s.option(form.ListValue, 'default_action', _('Default action'));
		o.value('allow', _('Allow')); o.value('deny', _('Deny')); o.rmempty = false;
		o = s.option(form.Value, 'health_failure_threshold', _('Failure threshold'));
		o.datatype = 'range(1,100)'; o.rmempty = false;
		o = s.option(form.Value, 'health_recovery_threshold', _('Recovery threshold'));
		o.datatype = 'range(1,100)'; o.rmempty = false;

		s = m.section(form.GridSection, 'policy', _('Ordered policy rules'));
		s.addremove = true;
		s.anonymous = false;
		s.nodescriptions = true;
		s.sortable = true;
		o = s.option(form.Flag, 'enabled', _('Enabled'));
		o.rmempty = false; o.default = o.enabled;
		o = s.option(form.Value, 'policy_id', _('Policy ID'));
		o.datatype = 'uciname'; o.rmempty = false;
		o = s.option(form.Value, 'priority', _('Priority'));
		o.datatype = 'uinteger'; o.rmempty = false;
		o = s.option(form.ListValue, 'action', _('Action'));
		o.value('allow', _('Allow')); o.value('deny', _('Deny')); o.rmempty = false;
		o = s.option(form.Value, 'tenant', _('Tenant'));
		o.default = '*'; o.rmempty = false;
		o = s.option(form.Value, 'source_agent', _('Source agent'));
		o.default = '*'; o.rmempty = false;
		o = s.option(form.Value, 'intent', _('Intent class'));
		o.default = '*'; o.rmempty = false;
		o = s.option(form.Value, 'required_region', _('Required region'));
		o.placeholder = 'local';
		o = s.option(form.Value, 'route_sources', _('Route sources'));
		o.placeholder = 'local,static,peer';
		o = s.option(form.Value, 'max_cost_microunits', _('Maximum cost'));
		o.datatype = 'uinteger';
		o = s.option(form.Value, 'max_latency_ms', _('Maximum latency (ms)'));
		o.datatype = 'uinteger';
		o = s.option(form.Value, 'min_trust', _('Minimum trust'));
		o.datatype = 'range(0,100)';
		o = s.option(form.Value, 'max_load_permille', _('Maximum load (‰)'));
		o.datatype = 'range(0,1000)';
		o = s.option(form.Value, 'max_hops', _('Maximum hops'));
		o.datatype = 'range(0,32)';
		o = s.option(form.Value, 'preferred_peer', _('Preferred peer'));
		o = s.option(form.Value, 'allowed_endpoint_prefix', _('Allowed endpoint prefix'));
		o.placeholder = 'https://';

		return m.render().then(function(node) {
			return E([], [
				E('link', { 'rel': 'stylesheet', 'href': L.resource('agent-router/agent-router.css') + '?v=#PKG_VERSION' }),
				mode.render('developer'), node
			]);
		});
	}
});
