'use strict';
'require view';
'require rpc';
'require poll';
'require dom';
'require ui';
'require agent-router.mode as mode';

const callOverview = rpc.declare({ object: 'nexus-agent-ui', method: 'overview', expect: {} });
const callSetFeature = rpc.declare({
	object: 'nexus-agent-ui', method: 'set_feature',
	params: [ 'feature', 'enabled', 'expected_generation' ], expect: {}
});
const callPairCloud = rpc.declare({
	object: 'nexus-agent-ui', method: 'pair_cloud',
	params: [ 'pairing_code', 'expected_generation' ], expect: {}
});
const callRefresh = rpc.declare({ object: 'nexus-agent-ui', method: 'refresh', expect: {} });
const callCanConfigure = rpc.declare({
	object: 'session', method: 'access', params: [ 'scope', 'object', 'function' ], expect: { access: false }
});

function statusLabel(state) {
	const labels = {
		ready: _('Ready'), needs_attention: _('Needs attention'), offline: _('Offline'),
		connecting: _('Connecting'), degraded: _('Needs attention'), disabled: _('Off'),
		unavailable: _('Unavailable'), applying: _('Applying'), verifying: _('Verifying')
	};
	return labels[state] || _('Unknown');
}

function statusTone(state) {
	if (state === 'ready') return 'up';
	if (state === 'connecting' || state === 'needs_attention' || state === 'degraded') return 'warn';
	if (state === 'disabled') return 'info';
	return 'down';
}

function badge(state) {
	return E('span', { 'class': 'ar-badge ar-badge-' + statusTone(state) }, statusLabel(state));
}

function count(value) {
	const parsed = Number(value || 0);
	return Number.isFinite(parsed) && parsed > 0 ? parsed : 0;
}

function empty(message) {
	return E('div', { 'class': 'ar-friendly-empty' }, [
		E('span', { 'class': 'ar-empty-icon', 'aria-hidden': 'true' }, '○'),
		E('span', {}, message)
	]);
}

function topologyNode(kind, title, state, detail) {
	return E('div', { 'class': 'ar-topology-node ar-topology-' + kind }, [
		E('span', { 'class': 'ar-topology-symbol', 'aria-hidden': 'true' },
			kind === 'cloud' ? '☁' : kind === 'router' ? '◆' : kind === 'neighbors' ? '◇' : '⬡'),
		E('strong', {}, title),
		E('span', { 'class': 'ar-muted' }, detail),
		badge(state)
	]);
}

function featureSwitch(feature, checked, disabled, handler) {
	return E('button', {
		'class': 'ar-feature-switch' + (checked ? ' is-on' : ''),
		'type': 'button',
		'role': 'switch',
		'aria-checked': checked ? 'true' : 'false',
		'aria-label': checked ? _('Turn off %s').format(feature) : _('Turn on %s').format(feature),
		'disabled': disabled ? true : null,
		'click': handler
	}, E('span', { 'class': 'ar-feature-switch-thumb', 'aria-hidden': 'true' }));
}

function transportLabel(value) {
	if (value === 'direct') return _('Direct IPv6');
	if (value === 'relay') return _('Cloud Relay');
	return _('Automatic connection');
}

return view.extend({
	load() {
		return Promise.all([
			callOverview(),
			callCanConfigure('ubus', 'nexus-agent-ui', 'set_feature').catch(() => false)
		]).then(data => ({ overview: data[0], canConfigure: !!data[1] }));
	},

	refresh() {
		return callOverview().then(L.bind(function(next) {
			this.payload.overview = next;
			if (this.root) dom.content(this.root, this.renderBody());
		}, this));
	},

	notifyResult(result, success) {
		if (!result || result.ok === false) {
			ui.addNotification(_('Nexus configuration was not changed'),
				E('p', {}, result && result.message ? result.message : _('The Router rejected the request.')), 'error');
			return false;
		}
		if (success) ui.addNotification(null, E('p', {}, success));
		return true;
	},

	applyFeature(feature, enabled) {
		if (this.busy) return Promise.resolve();
		this.busy = feature;
		this.refreshView();
		const generation = Number(this.payload.overview.generation || 0);
		return callSetFeature(feature, enabled, generation).then(L.bind(function(result) {
			this.notifyResult(result, enabled ? _('Feature enabled and verified.') : _('Feature disabled.'));
		}, this)).catch(L.bind(function(error) {
			ui.addNotification(_('Could not change this feature'), E('p', {}, error.message || String(error)), 'error');
		}, this)).finally(L.bind(function() {
			this.busy = '';
			return this.refresh();
		}, this));
	},

	requestFeature(feature, enabled, impact) {
		if (enabled || !impact) return this.applyFeature(feature, enabled);
		ui.showModal(_('Turn off this feature?'), [
			E('p', {}, impact),
			E('p', { 'class': 'ar-muted' }, _('Advanced settings are preserved and can be used again when the feature is enabled.')),
			E('div', { 'class': 'right' }, [
				E('button', { 'class': 'btn', 'click': ui.hideModal }, _('Cancel')), ' ',
				E('button', { 'class': 'btn cbi-button-negative', 'click': L.bind(function() {
					ui.hideModal();
					this.applyFeature(feature, false);
				}, this) }, _('Turn off'))
			])
		]);
	},

	pairCloud() {
		const code = String(this.pairingCode || '').trim();
		if (!code) {
			this.pairError = _('Enter the one-time pairing code from Nexus Cloud.');
			this.refreshView();
			return;
		}
		this.busy = 'cloud';
		this.pairError = '';
		this.refreshView();
		return callPairCloud(code, Number(this.payload.overview.generation || 0)).then(L.bind(function(result) {
			if (this.notifyResult(result, _('Pairing started. This page will update automatically.'))) {
				this.pairingCode = '';
				this.showPairing = false;
			}
		}, this)).catch(L.bind(function(error) {
			ui.addNotification(_('Cloud pairing could not start'), E('p', {}, error.message || String(error)), 'error');
		}, this)).finally(L.bind(function() {
			this.busy = '';
			return this.refresh();
		}, this));
	},

	refreshView() {
		if (this.root && this.payload)
			dom.content(this.root, this.renderBody());
	},

	renderRecommended(data) {
		const action = data.recommended_action;
		if (action === 'pair_cloud') return E('button', {
			'class': 'btn cbi-button-action important', 'click': function() {
				const input = document.getElementById('nexus-pairing-code');
				if (input) input.focus();
			}
		}, _('Pair with Nexus Cloud'));
		if (action === 'enable_router_network') return E('button', {
			'class': 'btn cbi-button-action important',
			'click': L.bind(function() { this.applyFeature('router_network', true); }, this)
		}, _('Enable Router network'));
		if (action === 'enable_agent_services') return E('button', {
			'class': 'btn cbi-button-action important',
			'click': L.bind(function() { this.applyFeature('agent_services', true); }, this)
		}, _('Enable Agent services'));
		if (action === 'check_cloud') return E('button', {
			'class': 'btn cbi-button-action important', 'type': 'button',
			'click': function() { mode.select('developer', 'cloud'); }
		}, _('Review Cloud connection'));
		if (action === 'open_developer') return E('button', {
			'class': 'btn cbi-button-action important', 'type': 'button',
			'click': function() { mode.select('developer'); }
		}, _('Open Developer mode'));
		return E('span', { 'class': 'ar-operational' }, [ E('span', { 'aria-hidden': 'true' }, '✓'), _('Operational') ]);
	},

	renderFeatureRow(title, description, feature, data, impact) {
		const applying = this.busy === feature;
		return E('div', { 'class': 'ar-feature-row' }, [
			E('div', { 'class': 'ar-feature-state' }, [
				badge(applying ? 'applying' : data.state),
				E('div', {}, [ E('strong', {}, title), E('p', { 'class': 'ar-muted' }, description) ])
			]),
			featureSwitch(title, !!data.enabled, !this.payload.canConfigure || !!this.busy,
				L.bind(function() { this.requestFeature(feature, !data.enabled, impact); }, this))
		]);
	},

	renderCloud(data) {
		const cloud = data.cloud || {};
		const paired = !!cloud.paired;
		const replaceWarning = paired ? E('p', { 'class': 'ar-inline-warning' },
			_('A new code replaces this Router’s existing Cloud enrollment.')) : '';
		return E('section', { 'class': 'ar-user-section', 'aria-labelledby': 'ar-cloud-title' }, [
			E('div', { 'class': 'ar-section-heading' }, [
				E('div', {}, [ E('h3', { 'id': 'ar-cloud-title' }, _('Nexus Cloud')), E('p', { 'class': 'ar-muted' },
					paired ? _('This Router has a managed Cloud identity.') : _('Use a one-time code from Nexus Cloud. No address or port is required.')) ]),
				E('div', { 'class': 'ar-cloud-summary' }, [
					badge(cloud.state), E('span', { 'class': 'ar-muted' }, transportLabel(cloud.transport)),
					paired && this.payload.canConfigure && !this.showPairing ? E('button', {
						'class': 'btn', 'type': 'button', 'disabled': this.busy ? true : null,
						'click': L.bind(function() { this.showPairing = true; this.refreshView(); }, this)
					}, _('Pair again')) : ''
				])
			]),
			!cloud.profile_available ? E('div', { 'class': 'alert-message warning ar-status-message' }, [
				E('strong', {}, _('Cloud profile unavailable')),
				E('div', {}, _('Update the Router firmware or configure the Cloud address once in Developer mode.'))
			]) : '',
			this.payload.canConfigure && cloud.profile_available && (!paired || cloud.state !== 'ready' || this.showPairing) ? E('div', { 'class': 'ar-pairing-row' }, [
				E('label', { 'for': 'nexus-pairing-code' }, _('One-time pairing code')),
				E('div', { 'class': 'ar-pairing-controls' }, [
					E('input', {
						'id': 'nexus-pairing-code', 'type': 'password', 'autocomplete': 'off',
						'placeholder': 'pair_…', 'value': this.pairingCode || '',
						'disabled': this.busy ? true : null,
						'input': L.bind(function(event) { this.pairingCode = event.target.value; this.pairError = ''; }, this)
					}),
					E('button', { 'class': 'btn cbi-button-action important', 'type': 'button',
						'disabled': this.busy ? true : null, 'click': L.bind(this.pairCloud, this) },
						this.busy === 'cloud' ? _('Pairing…') : _('Pair with Nexus Cloud'))
				]),
				this.pairError ? E('div', { 'class': 'cbi-value-error', 'role': 'alert' }, this.pairError) : '',
				replaceWarning
			]) : '',
			E('div', { 'class': 'ar-friendly-facts' }, [
				E('div', {}, [ E('span', { 'class': 'ar-muted' }, _('Cloud Agents')), E('strong', {}, String(count(cloud.registered_agents))) ]),
				E('div', {}, [ E('span', { 'class': 'ar-muted' }, _('Connection')), E('strong', {}, transportLabel(cloud.transport)) ])
			])
		]);
	},

	renderNeighbors(data) {
		const neighbors = data.neighbors || [];
		return E('section', { 'class': 'ar-user-section', 'aria-labelledby': 'ar-neighbor-title' }, [
			E('div', { 'class': 'ar-section-heading' }, [
				E('div', {}, [ E('h3', { 'id': 'ar-neighbor-title' }, _('Neighbor Routers')), E('p', { 'class': 'ar-muted' }, _('Routers discovered by the distributed Agent network.')) ]),
				E('strong', {}, _('%s online').format(neighbors.filter(item => item.state === 'ready').length))
			]),
			neighbors.length ? E('div', { 'class': 'ar-friendly-list' }, neighbors.map(function(item) {
				return E('div', { 'class': 'ar-friendly-list-row' }, [
					E('span', { 'class': 'ar-list-symbol', 'aria-hidden': 'true' }, '◇'),
					E('div', {}, [ E('strong', {}, item.name || item.router || _('Neighbor Router')),
						E('span', { 'class': 'ar-muted' }, _('%s capabilities').format(count(item.capabilities))) ]),
					badge(item.state)
				]);
			})) : empty(data.router_network && data.router_network.enabled
				? _('No Neighbor Router is currently online.') : _('Enable Router network to discover nearby Routers.'))
		]);
	},

	renderAgents(data) {
		const agents = data.agents || [];
		return E('section', { 'class': 'ar-user-section', 'aria-labelledby': 'ar-agent-title' }, [
			E('div', { 'class': 'ar-section-heading' }, [
				E('div', {}, [ E('h3', { 'id': 'ar-agent-title' }, _('Agents')), E('p', { 'class': 'ar-muted' }, _('Agents registered on this Router through the Python SDK.')) ]),
				E('strong', {}, _('%s registered').format(agents.length))
			]),
			agents.length ? E('div', { 'class': 'ar-friendly-list' }, agents.map(function(item) {
				return E('div', { 'class': 'ar-friendly-list-row' }, [
					E('span', { 'class': 'ar-list-symbol', 'aria-hidden': 'true' }, '⬡'),
					E('div', {}, [ E('strong', {}, item.name || _('Agent')),
						E('span', { 'class': 'ar-muted' }, _('%s capabilities').format(count(item.capabilities))) ]),
					badge(item.state)
				]);
			})) : empty(data.agent_services && data.agent_services.enabled
				? _('No Agent is currently registered.') : _('Enable Agent services to accept zero-configuration SDK registration.'))
		]);
	},

	renderRoutes(data) {
		const routes = data.routes || [];
		const sourceLabel = { local: _('Local'), neighbor: _('Neighbor'), cloud: _('Cloud') };
		return E('section', { 'class': 'ar-user-section ar-route-catalog', 'aria-labelledby': 'ar-route-title' }, [
			E('div', { 'class': 'ar-section-heading' }, [
				E('div', {}, [ E('h3', { 'id': 'ar-route-title' }, _('Agent capabilities')), E('p', { 'class': 'ar-muted' }, _('Callable functions available through this Router.')) ]),
				E('strong', {}, _('%s available').format(routes.filter(item => item.state === 'ready').length))
			]),
			routes.length ? E('div', { 'class': 'ar-capability-catalog', 'role': 'list' }, routes.map(function(item) {
				return E('div', { 'class': 'ar-capability-item', 'role': 'listitem' }, [
					E('div', {}, [ E('strong', {}, item.capability || _('Capability')), E('span', { 'class': 'ar-muted' }, item.agent || _('Agent')) ]),
					E('div', { 'class': 'ar-capability-path' }, [
						E('span', {}, sourceLabel[item.source] || _('Local')),
						item.via ? E('span', { 'class': 'ar-muted' }, _('via %s').format(item.via)) : ''
					]),
					badge(item.state)
				]);
			})) : empty(_('No callable Agent capability is currently available.'))
		]);
	},

	renderBody() {
		const data = this.payload.overview || {};
		const cloud = data.cloud || { state: 'offline' };
		const network = data.router_network || { enabled: false, state: 'disabled', neighbors: 0, routes: 0 };
		const agents = data.agent_services || { enabled: false, state: 'disabled', agents: 0 };
		return [
			mode.render('user'),
			E('section', { 'class': 'ar-user-hero' }, [
				E('div', {}, [ E('span', { 'class': 'ar-eyebrow' }, _('Nexus Agent Network')),
					E('h2', {}, _('Your Router at a glance')),
					E('p', {}, _('Connect to Nexus Cloud, discover Neighbor Routers and make Agent capabilities available without configuring addresses or ports.')) ]),
				E('div', { 'class': 'ar-overall-state' }, [ badge(data.overall || 'offline'), this.payload.canConfigure ? this.renderRecommended(data) : E('span', { 'class': 'ar-muted' }, _('Read only')) ])
			]),
			E('section', { 'class': 'ar-topology', 'aria-label': _('Agent network status') }, [
				topologyNode('cloud', _('Nexus Cloud'), cloud.state, cloud.paired ? transportLabel(cloud.transport) : _('Not paired')),
				E('span', { 'class': 'ar-topology-link', 'aria-hidden': 'true' }),
				topologyNode('router', _('This Router'), network.state, _('%s routes').format(count(network.routes))),
				E('span', { 'class': 'ar-topology-link', 'aria-hidden': 'true' }),
				topologyNode('neighbors', _('Neighbor Routers'), network.enabled ? 'ready' : 'disabled', _('%s connected').format(count(network.neighbors))),
				E('span', { 'class': 'ar-topology-link', 'aria-hidden': 'true' }),
				topologyNode('agents', _('Agents'), agents.state, _('%s registered').format(count(agents.agents)))
			]),
			E('section', { 'class': 'ar-feature-panel', 'aria-labelledby': 'ar-feature-title' }, [
				E('div', { 'class': 'ar-section-heading' }, [ E('div', {}, [ E('h3', { 'id': 'ar-feature-title' }, _('Network features')), E('p', { 'class': 'ar-muted' }, _('Simple controls use the Router’s saved secure defaults.')) ]) ]),
				this.renderFeatureRow(_('Cloud connection'), _('Synchronize eligible Agents with Nexus Cloud.'), 'cloud', cloud,
					count(cloud.registered_agents) ? _('%s Cloud Agent registrations will go offline until this connection is enabled again.').format(count(cloud.registered_agents)) : ''),
				this.renderFeatureRow(_('Router network'), _('Discover Neighbor Routers and exchange Agent capability routes.'), 'router_network', network,
					count(network.neighbors) ? _('%s Neighbor Router connections will be stopped.').format(count(network.neighbors)) : ''),
				this.renderFeatureRow(_('Agent services'), _('Allow zero-configuration SDK registration and Agent invocation.'), 'agent_services', agents,
					count(agents.agents) ? _('%s registered Agents will no longer be callable.').format(count(agents.agents)) : '')
			]),
			this.renderCloud(data),
			E('div', { 'class': 'ar-user-columns' }, [ this.renderNeighbors(data), this.renderAgents(data) ]),
			this.renderRoutes(data),
			E('div', { 'class': 'ar-user-footer' }, [
				E('span', { 'class': 'ar-muted' }, _('Updated %s').format(data.updated_at || '—')),
				E('button', { 'class': 'btn', 'type': 'button', 'disabled': this.busy ? true : null, 'click': L.bind(function() {
					this.busy = 'refresh'; this.refreshView();
					const request = this.payload.canConfigure ? callRefresh() : callOverview();
					request.then(L.bind(function(next) { this.payload.overview = next; }, this))
						.catch(() => {}).finally(L.bind(function() { this.busy = ''; this.refreshView(); }, this));
				}, this) }, this.busy === 'refresh' ? _('Refreshing…') : _('Refresh status')),
				E('button', { 'class': 'btn', 'type': 'button',
					'click': function() { mode.select('developer'); } }, _('Technical details'))
			])
		];
	},

	render(payload) {
		mode.enterUser();
		this.payload = payload;
		this.busy = '';
		this.pairError = '';
		this.showPairing = false;
		this.root = E('div', { 'class': 'cbi-map ar-shell ar-user-page' }, this.renderBody());
		poll.add(L.bind(function() {
			if (this.busy || this.pairingCode) return Promise.resolve();
			return this.refresh();
		}, this), 5);
		return E([], [
			E('link', { 'rel': 'stylesheet', 'href': L.resource('agent-router/agent-router.css') + '?v=#PKG_VERSION' }),
			this.root
		]);
	},

	handleSaveApply: null,
	handleSave: null,
	handleReset: null
});
