'use strict';
'require view';
'require form';
'require rpc';
'require uci';
'require fs';
'require ui';
'require poll';
'require dom';
'require agent-router.mode as mode';
'require agent-router.mesh-setup as meshSetup';
'require agent-router.directory-relays as directoryRelays';

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

function serviceState(payload, name) {
	if (payload == null) return { unknown: true, running: false };
	const service = (payload || {})[name] || {};
	const instances = Object.values(service.instances || {});
	const running = instances.some(instance => !!instance.running);
	const retrying = instances.find(instance => !instance.running && instance.respawn);
	const failed = instances.find(instance => Number.isInteger(instance.exit_code) && instance.exit_code !== 0);
	return {
		unknown: false,
		running,
		retrying: !running && !!retrying,
		retrySeconds: retrying ? Number(retrying.respawn.timeout) || 30 : 0,
		exitCode: !running && failed ? failed.exit_code : null
	};
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

function roleStatus(role, wanted, state) {
	if (state.unknown)
		return { label: _('Status unavailable'), tone: 'warn', next: _('Could not refresh service status. Retrying automatically; no configuration was changed.') };
	if (!wanted && state.running)
		return { label: _('Running until Save & Apply'), tone: 'warn', next: _('Apply the selected mode to stop this service.') };
	if (!wanted)
		return { label: _('Not enabled'), tone: 'info', next: '' };
	if (!state.installed)
		return { label: _('Component missing'), tone: 'down', next: _('Install %s.').format(ROLE_FILES[role].package) };
	if (!state.running && !state.configured && !state.customConfig && !state.retrying)
		return { label: _('Configuration missing'), tone: 'warn', next: _('Use Open Mesh setup to initialize Relay and Directory together. No manual JSON configuration is needed.') };
	if (state.retrying)
		return { label: _('Recovering'), tone: 'warn', next: state.exitCode != null ?
			_('Last process exit: %s. Automatic retry every %s seconds. If this persists, check the role configuration, certificate permissions and system log.').format(state.exitCode, state.retrySeconds) :
			_('Automatic restart pending (retry interval: %s seconds).').format(state.retrySeconds) };
	if (!state.running && state.exitCode != null)
		return { label: _('Recovery stopped'), tone: 'down', next: _('The process exited with code %s and automatic recovery is not armed. Save & Apply, then check the system log.').format(state.exitCode) };
	if (!state.running)
		return { label: _('Stopped'), tone: 'warn', next: _('No active supervisor. Save & Apply; check storage permissions and the system log if startup fails.') };
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
		fact(_('Runtime'), badge(state.unknown ? _('Status unavailable') : state.running ? _('Running') : state.retrying ? _('Recovering') : _('Stopped'), state.running ? 'up' : 'warn'))
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

function openMeshClientGuide() {
	return meshSetup.render();
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
			callService(ROLE_FILES.relay.service).catch(() => null),
			callService(ROLE_FILES.directory.service).catch(() => null),
			fs.stat(ROLE_FILES.relay.init).catch(() => null),
			fs.stat(ROLE_FILES.directory.init).catch(() => null),
			fs.stat(ROLE_FILES.relay.config).catch(() => null),
			fs.stat(ROLE_FILES.directory.config).catch(() => null)
		]));
	},

	render(status) {
		const roleMode = savedMode();
		const runtime = {};
		const relayConfig = uci.get('nexus_roles', 'relay', 'config_file') || ROLE_FILES.relay.config;
		const directoryConfig = uci.get('nexus_roles', 'directory', 'config_file') || ROLE_FILES.directory.config;
		const state = {
			relay: {
				installed: exists(status[2]),
				configured: exists(status[4]),
				customConfig: relayConfig !== ROLE_FILES.relay.config,
				...serviceState(status[0], ROLE_FILES.relay.service)
			},
			directory: {
				installed: exists(status[3]),
				configured: exists(status[5]),
				customConfig: directoryConfig !== ROLE_FILES.directory.config,
				...serviceState(status[1], ROLE_FILES.directory.service)
			}
		};

		return Promise.all([ modeMap().render(), relayMap().render(), directoryMap(runtime).render() ])
			.then(nodes => {
				nodes[0].classList.add('ar-role-mode');
				nodes[1].classList.add('ar-role-config-map');
				nodes[2].classList.add('ar-role-config-map');
				const summary = E('div', { 'aria-live': 'polite' }, [roleSummary(roleMode, state)]);
				// Update only status, never re-render forms or overwrite unsaved edits.
				poll.add(() => Promise.all([
					callService(ROLE_FILES.relay.service).catch(() => null),
					callService(ROLE_FILES.directory.service).catch(() => null),
					fs.stat(ROLE_FILES.relay.config).catch(() => null),
					fs.stat(ROLE_FILES.directory.config).catch(() => null)
				]).then(current => {
					const next = {};
					['relay', 'directory'].forEach((role, index) => {
						next[role] = Object.assign({}, state[role], serviceState(current[index], ROLE_FILES[role].service));
						next[role].configured = exists(current[index + 2]);
					});
					dom.content(summary, roleSummary(roleMode, next));
				}), 5);

				return E([], [
					E('link', { 'rel': 'stylesheet', 'href': L.resource('agent-router/agent-router.css') + '?v=#PKG_VERSION' }),
					mode.render('developer'),
					E('div', { 'class': 'ar-shell ar-roles-page' }, [
						openMeshClientGuide(),
						summary,
						directoryRelays.render(),
						E('details', { 'class': 'ar-advanced-panel ar-role-advanced' }, [
							E('summary', {}, _('Advanced role configuration')),
							E('p', { 'class': 'ar-muted' }, _('Only change these values when hosting Relay or Directory services on this router.')),
							nodes[0],
							E('div', { 'class': 'ar-role-advanced-grid' }, [ nodes[1], nodes[2] ])
						])
					])
				]);
			});
	}
});
