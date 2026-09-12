'use strict';
'require view';
'require rpc';
'require poll';
'require dom';
'require agent-router.mode as mode';

const LIST_LIMIT = 200;
const callNeighbors = rpc.declare({ object: 'agent', method: 'neighbors', params: [ 'limit' ], expect: {} });
const callDiscoveries = rpc.declare({ object: 'agent', method: 'discoveries', params: [ 'limit' ], expect: {} });
const callCrossDiscoveries = rpc.declare({ object: 'agent', method: 'cross_discoveries', params: [ 'limit' ], expect: {} });

function settle(name, promise, fallback) {
	return promise.then(data => ({ name: name, ok: true, data: data }), error => ({
		name: name, ok: false, data: fallback,
		error: error && error.message ? error.message : String(error)
	}));
}

function statusBadge(up, label) {
	return E('span', { 'class': 'ar-badge ' + (up ? 'ar-badge-up' : 'ar-badge-down') }, label);
}

function peerDetails(peer) {
	return E('details', { 'class': 'ar-route-details' }, [
		E('summary', {}, _('Session details')),
		E('dl', { 'class': 'ar-detail-grid' }, [
			E('dt', {}, _('Transport phase')), E('dd', {}, peer.transport_phase || peer.state || '—'),
			E('dt', {}, _('Direction')), E('dd', {}, peer.transport_direction || '—'),
			E('dt', {}, _('TLS / HTTP2')), E('dd', {}, _('%s handshakes · %s sessions').format(peer.tls_handshakes || 0, peer.h2_sessions || 0)),
			E('dt', {}, _('Traffic')), E('dd', {}, _('%s sent · %s received').format(peer.messages_sent || 0, peer.messages_received || 0)),
			E('dt', {}, _('Reconnects')), E('dd', {}, String(peer.reconnects || 0)),
			E('dt', {}, _('Last error')), E('dd', {}, peer.last_error || _('None recorded'))
		]),
		E('details', { 'class': 'ar-raw' }, [ E('summary', {}, _('Show raw session JSON')), E('pre', {}, JSON.stringify(peer, null, 2)) ])
	]);
}

function neighborRows(result) {
	return (result.neighbors || []).map(function(peer) {
		const up = !!peer.session_up;
		return E('tr', {}, [
			E('td', {}, [E('code', {}, peer.peer_id || '—'), E('div', { 'class': 'ar-muted' }, peer.role || '—')]),
			E('td', {}, [peer.router_id || '—', E('div', { 'class': 'ar-muted' }, peer.domain_id || '—')]),
			E('td', {}, [peer.endpoint || '—', E('div', { 'class': 'ar-muted' }, peer.connect_ipv4 || _('DNS underlay'))]),
			E('td', {}, [statusBadge(up, up ? _('Established') : (peer.transport_phase || peer.state || _('Down'))), E('div', { 'class': 'ar-muted' }, peer.transport_direction || '—'), peerDetails(peer)]),
			E('td', {}, [String(peer.learned_routes || 0), E('div', { 'class': 'ar-muted' }, _('sequence %s').format(peer.last_sequence || 0))])
		]);
	});
}

function discoveryRows(result, cross) {
	const values = cross ? (result.cross_discoveries || result.discoveries || []) : (result.discoveries || []);
	return values.map(function(item) {
		const endpoint = cross ? ((item.target || '—') + ':' + (item.port || 0)) : ((item.ipv4 || item.hostname || '—') + ':' + (item.port || 0));
		return E('tr', {}, [
			E('td', {}, [E('code', {}, item.router_id || '—'), E('div', { 'class': 'ar-muted' }, item.domain_id || '—')]),
			E('td', {}, endpoint),
			E('td', {}, cross ? statusBadge(!!item.dnssec_secure, item.dnssec_secure ? _('DNSSEC secure') : _('Rejected')) : (item.interface || '—')),
			E('td', {}, item.promoted ? statusBadge(true, _('Promoted')) : statusBadge(!!item.auto_promotion_eligible, item.auto_promotion_eligible ? _('Eligible') : _('Observed'))),
			E('td', {}, Math.max(0, Math.round(Number(item.remaining_ms || 0) / 1000)) + ' s')
		]);
	});
}

function section(title, rows, empty) {
	return E('div', { 'class': 'cbi-section ar-table-wrap' }, [
		E('h3', {}, title),
		E('table', { 'class': 'ar-table' }, [
			E('thead', {}, E('tr', {}, [E('th', {}, _('Router / peer')), E('th', {}, _('Endpoint')), E('th', {}, _('Transport / trust')), E('th', {}, _('Admission')), E('th', {}, _('Lease'))])),
			E('tbody', {}, rows.length ? rows : E('tr', {}, E('td', { 'colspan': 5, 'class': 'ar-muted' }, empty)))
		])
	]);
}

return view.extend({
	load() {
		return Promise.all([
			settle('neighbors', callNeighbors(LIST_LIMIT), { neighbors: [] }),
			settle('LAN discovery', callDiscoveries(LIST_LIMIT), { discoveries: [] }),
			settle('cross-domain discovery', callCrossDiscoveries(LIST_LIMIT), { cross_discoveries: [] })
		]);
	},

	renderBody(data) {
		const peers = data[0].data || {};
		const lan = data[1].data || {};
		const cross = data[2].data || {};
		const failures = data.filter(item => !item.ok);
		return [
			E('div', { 'class': 'ar-hero' }, [
				E('div', {}, [E('h2', {}, _('Neighbors & Discovery')), E('p', {}, _('ARPX session state and admitted LAN or DNSSEC discovery metadata. Discovery does not itself create capability routes.'))]),
				E('span', { 'class': 'ar-phase' }, _('%s sessions').format((peers.neighbors || []).filter(p => p.session_up).length))
			])
		].concat(failures.map(function(item) {
			return E('div', { 'class': 'alert-message error ar-status-message' }, [
				E('strong', {}, _('%s is unavailable').format(item.name)), E('div', {}, item.error)
			]);
		})).concat([
			section(_('ARPX neighbors'), neighborRows(peers), _('No peers are configured or promoted.')),
			section(_('LAN DNS-SD candidates'), discoveryRows(lan, false), lan.enabled === false ? _('LAN discovery is disabled.') : _('No LAN candidates are currently leased.')),
			section(_('Cross-domain SVCB candidates'), discoveryRows(cross, true), cross.enabled === false ? _('Cross-domain discovery is disabled.') : _('No DNSSEC-validated candidates are currently leased.')),
			E('p', { 'class': 'ar-muted' }, _('Updated %s').format(new Date().toLocaleTimeString()))
		]);
	},

	render(data) {
		const root = E('div', { 'class': 'cbi-map ar-shell' }, this.renderBody(data));
		poll.add(L.bind(function() {
			return this.load().then(L.bind(function(next) { dom.content(root, this.renderBody(next)); }, this));
		}, this), 5);
		return E([], [E('link', { 'rel': 'stylesheet', 'href': L.resource('agent-router/agent-router.css') + '?v=#PKG_VERSION' }), mode.render('developer'), root]);
	},

	handleSaveApply: null,
	handleSave: null,
	handleReset: null
});
