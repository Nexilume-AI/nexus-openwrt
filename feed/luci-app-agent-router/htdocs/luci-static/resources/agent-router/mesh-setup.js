'use strict';
'require baseclass';
'require rpc';
'require ui';
'require dom';
'require poll';

const status = rpc.declare({ object: 'nexus-agent-ui', method: 'mesh_status', expect: {} });
const setup = rpc.declare({ object: 'nexus-agent-ui', method: 'setup_mesh', timeout: 120000,
	params: ['action', 'join_link', 'advertised_ipv4', 'expected_generation'], expect: {} });
const share = rpc.declare({ object: 'nexus-agent-ui', method: 'share_mesh',
	params: ['paths_json', 'expected_generation'], expect: {} });
const access = rpc.declare({ object: 'session', method: 'access',
	params: ['scope', 'object', 'function'], expect: { access: false } });

function button(attrs, label) {
	const disabled = attrs.disabled;
	delete attrs.disabled;
	const node = E('button', attrs, label);
	// LuCI E() serializes boolean attributes; disabled="false" is still disabled.
	node.disabled = !!disabled;
	return node;
}

function reason(code) {
	return ({
		SEED_COMPONENTS_MISSING: _('Install the Seed profile (nexus-agent-router-seed) from the matching package feed, then check again. Relay, Directory, Node.js and OpenSSL are required.'),
		CONFIGURATION_CHANGED: _('Configuration changed or there are pending edits. Apply or discard them, then reload the setup check.'),
		SEED_IDENTITY_INCOMPLETE: _('An incomplete seed identity exists. Open Developer diagnostics to repair it; setup will not replace existing keys.'),
		CUSTOM_ROLE_CONFIGURATION: _('Custom role configuration already exists. It is preserved; use Developer mode to manage it.'),
		SEED_FIREWALL_UNAVAILABLE: _('Configure a WAN firewall zone in OpenWrt Network settings before hosting a seed.'),
		FIRMWARE_UPGRADE_REQUIRED: _('Update the Router packages to use guided Mesh setup.'),
		SEED_ADDRESS_REQUIRED: _('Enter a valid reachable IPv4 address or configure the LAN address first.'),
		INVALID_MESH_LINK: _('Paste a complete nexus-mesh://join/ link. Nexus Cloud pairing links cannot join this Mesh.'),
		ROUTER_NETWORK_DISABLED: _('Enable Router network on the home page before joining a Mesh.'),
		MESH_NOT_JOINED: _('Join a Mesh before reconnecting. No saved connection was changed.'),
		INVALID_MESH_PATHS: _('Use up to four distinct access paths with valid domain names or IP addresses and service ports.'),
		SEED_ALREADY_CONFIGURED: _('This router already hosts a seed. Its identity and join link are preserved.')
	})[code] || _('Mesh setup failed. Existing configuration was preserved or rolled back. Check service diagnostics before retrying.');
}

function validAddress(value) {
	const parts = value.split('.');
	return parts.length === 4 && parts.every(x => /^\d{1,3}$/.test(x) && Number(x) <= 255) &&
		Number(parts[0]) > 0 && Number(parts[0]) < 224 && Number(parts[0]) !== 127;
}

function linkPanel(link) {
	const field = E('textarea', { readonly: true, rows: 4, 'class': 'ar-mesh-link',
		'aria-label': _('Open Mesh join link') }, link);
	return E('div', { 'class': 'ar-shell ar-mesh-flow' }, [
		E('p', {}, _('On another router, open Agent Routing → Open Mesh setup → Join Mesh and paste this link.')),
		field,
		button({ type: 'button', 'class': 'btn', click: () => { field.focus(); field.select(); } }, _('Select link')),
		E('p', { 'class': 'ar-muted' }, _('The link contains public Mesh routing information, not Cloud credentials. Share it only with routers you intend to join this open network.')),
		E('p', { 'class': 'ar-muted' }, _('Setup verifies local TLS listeners, not Internet reachability. A seed behind upstream NAT still needs inbound forwarding or a reachable seed.'))
	]);
}

function facts(current, clientOnly) {
	const client = ({ not_joined: _('Not joined'), connecting: _('Connecting to Directory'),
		assigned: _('Relay assigned — connecting tunnel'), connected: _('Relay tunnel connected'), disconnected: _('Disconnected'), paused: _('Router network paused'), retrying: _('Connection retrying') })[current.client_state] || _('Status unavailable');
	return E('dl', { 'class': 'ar-mesh-facts' }, (clientOnly ? [] : [
		E('dt', {}, _('Hosted seed')), E('dd', {}, current.configured ? _('Seed configured') : _('Not initialized')),
		E('dt', {}, _('Relay')), E('dd', {}, current.relay_running ? _('Running') : _('Not running')),
		E('dt', {}, _('Directory')), E('dd', {}, current.directory_running ? _('Running') : _('Not running'))
	]).concat([
		E('dt', {}, _('This router as a Mesh client')), E('dd', {}, client)
	]));
}

function sharePaths(current, refresh) {
	let paths = [], busy = false, review = false, stale = false, message = '';
	try { paths = JSON.parse(current.share_paths || '[]'); } catch (_) { /* Old firmware. */ }
	if (!Array.isArray(paths)) paths = [];
	const body = E('div', { 'class': 'ar-shell ar-mesh-flow' });
	const fields = [
		['directory_host', _('Directory domain or IP'), 'text'], ['directory_port', _('Directory external port'), 'number'],
		['relay_host', _('Relay domain or IP'), 'text'], ['relay_port', _('Relay external port'), 'number']
	];
	function draw() {
		dom.content(body, [
			E('p', {}, _('These are addresses reachable by joining routers, not LuCI addresses. Empty uses the current LAN entry. Add public domain/IP and mapped ports only when needed; this does not create upstream NAT forwarding.')),
			E('p', { 'class': 'ar-muted' }, _('All paths must reach this same seed. TLS must pass through to its Relay and Directory. Changing entries preserves keys and services; existing members need the updated join link.')),
			...paths.map((path, index) => E('fieldset', { 'class': 'ar-mesh-flow' }, [
				E('legend', {}, _('Access path') + ' ' + (index + 1)),
				...fields.map(([key, label, type]) => {
					const input = E('input', {
					type, value: path[key], 'aria-label': label + ' ' + (index + 1),
					input: ev => { path[key] = type === 'number' ? Number(ev.target.value) : ev.target.value.trim().toLowerCase(); }
					});
					input.readOnly = review || busy;
					if (type === 'number') { input.min = 1; input.max = 65535; }
					return E('label', {}, [label, input]);
				}),
				!review ? button({type:'button', 'class':'btn', disabled:busy, click:() => { paths.splice(index,1); draw(); }}, _('Remove path')) : E('span')
			])),
			message ? E('p', {role:'alert'}, message) : E('span'),
			E('div', { 'class': 'ar-mesh-actions' }, [
				button({type:'button', 'class':'btn', disabled:busy, click:ui.hideModal}, _('Cancel')),
				!review && paths.length < 4 ? button({type:'button', 'class':'btn', disabled:busy, click:() => {
					paths.push({directory_host:'',directory_port:18443,relay_host:'',relay_port:17444}); draw();
				}}, _('Add access path')) : E('span'),
				review ? button({type:'button', 'class':'btn', disabled:busy, click:() => {review=false;draw();}}, _('Back')) : E('span'),
				button({type:'button', 'class':'btn cbi-button-action', disabled:busy || stale, click:async () => {
					message='';
					if (!review) {
						if (paths.some(p => fields.some(([k,,t]) => t === 'number' ? !Number.isInteger(p[k]) || p[k]<1 || p[k]>65535 : !p[k] || p[k].length>127 || /[\s/@?#%]/.test(p[k])))) message=reason('INVALID_MESH_PATHS');
						else review=true;
						draw(); return;
					}
					busy=true;draw();
					try {
						const result=await share(JSON.stringify(paths),current.generation);
						if (!result || result.ok !== true) { message=reason(result && result.code); stale=true; }
						else { ui.hideModal(); await refresh(); return; }
					} catch (_) { message=_('The setup response was interrupted. Reload checks to see whether it completed before applying again.'); stale=true; }
					finally {busy=false;}
					draw();
				}}, busy ? _('Applying and verifying…') : review ? _('Save sharing entries') : _('Review setup'))
			])
		]);
	}
	ui.showModal(_('Mesh sharing entries'), [body]); draw();
}

function changeConnection(action, current, refresh) {
	let busy = false, stale = false, message = '';
	const disconnect = action === 'disconnect';
	const body = E('div', { 'class': 'ar-shell ar-mesh-flow' });
	function draw() {
		dom.content(body, [
			E('p', {}, disconnect ? _('Disconnect this router from its Mesh? Calls using this connection will be interrupted. The saved join configuration is kept for reconnecting.') :
				_('Reconnect using the saved Mesh connection. No new join link is required.')),
			E('p', {}, _('Cloud Relay and this router’s hosted Relay and Directory remain running. LAN discovery and local Agent services are not disabled.')),
			E('p', { 'class': 'ar-muted' }, _('Applying a connection change briefly reloads Agent routing. Other routing connections reconnect automatically; their saved settings are unchanged.')),
			message ? E('p', { role: 'alert' }, message) : E('span'),
			E('div', { 'class': 'ar-mesh-actions' }, [
				button({ type: 'button', 'class': 'btn', disabled: busy, click: ui.hideModal }, _('Cancel')),
				button({ type: 'button', 'class': 'btn cbi-button-action', disabled: busy || stale, click: async () => {
					busy = true; draw();
					try {
						const result = await setup(action, '', '', current.generation);
						if (!result || result.ok !== true) { message = reason(result && result.code); stale = true; }
						else { ui.hideModal(); await refresh(); return; }
					} catch (_) {
						message = _('The setup response was interrupted. Reload checks to see whether it completed before applying again.'); stale = true;
					} finally { busy = false; }
					draw();
				} }, busy ? _('Applying and verifying…') : disconnect ? _('Disconnect Mesh') : _('Reconnect Mesh')),
				stale ? button({ type: 'button', 'class': 'btn', click: async () => { ui.hideModal(); await refresh(); } }, _('Refresh status')) : E('span')
			])
		]);
	}
	ui.showModal(disconnect ? _('Disconnect Mesh') : _('Reconnect Mesh'), [body]); draw();
}

async function open(action, refresh) {
	let current, step = 1, busy = false, needsReload = false, message = '';
	const input = E('textarea', { rows: 3, 'class': 'ar-mesh-link', 'aria-label': _('Open Mesh join link'), placeholder: 'nexus-mesh://join/…' });
	const address = E('input', { type: 'text', 'aria-label': _('Reachable seed IPv4 address'), placeholder: _('Automatic LAN address') });
	const body = E('div', { 'class': 'ar-shell ar-mesh-flow' });
	function draw() {
		const allowed = current && (action === 'join' ? current.can_join : current.can_create);
		const close = button({ type: 'button', 'class': 'btn', disabled: busy, click: ui.hideModal }, _('Cancel'));
		const next = button({ type: 'button', 'class': 'btn cbi-button-action', disabled: busy || !allowed || needsReload,
			click: async () => {
				message = '';
				if (step === 1) {
					if (action === 'join' && !/^nexus-mesh:\/\/join\/[A-Za-z0-9_-]+$/.test(input.value.trim())) message = reason('INVALID_MESH_LINK');
					if (action === 'create' && ((address.value.trim() && !validAddress(address.value.trim())) || (!address.value.trim() && !current.lan_address_available))) message = reason('SEED_ADDRESS_REQUIRED');
					if (!message) step = 2;
					draw(); return;
				}
				busy = true; draw();
				try {
					// Apply the reviewed snapshot, never silently fetch a newer generation.
					const result = await setup(action, action === 'join' ? input.value.trim() : '',
						action === 'create' ? address.value.trim() : '', current.generation);
					if (!result || result.ok === false) {
						message = reason(result && result.code); needsReload = true;
					} else {
						ui.showModal(action === 'create' ? _('Mesh seed configured') : _('Mesh connection saved'), [
							E('div', { role: 'status' }, action === 'create' ? _('Certificates and local services were verified. Share the join link next.') :
								_('Configuration saved. Directory discovery connects in the background; this is not yet proof of an end-to-end Agent connection.')),
							result.join_link ? linkPanel(result.join_link) : E('p', {}, _('Watch the Mesh client status below, then check Neighbor Routers for learned capabilities.')),
							E('div', { 'class': 'ar-mesh-actions' }, [button({ type: 'button', 'class': 'btn cbi-button-action', click: ui.hideModal }, _('Done'))])
						]);
						await refresh(); return;
					}
				} catch (error) {
					message = _('The setup response was interrupted. Reload checks to see whether it completed before applying again.');
					needsReload = true;
				} finally { busy = false; }
				draw();
			} }, busy ? _('Applying and verifying…') : step === 1 ? _('Review setup') : action === 'create' ? _('Initialize Relay & Directory') : _('Apply Mesh connection'));
		const fields = action === 'join' ? [E('label', {}, [_('Open Mesh join link'), input])] : [
			E('p', {}, _('Relay carries cross-NAT traffic. Directory introduces routers and assigns the Relay. This setup initializes both together.')),
			E('label', {}, [_('Reachable seed IPv4 address'), address]),
			E('p', { 'class': 'ar-muted' }, _('Leave empty for a LAN seed. For other networks, enter its reachable IPv4 address. Upstream NAT must forward TCP 17444 and 18443 unchanged; setup cannot configure another router.'))
		];
		dom.content(body, [
			E('ol', { 'class': 'ar-mesh-steps', 'aria-label': _('Setup progress') },
				[_('Check & configure'), _('Review'), _('Apply & verify')].map((label, i) => E('li', { 'aria-current': (busy ? 3 : step) === i + 1 ? 'step' : null }, label))),
			E('p', {}, _('Open Mesh automatically accepts peer routers. This is separate from Nexus Cloud pairing and does not grant Cloud access.')),
			!current ? E('p', { role: 'status' }, _('Checking prerequisites…')) : !allowed ? E('p', { role: 'alert' }, reason(action === 'join' ? 'ROUTER_NETWORK_DISABLED' : current.code)) :
				step === 1 ? E('div', { 'class': 'ar-mesh-flow' }, fields) : E('div', { 'class': 'ar-mesh-review' }, [
					E('h4', {}, _('Changes to apply')),
					E('p', {}, action === 'create' ? _('Create an isolated Mesh CA, configure Relay and Directory, enable both services, and open their two local firewall ports.') :
						_('Replace this router’s Mesh Directory connection with the supplied join link. Existing Cloud pairing and Cloud Relay settings are unchanged.')),
					E('p', {}, action === 'create' ? address.value.trim() || _('Automatic LAN address') : _('Use the reviewed Mesh join link')),
					E('p', {}, _('Cloud identity, Agent configuration and existing custom settings will not be overwritten.'))
					, E('p', { 'class': 'ar-muted' }, _('Applying a connection change briefly reloads Agent routing. Other routing connections reconnect automatically; their saved settings are unchanged.'))
				]),
			message ? E('p', { role: 'alert', 'class': 'alert-message error' }, message) : E('span'),
			E('div', { 'class': 'ar-mesh-actions' }, [close,
				step === 2 && !busy ? button({ type: 'button', 'class': 'btn', click: () => { step = 1; draw(); } }, _('Back')) : E('span'),
				button({ type: 'button', 'class': 'btn', disabled: busy, click: load }, _('Reload checks')), next])
		]);
	}
	async function load() {
		try { current = await status(); step = 1; needsReload = false; message = ''; }
		catch (error) { current = null; message = _('Could not check Mesh setup. Retry without changing configuration.'); }
		draw();
	}
	ui.showModal(action === 'create' ? _('Host a Mesh seed') : _('Join an Open Mesh'), [body]);
	draw(); await load();
}

return baseclass.extend({
	render: function(options) {
		const clientOnly = !!(options && options.clientOnly);
		const node = E('section', { 'class': 'ar-role-endpoint ar-mesh-setup' });
		let current, canConfigure = false, loading = false, lastSnapshot = '';
		function draw(error) {
			dom.content(node, [
				E('div', {}, [E('h3', {}, clientOnly ? _('Open Mesh connection') : _('Open Mesh setup')), E('p', { 'class': 'ar-muted' }, clientOnly ?
					_('Join using a Mesh link. Addresses and connection settings are managed automatically. Hosting Relay and Directory services is configured in Router Roles; Cloud Relay remains separate.') :
					_('Host a self-managed Relay and Directory, or join an existing Mesh. Nexus Cloud Relay is configured separately.'))]),
				error ? E('p', { role: 'alert' }, _('Could not refresh Mesh status. Existing connections were not changed.')) : E('span'),
				current ? facts(current, clientOnly) : E('p', { role: 'status' }, _('Checking prerequisites…')),
				!clientOnly && current && current.configured && (!current.relay_running || !current.directory_running) ? E('p', { role: 'status' }, _('Seed identity exists but a service is not running. Check Router Roles; displaying a join link does not mean the seed is reachable.')) : E('span'),
				current && current.client_state === 'paused' ? E('p', { role: 'status' }, reason('ROUTER_NETWORK_DISABLED')) : E('span'),
				E('div', { 'class': 'ar-mesh-actions' }, [
					canConfigure ? button({ type: 'button', 'class': 'btn', disabled: error || !current, click: () => open('join', refresh) }, current && current.client_configured ? _('Change Mesh') : _('Join Mesh')) : E('span', {}, _('Read only')),
					canConfigure && current && current.client_configured ? button({ type: 'button', 'class': 'btn', disabled: error || (!current.client_enabled && !current.can_join),
						click: () => changeConnection(current.client_enabled ? 'disconnect' : 'reconnect', current, refresh) }, current.client_enabled ? _('Disconnect') : _('Reconnect')) : E('span'),
					!clientOnly && canConfigure && current && !current.configured ? button({ type: 'button', 'class': 'btn', disabled: error, click: () => open('create', refresh) }, _('Create Mesh seed')) : E('span'),
					!clientOnly && canConfigure && current && current.join_link ? button({ type: 'button', 'class': 'btn', disabled: error, click: () => ui.showModal(_('Open Mesh join link'), [linkPanel(current.join_link),
						E('div', { 'class': 'ar-mesh-actions' }, [button({ type: 'button', 'class': 'btn', click: ui.hideModal }, _('Done'))])]) }, _('Show join link')) : E('span'),
					!clientOnly && canConfigure && current && current.configured ? button({type:'button', 'class':'btn', disabled:error, click:() => sharePaths(current,refresh)}, _('Sharing entries')) : E('span'),
					button({ type: 'button', 'class': 'btn', disabled: loading, click: refresh }, _('Refresh status'))
				])
			]);
		}
		async function refresh() {
			if (loading) return;
			loading = true;
			try {
				current = await status(); loading = false;
				const snapshot = JSON.stringify([current, canConfigure]);
				if (snapshot !== lastSnapshot) { lastSnapshot = snapshot; draw(false); }
			} catch (_) { loading = false; lastSnapshot = ''; draw(true); }
		}
		access('ubus', 'nexus-agent-ui', 'setup_mesh').then(allowed => { canConfigure = allowed; return refresh(); }).catch(() => refresh());
		poll.add(refresh, 5);
		return node;
	}
});
