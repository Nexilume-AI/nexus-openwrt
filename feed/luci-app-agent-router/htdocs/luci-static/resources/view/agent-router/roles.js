'use strict';
'require view';
'require form';
'require rpc';
'require uci';
'require fs';
'require ui';
'require agent-router.mode as mode';

const callService = rpc.declare({ object: 'service', method: 'list', params: [ 'name' ], expect: {} });

const ROLE_FILES = {
	relay: {
		service: 'nexus-relayd',
		package: 'nexus-agent-relayd',
		init: '/etc/init.d/nexus-relayd',
		config: '/etc/nexus-relayd/relay.json'
	},
	directory: {
		service: 'nexus-directoryd',
		package: 'nexus-agent-directoryd',
		init: '/etc/init.d/nexus-directoryd',
		config: '/etc/nexus-directoryd/directory.json'
	}
};

function serviceRunning(payload, name) {
	const service = (payload || {})[name] || {};
	const instances = service.instances || {};
	return Object.keys(instances).some(key => !!instances[key].running);
}

function exists(result) {
	return !!result && result.type === 'file';
}

function badge(label, state) {
	return E('span', { 'class': 'ar-badge ar-badge-' + state }, label);
}

function selected(mode, role) {
	return mode === 'all' || mode === 'node-' + role;
}

function savedMode() {
	const configured = uci.get('nexus_roles', 'main', 'mode');
	if (configured) return configured;
	const relay = uci.get('nexus_roles', 'relay', 'enabled') === '1';
	const directory = uci.get('nexus_roles', 'directory', 'enabled') === '1';
	if (relay && directory) return 'all';
	if (relay) return 'node-relay';
	if (directory) return 'node-directory';
	return 'node';
}

function directoryRuntimeConfig(content) {
	try {
		const parsed = JSON.parse(String(content || ''));
		const port = Number(parsed.port);
		return {
			listen: String(parsed.listen || ''),
			port: Number.isInteger(port) && port > 0 && port <= 65535 ? port : 8443
		};
	}
	catch (e) {
		return { listen: '', port: 8443 };
	}
}

function roleStatus(role, wanted, state) {
	if (!wanted && state.running)
		return { label: _('Running until Save & Apply'), tone: 'warn', next: _('Apply the selected mode to stop this service.') };
	if (!wanted)
		return { label: _('Not enabled'), tone: 'info', next: '' };
	if (!state.installed)
		return { label: _('Component missing'), tone: 'down', next: _('Install %s.').format(ROLE_FILES[role].package) };
	if (!state.configured)
		return { label: _('Configuration missing'), tone: 'warn', next: _('Add the role configuration file, then apply again.') };
	if (!state.running)
		return { label: _('Ready to start'), tone: 'warn', next: _('Save & Apply, then check the system log if it remains stopped.') };
	return { label: _('Ready'), tone: 'up', next: _('Service is running with the saved role configuration.') };
}

function fact(label, value) {
	return E('div', { 'class': 'ar-role-fact' }, [
		E('span', { 'class': 'ar-muted' }, label),
		value
	]);
}

function roleCard(role, title, description, mode, state) {
	const wanted = selected(mode, role);
	const status = roleStatus(role, wanted, state);
	const details = wanted || state.running ? E('div', { 'class': 'ar-role-facts' }, [
		fact(_('Component'), state.installed ? badge(_('Installed'), 'up') : badge(_('Not installed'), wanted ? 'down' : 'info')),
		fact(_('Configuration'), state.customConfig ? badge(_('Custom file'), 'info') :
			(state.configured ? badge(_('Default file found'), 'up') : badge(_('File missing'), wanted ? 'warn' : 'info'))),
		fact(_('Runtime'), state.running ? badge(_('Running'), 'up') : badge(_('Stopped'), wanted ? 'warn' : 'info'))
	]) : E([], []);

	return E('div', { 'class': 'ar-role-row' + (wanted ? ' ar-role-row-enabled' : '') }, [
		E('div', { 'class': 'ar-role-card-head' }, [
			E('div', {}, [ E('h3', {}, title), E('p', { 'class': 'ar-muted' }, description) ]),
			badge(status.label, status.tone)
		]),
		details,
		status.next ? E('p', { 'class': 'ar-role-next' }, status.next) : E([], [])
	]);
}

function roleSummary(mode, state) {
	return E('section', { 'class': 'ar-role-summary ar-role-surface' }, [
		E('div', { 'class': 'ar-section-heading' }, [
			E('div', {}, [
				E('h2', {}, _('Role status')),
				E('p', { 'class': 'ar-muted' }, _('Current state from the last saved operating mode.'))
			])
		]),
		E('div', { 'class': 'ar-role-status-list' }, [
			E('div', { 'class': 'ar-role-row ar-role-row-node' }, [
				E('div', { 'class': 'ar-role-card-head' }, [
					E('div', {}, [
						E('h3', {}, _('Node')),
						E('p', { 'class': 'ar-muted' }, _('Routes local and remote Agent capabilities. Always enabled.'))
					]),
					badge(_('Active'), 'up')
				])
			]),
			roleCard('relay', _('Relay'), _('Carries cross-NAT Agent traffic for other routers.'), mode, state.relay),
			roleCard('directory', _('Directory'), _('Assigns trusted Relays to routers.'), mode, state.directory)
		])
	]);
}

function directoryConnectionGuide(mode, runtime) {
	if (!selected(mode, 'directory')) return E([], []);
	const hostname = String(uci.get('nexus_roles', 'directory', 'public_hostname') || '');
	const port = String(uci.get('nexus_roles', 'directory', 'public_port') || runtime.port || 8443);
	const endpoint = hostname ? 'https://' + hostname + ':' + port + '/v1/open-mesh/assignment' : '';

	return E('section', { 'class': 'ar-role-endpoint' }, [
		E('div', {}, [
			E('h3', {}, _('Directory address for other routers')),
			endpoint ? E('code', { 'class': 'ar-code-wrap' }, endpoint) :
				E('span', { 'class': 'ar-muted' }, _('Add the public hostname in Advanced role configuration, then Save & Apply.'))
		]),
		E('div', { 'class': 'ar-role-listener' }, [
			E('span', { 'class': 'ar-muted' }, _('Detected local listener')),
			E('strong', {}, '%s:%s'.format(runtime.listen || '0.0.0.0', runtime.port))
		])
	]);
}

function openMeshClientGuide() {
	return E('section', { 'class': 'ar-role-endpoint' }, [
		E('div', {}, [
			E('h3', {}, _('Connect this router to an Open Mesh Relay')),
			E('p', { 'class': 'ar-muted' }, [
				_('A managed seed connects automatically when Router network is enabled. To use a custom OpenWrt Directory seed, keep Node only and configure its assignment URL; the Directory selects the Relay automatically.')
			])
		]),
		E('a', {
			'class': 'btn cbi-button-action',
			'href': L.url('admin/status/agent-router/developer/settings')
		}, _('Configure a custom seed'))
	]);
}

function modeMap() {

	let m, s, o;
	m = new form.Map('nexus_roles', _('Router Roles'),
		_('Choose which services this router hosts. Node routing always remains enabled.'));
	s = m.section(form.NamedSection, 'main', 'profile', _('Operating mode'));
	s.addremove = false;
	o = s.option(form.ListValue, 'mode', _('Hosted services'));
	o.rmempty = false;
	o.default = 'node';
	o.value('node', _('Node only (recommended for normal LAN routers)'));
	o.value('node-relay', _('Node + Relay'));
	o.value('node-directory', _('Node + Directory'));
	o.value('all', _('All roles'));
	o.description = _('Node only is recommended for normal LAN routers. Add Relay for cross-NAT traffic or Directory to assign trusted Relays.');
	return m;
}

function relayMap() {
	let m, s, o;
	m = new form.Map('nexus_roles', _('Relay'));
	s = m.section(form.NamedSection, 'relay', 'role', _('Relay service file'));
	s.addremove = false;
	o = s.option(form.Value, 'config_file', _('Configuration file'));
	o.rmempty = false;
	o.placeholder = ROLE_FILES.relay.config;
	o.description = _('Keep %s unless you maintain a custom Relay JSON.').format(ROLE_FILES.relay.config);
	return m;
}

function directoryMap(runtime) {
	let m, s, o;
	m = new form.Map('nexus_roles', _('Directory'));
	s = m.section(form.NamedSection, 'directory', 'role', _('Address shared with other routers'));
	s.addremove = false;
	o = s.option(form.Value, 'public_hostname', _('Public hostname'));
	o.datatype = 'hostname';
	o.placeholder = 'directory.example.com';
	o.description = _('Use the hostname covered by the Directory TLS certificate.');
	o = s.option(form.Value, 'public_port', _('Public port'));
	o.datatype = 'port';
	o.default = String(runtime.port);
	o.description = _('External port reachable by other routers.');
	o = s.option(form.Value, 'config_file', _('Configuration file'));
	o.rmempty = false;
	o.placeholder = ROLE_FILES.directory.config;
	o.description = _('Keep %s unless you maintain a custom Directory JSON.').format(ROLE_FILES.directory.config);
	return m;
}

return view.extend({
	handleSave: function(ev) {
		return this.super('handleSave', [ev]).then(function() {
			return ui.changes.apply(false);
		});
	},


	load() {
		return uci.load('nexus_roles').then(() => Promise.all([
			callService(ROLE_FILES.relay.service).catch(() => ({})),
			callService(ROLE_FILES.directory.service).catch(() => ({})),
			fs.stat(ROLE_FILES.relay.init).catch(() => null),
			fs.stat(ROLE_FILES.directory.init).catch(() => null),
			fs.stat(ROLE_FILES.relay.config).catch(() => null),
			fs.stat(ROLE_FILES.directory.config).catch(() => null),
			fs.read(ROLE_FILES.directory.config).catch(() => '')
		]));
	},

	render(status) {
		const roleMode = savedMode();
		const runtime = directoryRuntimeConfig(status[6]);
		const relayConfig = uci.get('nexus_roles', 'relay', 'config_file') || ROLE_FILES.relay.config;
		const directoryConfig = uci.get('nexus_roles', 'directory', 'config_file') || ROLE_FILES.directory.config;
		const state = {
			relay: {
				installed: exists(status[2]),
				configured: exists(status[4]),
				customConfig: relayConfig !== ROLE_FILES.relay.config,
				running: serviceRunning(status[0], ROLE_FILES.relay.service)
			},
			directory: {
				installed: exists(status[3]),
				configured: exists(status[5]),
				customConfig: directoryConfig !== ROLE_FILES.directory.config,
				running: serviceRunning(status[1], ROLE_FILES.directory.service)
			}
		};

		return Promise.all([ modeMap().render(), relayMap().render(), directoryMap(runtime).render() ])
			.then(nodes => {
				nodes[0].classList.add('ar-role-mode');
				nodes[1].classList.add('ar-role-config-map');
				nodes[2].classList.add('ar-role-config-map');

				return E([], [
					E('link', { 'rel': 'stylesheet', 'href': L.resource('agent-router/agent-router.css') + '?v=#PKG_VERSION' }),
					mode.render('developer'),
					E('div', { 'class': 'ar-shell ar-roles-page' }, [
						nodes[0],
						openMeshClientGuide(),
						roleSummary(roleMode, state),
						directoryConnectionGuide(roleMode, runtime),
						E('details', { 'class': 'ar-advanced-panel ar-role-advanced' }, [
							E('summary', {}, _('Advanced role configuration')),
							E('p', { 'class': 'ar-muted' }, _('Only change these values when hosting Relay or Directory services on this router.')),
							E('div', { 'class': 'ar-role-advanced-grid' }, [ nodes[1], nodes[2] ])
						])
					])
				]);
			});
	}
});
