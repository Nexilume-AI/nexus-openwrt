'use strict';
'require view';
'require rpc';
'require poll';
'require dom';
'require agent-router.mode as mode';

const LIST_LIMIT = 200;
const ADVANCED_KEY = 'nexus.agentRouter.advanced';
const callRoutes = rpc.declare({ object: 'agent', method: 'routes', params: [ 'limit' ], expect: {} });

function badge(ok) {
	return E('span', { 'class': 'ar-badge ' + (ok ? 'ar-badge-up' : 'ar-badge-down') }, ok ? _('Healthy') : _('Unavailable'));
}

function advancedDefault() {
	try { return window.localStorage.getItem(ADVANCED_KEY) === '1'; }
	catch (error) { return false; }
}

function saveAdvanced(value) {
	try { window.localStorage.setItem(ADVANCED_KEY, value ? '1' : '0'); }
	catch (error) { /* Browser storage is optional. */ }
}

function routePath(route) {
	const path = route.path_vector || route.path || [];
	return path.length ? path.join(' → ') : _('Local endpoint');
}

function details(route) {
	return E('details', { 'class': 'ar-route-details' }, [
		E('summary', {}, _('Inspect path and raw metadata')),
		E('dl', { 'class': 'ar-detail-grid' }, [
			E('dt', {}, _('Route ID')), E('dd', {}, E('code', { 'class': 'ar-code-wrap' }, route.route_id || '—')),
			E('dt', {}, _('Source')), E('dd', {}, route.source || '—'),
			E('dt', {}, _('Next hop')), E('dd', {}, route.learned_from_peer || _('Local Agent')),
			E('dt', {}, _('Path')), E('dd', {}, routePath(route)),
			E('dt', {}, _('Endpoint')), E('dd', { 'class': 'ar-code-wrap' }, route.endpoint || '—'),
			E('dt', {}, _('Lease')), E('dd', {}, route.remaining_ms === undefined ? '—' : _('%s seconds remaining').format(Math.max(0, Math.round(Number(route.remaining_ms) / 1000))))
		]),
		E('details', { 'class': 'ar-raw' }, [ E('summary', {}, _('Show raw route JSON')), E('pre', {}, JSON.stringify(route, null, 2)) ])
	]);
}

function matches(route, query, source, health) {
	const text = [ route.intent, route.route_id, route.origin, route.tenant, route.endpoint,
		route.learned_from_peer, route.region ].join(' ').toLowerCase();
	return (!query || text.indexOf(query) >= 0) &&
		(!source || route.source === source) &&
		(!health || (health === 'healthy' ? !!route.healthy : !route.healthy));
}

function renderRows(routes, advanced) {
	return routes.map(function(route) {
		const common = [
			E('td', {}, [ E('code', {}, route.intent || '—'), E('div', { 'class': 'ar-muted' }, 'v' + (route.version || 0)) ]),
			E('td', {}, [ route.origin || '—', E('div', { 'class': 'ar-muted' }, route.learned_from_peer || route.region || _('Local')) ]),
			E('td', {}, [ badge(!!route.healthy), E('div', { 'class': 'ar-muted' }, _('%s hop(s)').format(route.hop_count || 0)) ])
		];
		if (advanced) common.splice(1, 0,
			E('td', {}, [ E('code', { 'class': 'ar-code-wrap' }, route.route_id || '—'), E('div', { 'class': 'ar-muted' }, route.source || '—') ]),
			E('td', {}, [ route.tenant || '—', E('div', { 'class': 'ar-muted ar-code-wrap' }, route.endpoint || '—') ]),
			E('td', {}, [ String(route.latency_ms || 0) + ' ms', E('div', { 'class': 'ar-muted' }, _('cost %s · trust %s · load %s‰').format(route.cost_microunits || 0, route.trust || 0, route.load_permille || 0)) ])
		);
		common.push(E('td', {}, details(route)));
		return E('tr', {}, common);
	});
}

return view.extend({
	load() {
		return callRoutes(LIST_LIMIT).then(result => ({ ok: true, result: result }), error => ({ ok: false, error: error }));
	},

	refreshView() {
		if (this.root && this.payload) dom.content(this.root, this.renderTable(this.payload));
	},

	renderTable(payload) {
		if (!payload.ok) return [
			E('div', { 'class': 'ar-hero' }, E('div', {}, [ E('h2', {}, _('Capability Routes')), E('p', {}, _('Selected ARIB entries installed in the Agent FIB.')) ])),
			E('div', { 'class': 'alert-message error ar-status-message' }, [
				E('strong', {}, _('Could not read capability routes.')),
				E('div', {}, payload.error && payload.error.message ? payload.error.message : String(payload.error || _('Unknown RPC error')))
			])
		];
		const result = payload.result || { routes: [] };
		const query = String(this.query || '').trim().toLowerCase();
		const allRoutes = result.routes || [];
		const filtered = allRoutes.filter(route => matches(route, query, this.sourceFilter || '', this.healthFilter || ''));
		const sources = Array.from(new Set(allRoutes.map(route => route.source).filter(Boolean))).sort();
		const headers = this.advanced
			? [ _('Intent'), _('Route ID'), _('Origin / next hop'), _('Tenant / endpoint'), _('Metrics'), _('State'), _('Details') ]
			: [ _('Intent'), _('Origin / next hop'), _('State'), _('Details') ];
		const search = E('input', { 'type': 'search', 'class': 'cbi-input-text', 'placeholder': _('Search intent, route, tenant, endpoint or peer'), 'value': this.query || '',
			'change': L.bind(function(event) { this.query = event.target.value; this.refreshView(); }, this),
			'keydown': L.bind(function(event) { if (event.key === 'Enter') { this.query = event.target.value; this.refreshView(); } }, this) });
		const source = E('select', { 'class': 'cbi-input-select', 'change': L.bind(function(event) { this.sourceFilter = event.target.value; this.refreshView(); }, this) },
			[ E('option', { 'value': '', 'selected': !this.sourceFilter }, _('All route sources')) ].concat(sources.map(value => E('option', { 'value': value, 'selected': value === this.sourceFilter }, value))));
		const health = E('select', { 'class': 'cbi-input-select', 'change': L.bind(function(event) { this.healthFilter = event.target.value; this.refreshView(); }, this) }, [
			E('option', { 'value': '', 'selected': !this.healthFilter }, _('All states')),
			E('option', { 'value': 'healthy', 'selected': this.healthFilter === 'healthy' }, _('Healthy only')),
			E('option', { 'value': 'unavailable', 'selected': this.healthFilter === 'unavailable' }, _('Unavailable only'))
		]);
		return [
			E('div', { 'class': 'ar-hero' }, [
				E('div', {}, [ E('h2', {}, _('Capability Routes')), E('p', {}, _('Search the selected ARIB entries installed in AFIB. Open a row to inspect its path and native metadata.')) ]),
				E('span', { 'class': 'ar-phase' }, _('generation %s').format(result.generation || 0))
			]),
			result.truncated ? E('div', { 'class': 'alert-message warning ar-status-message' }, _('The table is truncated at %s routes. Use the filters above to narrow the result.').format(LIST_LIMIT)) : '',
			E('div', { 'class': 'ar-toolbar' }, [
				search, source, health,
				E('button', { 'type': 'button', 'class': 'cbi-button', 'click': L.bind(function() {
					this.advanced = !this.advanced; saveAdvanced(this.advanced); this.refreshView();
				}, this) }, this.advanced ? _('Use basic view') : _('Show advanced fields'))
			]),
			E('p', { 'class': 'ar-muted' }, _('%s of %s visible · refreshed %s').format(filtered.length, allRoutes.length, new Date().toLocaleTimeString())),
			E('div', { 'class': 'cbi-section ar-table-wrap' }, E('table', { 'class': 'ar-table' }, [
				E('thead', {}, E('tr', {}, headers.map(label => E('th', {}, label)))),
				E('tbody', {}, filtered.length ? renderRows(filtered, this.advanced) : E('tr', {}, E('td', { 'colspan': headers.length, 'class': 'ar-muted' },
					allRoutes.length ? _('No routes match the active filters.') : _('No capability routes are installed.'))))
			]))
		];
	},

	render(payload) {
		this.payload = payload;
		this.advanced = advancedDefault();
		this.root = E('div', { 'class': 'cbi-map ar-shell' }, this.renderTable(payload));
		poll.add(L.bind(function() {
			return this.load().then(L.bind(function(next) { this.payload = next; this.refreshView(); }, this));
		}, this), 5);
		return E([], [ E('link', { 'rel': 'stylesheet', 'href': L.resource('agent-router/agent-router.css') + '?v=#PKG_VERSION' }), mode.render('developer'), this.root ]);
	},

	handleSaveApply: null,
	handleSave: null,
	handleReset: null
});
