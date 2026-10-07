'use strict';
'require view';
'require rpc';
'require poll';
'require dom';
'require ui';
'require agent-router.mode as mode';

const LIST_LIMIT = 200;
const callDiscoveries = rpc.declare({ object: 'agent', method: 'discoveries', params: [ 'limit' ], expect: {} });
const callPromotions = rpc.declare({ object: 'agent', method: 'promotions', params: [ 'limit' ], expect: {} });
const callCards = rpc.declare({ object: 'agent', method: 'cards', params: [ 'limit' ], expect: {} });
const callCardTrust = rpc.declare({ object: 'agent', method: 'card_trust', expect: {} });
const callPromote = rpc.declare({
	object: 'agent', method: 'discovery_promote',
	params: [ 'source', 'router_id', 'generation', 'peer_id', 'graceful_restart_seconds' ], expect: {}
});
const callRevoke = rpc.declare({ object: 'agent', method: 'discovery_revoke', params: [ 'peer_id' ], expect: {} });
const callCardRevoke = rpc.declare({ object: 'agent', method: 'card_revoke', params: [ 'router_id' ], expect: {} });

function settle(name, promise, fallback) {
	return promise.then(data => ({ name: name, ok: true, data: data }), error => ({
		name: name, ok: false, data: fallback,
		error: error && error.message ? error.message : String(error)
	}));
}

function seconds(value) {
	return Math.max(0, Math.round(Number(value || 0) / 1000)) + ' s';
}

function badge(label, kind) {
	return E('span', { 'class': 'ar-badge ' + (kind || '') }, label);
}

function evidence(candidate) {
	const values = [];
	values.push(badge(_('LAN observed'), 'ar-badge-info'));
	if (candidate.policy_match) values.push(badge(_('Static policy match'), 'ar-badge-up'));
	if (candidate.auto_promotion_eligible) values.push(badge(_('Auto eligible'), 'ar-badge-warn'));
	return E('div', { 'class': 'ar-evidence' }, values);
}

function table(title, headers, rows, empty) {
	return E('div', { 'class': 'cbi-section ar-table-wrap' }, [
		E('h3', {}, title),
		E('table', { 'class': 'ar-table' }, [
			E('thead', {}, E('tr', {}, headers.map(h => E('th', {}, h)))),
			E('tbody', {}, rows.length ? rows : E('tr', {}, E('td', { 'colspan': headers.length, 'class': 'ar-muted' }, empty)))
		])
	]);
}

function actionButton(label, style, handler) {
	return E('button', { 'type': 'button', 'class': 'cbi-button ' + style, 'click': handler }, label);
}

return view.extend({
	load() {
		return Promise.all([
			settle('LAN discovery', callDiscoveries(LIST_LIMIT), { discoveries: [] }),
			settle('promotions', callPromotions(LIST_LIMIT), { promotions: [] }),
			settle('Agent Cards', callCards(LIST_LIMIT), { cards: [] }),
			settle('Directory trust', callCardTrust(), { loaded: false, card_keys: [] })
		]);
	},

	refresh() {
		return this.load().then(L.bind(function(data) {
			if (this.root) dom.content(this.root, this.renderBody(data));
		}, this));
	},

	notifyError(error) {
		ui.addNotification(_('Peer trust operation failed'), E('p', {}, error && error.message ? error.message : String(error)), 'error');
	},

	confirmPromote(candidate, generation) {
		const peerInput = E('input', { 'type': 'text', 'class': 'cbi-input-text', 'value': candidate.router_id || '' });
		const graceInput = E('input', { 'type': 'number', 'class': 'cbi-input-text', 'min': 5, 'max': 300, 'value': 30 });
		const proof = _('This candidate was observed on the local LAN. Confirming creates a leased direct ARPX peer.');
		ui.showModal(_('Confirm peer trust'), [
			E('p', {}, proof),
			E('div', { 'class': 'ar-modal-grid' }, [
				E('label', {}, _('Router ID')), E('code', { 'class': 'ar-code-wrap' }, candidate.router_id || '—'),
				E('label', { 'for': 'ar-peer-id' }, _('Peer ID')), peerInput,
				E('label', { 'for': 'ar-grace' }, _('Graceful restart')), graceInput
			]),
			E('div', { 'class': 'ar-risk-note' }, _('The candidate generation is checked by agentd. If discovery changed after this dialog opened, the operation fails without changing the live peer table.')),
			E('div', { 'class': 'right' }, [
				actionButton(_('Cancel'), 'cbi-button-neutral', ui.hideModal),
				' ',
				actionButton(_('Trust peer'), 'cbi-button-positive', L.bind(function() {
					const peerId = String(peerInput.value || '').trim();
					const grace = Number(graceInput.value);
					if (!peerId || !Number.isInteger(grace) || grace < 5 || grace > 300) {
						this.notifyError(_('Peer ID is required and graceful restart must be 5–300 seconds.'));
						return;
					}
					return callPromote('lan', candidate.router_id, generation, peerId, grace)
						.then(L.bind(function() {
							ui.hideModal();
							ui.addNotification(null, E('p', {}, _('Peer trust confirmed. ARPX runtime is reloading.')), 'info');
							return this.refresh();
						}, this)).catch(L.bind(this.notifyError, this));
				}, this))
			])
		]);
	},

	confirmRevoke(promotion) {
		ui.showModal(_('Revoke peer trust'), [
			E('p', {}, _('Revoke peer %s and remove its learned capabilities from AFIB immediately?').format(promotion.peer_id || promotion.router_id)),
			promotion.automatic ? E('div', { 'class': 'ar-risk-note' }, _('This peer was admitted automatically. It can return on the next discovery refresh unless auto admission or its allowlist is changed.')) : '',
			E('div', { 'class': 'right' }, [
				actionButton(_('Cancel'), 'cbi-button-neutral', ui.hideModal), ' ',
				actionButton(_('Revoke trust'), 'cbi-button-negative', L.bind(function() {
					return callRevoke(promotion.peer_id).then(L.bind(function() {
						ui.hideModal();
						ui.addNotification(null, E('p', {}, _('Peer trust revoked. Learned routes are stale until graceful expiry.')), 'info');
						return this.refresh();
					}, this)).catch(L.bind(this.notifyError, this));
				}, this))
			])
		]);
	},

	confirmCardRevoke(card) {
		ui.showModal(_('Revoke Agent Card'), [
			E('p', {}, _('Revoke the authorized Agent Card for %s? Its automatic peer and learned routes will be withdrawn.').format(card.router_id || '—')),
			E('div', { 'class': 'right' }, [
				actionButton(_('Cancel'), 'cbi-button-neutral', ui.hideModal), ' ',
				actionButton(_('Revoke card'), 'cbi-button-negative', L.bind(function() {
					return callCardRevoke(card.router_id).then(L.bind(function() {
						ui.hideModal();
						ui.addNotification(null, E('p', {}, _('Agent Card authorization revoked.')), 'info');
						return this.refresh();
					}, this)).catch(L.bind(this.notifyError, this));
				}, this))
			])
		]);
	},

	pendingRows(lan) {
		const rows = [];
		(lan.discoveries || []).filter(item => !item.promoted).forEach(L.bind(function(item) {
			rows.push(E('tr', {}, [
				E('td', {}, [E('code', {}, item.router_id || '—'), E('div', { 'class': 'ar-muted' }, item.domain_id || '—')]),
				E('td', {}, [item.ipv4 || item.hostname || '—', E('div', { 'class': 'ar-muted' }, _('LAN · %s').format(item.interface || '—'))]),
				E('td', {}, evidence(item)),
				E('td', {}, seconds(item.remaining_ms)),
				E('td', {}, actionButton(_('Review & trust'), 'cbi-button-action', L.bind(this.confirmPromote, this, item, lan.generation)))
			]));
		}, this));
		return rows;
	},

	promotionRows(result) {
		return (result.promotions || []).map(L.bind(function(item) {
			return E('tr', {}, [
				E('td', {}, [E('code', {}, item.peer_id || '—'), E('div', { 'class': 'ar-muted' }, item.router_id || '—')]),
				E('td', {}, [item.endpoint || '—', E('div', { 'class': 'ar-muted' }, item.connect_ipv4 || _('DNS underlay'))]),
				E('td', {}, [badge(item.automatic ? _('Automatic') : _('User confirmed'), item.automatic ? 'ar-badge-warn' : 'ar-badge-up'), E('div', { 'class': 'ar-muted' }, item.source || '—')]),
				E('td', {}, [seconds(item.remaining_ms), E('div', { 'class': 'ar-muted' }, _('grace %s s').format(item.graceful_restart_seconds || 0))]),
				E('td', {}, actionButton(_('Revoke'), 'cbi-button-negative', L.bind(this.confirmRevoke, this, item)))
			]);
		}, this));
	},

	cardRows(result) {
		return (result.cards || []).map(L.bind(function(card) {
			const capabilities = (card.capabilities || []).map(c => c.intent + '.v' + c.version).join(', ');
			return E('tr', {}, [
				E('td', {}, [E('code', {}, card.router_id || '—'), E('div', { 'class': 'ar-muted' }, card.domain_id || '—')]),
				E('td', {}, [card.issuer || '—', E('div', { 'class': 'ar-muted ar-code-wrap' }, card.key_id || '—')]),
				E('td', {}, [badge(card.directory_trusted ? _('Directory trusted') : _('Policy authorized'), card.directory_trusted ? 'ar-badge-up' : 'ar-badge-info'), E('div', { 'class': 'ar-muted ar-code-wrap' }, capabilities || _('No capabilities'))]),
				E('td', {}, [String(card.revision || 0), E('div', { 'class': 'ar-muted' }, seconds(card.remaining_ms))]),
				E('td', {}, actionButton(_('Revoke card'), 'cbi-button-negative', L.bind(this.confirmCardRevoke, this, card)))
			]);
		}, this));
	},

	trustKeyRows(result) {
		return (result.card_keys || []).map(function(key) {
			return E('tr', {}, [
				E('td', {}, [E('code', {}, key.router_id || '—'), E('div', { 'class': 'ar-muted' }, key.domain_id || '—')]),
				E('td', {}, [key.issuer || '—', E('div', { 'class': 'ar-muted ar-code-wrap' }, key.key_id || '—')]),
				E('td', {}, badge(key.status || _('unknown'), key.status === 'active' ? 'ar-badge-up' : 'ar-badge-warn')),
				E('td', { 'class': 'ar-code-wrap' }, key.public_key_sha256 || '—')
			]);
		});
	},

	renderBody(data) {
		const lan = data[0].data || { discoveries: [] };
		const promotions = data[1].data || { promotions: [] };
		const cards = data[2].data || { cards: [] };
		const trust = data[3].data || { loaded: false, card_keys: [] };
		const failures = data.filter(item => !item.ok);
		const pending = this.pendingRows(lan);
		return [
			E('div', { 'class': 'ar-hero' }, [
				E('div', {}, [E('h2', {}, _('Peer Trust')), E('p', {}, _('Review LAN candidates, revoke existing peers, and inspect Agent Card and Directory trust evidence.'))]),
				E('span', { 'class': 'ar-phase' }, _('generation %s').format(promotions.generation || 0))
			])
		].concat(failures.map(function(item) {
			return E('div', { 'class': 'alert-message error ar-status-message' }, [
				E('strong', {}, _('%s evidence is unavailable').format(item.name)), E('div', {}, item.error)
			]);
		})).concat([
			E('div', { 'class': 'ar-grid ar-trust-summary' }, [
				E('div', { 'class': 'ar-card' }, [E('div', { 'class': 'ar-card-label' }, _('Pending candidates')), E('div', { 'class': 'ar-card-value ar-warn' }, String(pending.length)), E('div', { 'class': 'ar-card-note' }, _('Not admitted to ARPX'))]),
				E('div', { 'class': 'ar-card' }, [E('div', { 'class': 'ar-card-label' }, _('Trusted peers')), E('div', { 'class': 'ar-card-value ar-ok' }, String((promotions.promotions || []).length)), E('div', { 'class': 'ar-card-note' }, _('Leased promotions'))]),
				E('div', { 'class': 'ar-card' }, [E('div', { 'class': 'ar-card-label' }, _('Authorized Cards')), E('div', { 'class': 'ar-card-value' }, String((cards.cards || []).length)), E('div', { 'class': 'ar-card-note' }, cards.authorization_mode || _('off'))]),
				E('div', { 'class': 'ar-card' }, [E('div', { 'class': 'ar-card-label' }, _('Directory trust')), E('div', { 'class': 'ar-card-value ' + (trust.loaded ? 'ar-ok' : 'ar-warn') }, trust.loaded ? _('Loaded') : _('Not loaded')), E('div', { 'class': 'ar-card-note' }, trust.directory_id || _('No signed bundle'))])
			]),
			table(_('Pending trust'), [ _('Router'), _('Endpoint'), _('Evidence'), _('Lease'), _('Action') ], pending, _('No untrusted discovery candidates are currently leased.')),
			table(_('Trusted dynamic peers'), [ _('Peer'), _('Endpoint'), _('Admission'), _('Lease'), _('Action') ], this.promotionRows(promotions), _('No dynamic Peer trust is active.')),
			table(_('Authorized Agent Cards'), [ _('Router'), _('Issuer / key'), _('Authorization'), _('Revision / lease'), _('Action') ], this.cardRows(cards), _('No authorized Agent Cards are active.')),
			table(_('Directory Card trust keys'), [ _('Router'), _('Issuer / key'), _('Status'), _('Public key SHA-256') ], this.trustKeyRows(trust), trust.loaded ? _('The signed bundle contains no Card keys.') : _('No verified Directory trust bundle is loaded.')),
			E('p', { 'class': 'ar-muted' }, _('Updated %s').format(new Date().toLocaleTimeString()))
		]);
	},

	render(data) {
		this.root = E('div', { 'class': 'cbi-map ar-shell' }, this.renderBody(data));
		poll.add(L.bind(this.refresh, this), 5);
		return E([], [E('link', { 'rel': 'stylesheet', 'href': L.resource('agent-router/agent-router.css') + '?v=#PKG_VERSION' }), mode.render('developer'), this.root]);
	},

	handleSaveApply: null,
	handleSave: null,
	handleReset: null
});
