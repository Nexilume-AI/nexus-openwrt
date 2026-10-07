'use strict';
'require baseclass';
'require rpc';
'require ui';

const callOverview = rpc.declare({ object: 'nexus-agent-ui', method: 'overview', expect: {} });
const callPairCloud = rpc.declare({
	object: 'nexus-agent-ui', method: 'pair_cloud',
	params: [ 'pairing_code', 'expected_generation' ], expect: {}
});
const callCanPair = rpc.declare({
	object: 'session', method: 'access', params: [ 'scope', 'object', 'function' ], expect: { access: false }
});
let active = false;

function validLink(value) {
	return value.length <= 49152 && /^nexus-router:\/\/pair\/v1\/[A-Za-z0-9_-]+={0,2}$/.test(value);
}

return baseclass.extend({
	open: function(onPaired) {
		if (active) return Promise.resolve();
		active = true;
		return Promise.all([callCanPair('ubus', 'nexus-agent-ui', 'pair_cloud'), callOverview()]).then(function(data) {
			if (!data[0]) throw new Error('read_only');
			let generation = data[1].generation;
			if (!Number.isSafeInteger(generation)) throw new Error('overview_unavailable');
			const paired = !!(data[1].cloud && data[1].cloud.paired);
			let busy = false;
			const error = E('p', { 'class': 'cbi-value-error', 'role': 'alert', 'aria-live': 'polite' });
			const input = E('input', {
				'id': 'nexus-cloud-pairing-link', 'type': 'password', 'autocomplete': 'off',
				'spellcheck': 'false', 'maxlength': 49152, 'class': 'cbi-input-text',
				'placeholder': 'nexus-router://pair/v1/…', 'aria-describedby': 'nexus-cloud-pairing-help'
			});
			const close = function() {
				if (busy) return;
				input.value = '';
				active = false;
				ui.hideModal();
			};
			const cancel = E('button', { 'class': 'btn', 'type': 'button', 'click': close }, _('Cancel'));
			const submit = E('button', { 'class': 'btn cbi-button-action important', 'type': 'button' },
				paired ? _('Pair again') : _('Pair with Nexus Cloud'));
			submit.addEventListener('click', async function() {
				if (busy) return;
				const link = String(input.value || '').trim();
				if (!validLink(link)) {
					error.textContent = _('Paste the complete pairing link from Nexus Cloud, not a Cloud address or an old pairing code.');
					input.focus();
					return;
				}
				busy = true;
				input.disabled = cancel.disabled = submit.disabled = true;
				submit.textContent = _('Pairing…');
				error.textContent = '';
				let accepted = false;
				try {
					const result = await callPairCloud(link, generation);
					accepted = !!(result && result.ok === true);
					if (!accepted) {
						error.textContent = result && result.message ? result.message : _('The Router rejected the request.');
						// No automatic retry: the user reviews the error before resubmitting.
						if (result && result.code === 'CONFIGURATION_CHANGED') {
							const fresh = await callOverview();
							if (Number.isSafeInteger(fresh.generation)) generation = fresh.generation;
						}
					}
				}
				catch (ignored) {
					error.textContent = _('Pairing could not be confirmed. Check the connection status before trying again.');
				}
				finally {
					busy = false;
					input.disabled = cancel.disabled = submit.disabled = false;
					submit.textContent = paired ? _('Pair again') : _('Pair with Nexus Cloud');
				}
				if (accepted) {
					close();
					ui.addNotification(null, E('p', {}, _('Pairing started. This page will update automatically.')));
					if (onPaired) onPaired();
				}
			});
			ui.showModal(paired ? _('Pair again') : _('Pair with Nexus Cloud'), [
				E('p', { 'id': 'nexus-cloud-pairing-help' }, _('Paste a pairing link from your trusted Nexus Cloud console. It includes the address and certificate trust.')),
				paired ? E('p', { 'class': 'alert-message warning' }, _('Pairing again replaces this Router’s existing Cloud enrollment.')) : '',
				E('label', { 'for': 'nexus-cloud-pairing-link' }, _('Pairing link')),
				input, error,
				E('div', { 'class': 'right' }, [cancel, ' ', submit])
			]);
			input.focus();
		}).catch(function(ignored) {
			active = false;
			ui.addNotification(_('Cloud pairing unavailable'), E('p', {},
				_('Check your configuration permission and refresh the Router status.')), 'error');
		});
	}
});
