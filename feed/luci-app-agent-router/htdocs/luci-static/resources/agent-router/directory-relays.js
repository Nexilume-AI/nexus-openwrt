'use strict';
'require baseclass';
'require rpc';
'require ui';
'require dom';

const getCatalog = rpc.declare({object:'nexus-agent-ui', method:'directory_relays', expect:{}});
const saveCatalog = rpc.declare({object:'nexus-agent-ui', method:'set_directory_relays', timeout:60000,
	params:['catalog_json', 'expected_generation'], expect:{}});
const access = rpc.declare({object:'session', method:'access',
	params:['scope', 'object', 'function'], expect:{access:false}});

function button(label, click, disabled, primary) {
	const node = E('button', {type:'button', 'class':'btn' + (primary ? ' cbi-button-action' : ''), click}, label);
	node.disabled = !!disabled;
	return node;
}
function reason(code) {
	return ({
		DIRECTORY_COMPONENT_MISSING: _('Install or upgrade the Directory package to edit Relay targets.'),
		DIRECTORY_CONFIG_UNAVAILABLE: _('Initialize the Directory first, then reload. Its configuration could not be read.'),
		DIRECTORY_CONFIG_UNSAFE: _('This configuration path is not supported by the editor. Use a regular JSON file directly inside /etc/nexus-directoryd/.'),
		DIRECTORY_CONFIG_INVALID: _('The Directory configuration is incomplete or invalid. Initialize or repair the service first. Nothing was changed.'),
		CONFIGURATION_CHANGED: _('Configuration changed. Close and reload before reviewing your changes again.'),
		BUSY: _('Another configuration change is in progress. Try again shortly.'),
		INVALID_RELAY: _('Check Relay ID, Router ID, domain and connection IPv4. Use lowercase identifiers without spaces.'),
		INVALID_RELAY_ENDPOINT: _('Use an HTTPS address with a certificate hostname, explicit port (1–65535), and /arpx/v1 path.'),
		DUPLICATE_RELAY: _('Relay IDs must be unique.'),
		INVALID_RELAY_COUNT: _('Keep between 1 and 32 Relay targets.'),
		INVALID_MESH_RELAY_COUNT: _('Select between 1 and 4 Relays for Open Mesh assignment.'),
		RELAY_IN_USE: _('This Relay is still referenced by a configured identity. Keep it until that assignment is removed.'),
		OPEN_MESH_DISABLED: _('Open Mesh assignment is not enabled for this Directory.'),
		PINNED_SEED_RELAY: _('The default Seed Relay is protected by existing pinned join links. Use Sharing entries for its access addresses; it cannot be replaced here.'),
		APPLY_FAILED: _('Directory verification failed. The previous configuration was restored; review the targets and retry.'),
		ROLLBACK_FAILED: _('The previous configuration was restored, but Directory recovery failed. Check Role status before retrying.'),
		ROLLBACK_CONFLICT: _('Another program changed the file during verification. It was not overwritten; the protected backup was kept. Reload and check Directory diagnostics.')
	})[code] || _('The Directory request failed. Reload its status before trying again.');
}
function requestRow(row) {
	return {id:row.id, router_id:row.router_id, domain_id:row.domain_id,
		endpoint:row.endpoint, connect_ipv4:row.connect_ipv4, open_mesh:row.open_mesh};
}
function validation(rows, current) {
	if (!rows.length || rows.length > 32) return 'INVALID_RELAY_COUNT';
	if (new Set(rows.map(r => r.id)).size !== rows.length) return 'DUPLICATE_RELAY';
	const id = /^[a-z0-9](?:[a-z0-9._-]{0,62}[a-z0-9])?$/;
	for (const row of rows) {
		const ip = row.connect_ipv4.split('.');
		if (!id.test(row.id) || !id.test(row.router_id) || !row.domain_id || ip.length !== 4 ||
			ip.some(p => !/^(0|[1-9][0-9]{0,2})$/.test(p) || Number(p) > 255)) return 'INVALID_RELAY';
		const endpoint = /^https:\/\/[a-z0-9.-]+:([0-9]{1,5})\/arpx\/v1$/.exec(row.endpoint);
		if (!endpoint || Number(endpoint[1]) < 1 || Number(endpoint[1]) > 65535) return 'INVALID_RELAY_ENDPOINT';
	}
	const count = rows.filter(r => r.open_mesh).length;
	if (current.open_mesh_enabled && (count < 1 || count > 4)) return 'INVALID_MESH_RELAY_COUNT';
	return '';
}
function edit(current, refresh) {
	const rows = current.relays.map(r => Object.assign({}, r));
	let busy = false, review = false, stale = false, message = '';
	const body = E('div', {'class':'ar-shell ar-mesh-flow ar-directory-editor'});
	const fields = [ ['id', _('Relay ID')], ['router_id', _('Relay Router ID')],
		['domain_id', _('Relay domain')], ['endpoint', _('Relay HTTPS endpoint')], ['connect_ipv4', _('Connection IPv4')] ];
	function draw() {
		dom.content(body, [
			E('p', {}, _('Enter the values from the target Relay. Its TLS identity and ticket key must already match this Directory; adding an address does not provision a remote Relay or verify Internet reachability. Nexus Cloud Relay is not changed.')),
			current.pinned_seed ? E('p', {'class':'alert-message warning'}, _('Existing v2 join links pin this Seed Relay. Its default assignment is protected. Additional targets can be catalogued, but are not automatically assigned to those clients. Use Sharing entries to change Seed access paths.')) : E('span'),
			E('div', {'class':'ar-directory-editor-rows'}, rows.map((row, index) => {
				const protectedSeed = current.pinned_seed && row.id === 'open-mesh-relay';
				return E('fieldset', {'class':'ar-directory-relay-fields'}, [
					E('legend', {}, row.id || _('New Relay')),
					...fields.map(([key, label]) => {
						const input = E('input', {type:'text', value:row[key], 'aria-label':label + ' ' + (index + 1),
							input:ev => {row[key] = ev.target.value.trim();}});
						input.readOnly = review || busy || protectedSeed;
						input.maxLength = key === 'endpoint' ? 255 : key === 'domain_id' ? 253 : key === 'connect_ipv4' ? 15 : 64;
						input.placeholder = key === 'endpoint' ? 'https://relay.example:17444/arpx/v1' : '';
						return E('label', {}, [label, input]);
					}),
					(() => {
						const checkbox = E('input', {type:'checkbox', change:ev => {row.open_mesh = ev.target.checked;}});
						checkbox.checked = row.open_mesh;
						checkbox.disabled = review || busy || current.pinned_seed || !current.open_mesh_enabled;
						return E('label', {'class':'ar-directory-assignment'}, [checkbox, _('Assign to Open Mesh nodes')]);
					})(),
					row.identity_references ? E('p', {'class':'ar-muted'}, _('Referenced by configured identities; removal is blocked.')) : E('span'),
					!review ? button(_('Remove Relay'), () => {rows.splice(index,1);draw();}, busy || protectedSeed || row.identity_references > 0) : E('span')
				]);
			})),
			review ? E('p', {role:'status'}, _('Saving changes only the Relay catalog and Open Mesh selection. An enabled Directory is restarted and verified; failures restore the previous file. Existing Relay tunnels are not stopped.')) : E('span'),
			message ? E('p', {role:'alert'}, message) : E('span'),
			E('div', {'class':'ar-mesh-actions'}, [
				button(_('Cancel'), ui.hideModal, busy),
				!review ? button(_('Add Relay'), () => {rows.push({id:'',router_id:'',domain_id:'',endpoint:'',connect_ipv4:'',open_mesh:false});draw();}, busy || rows.length >= 32) : button(_('Back'), () => {review=false;draw();}, busy),
				stale ? button(_('Close and reload'), async () => {ui.hideModal();await refresh();}, busy) :
					button(busy ? _('Applying and verifying…') : review ? _('Save Relay targets') : _('Review changes'), async () => {
						message = '';
						if (!review) {
							const invalid = validation(rows,current);
							if (invalid) message=reason(invalid); else review=true;
							draw();return;
						}
						busy=true;draw();
						try {
							const result = await saveCatalog(JSON.stringify({revision:current.revision, relays:rows.map(requestRow)}), current.generation);
							if (!result || !result.ok) {
								message=reason(result && result.code);
								stale=['CONFIGURATION_CHANGED','ROLLBACK_FAILED','ROLLBACK_CONFLICT'].includes(result && result.code);
							} else {
								ui.hideModal();await refresh();
								ui.addNotification(null, E('p', {}, result.state === 'unchanged' ? _('No Relay target changes to apply.') : result.state === 'saved_stopped' ? _('Relay targets saved. Directory remains stopped until enabled in Router Roles.') : _('Directory Relay targets saved and verified.')), 'info');
								return;
							}
						} catch (error) {message=_('The response was interrupted. Close and reload to check the saved configuration before retrying.');stale=true;}
						finally {busy=false;}
						draw();
					},busy,true)
			])
		]);
	}
	ui.showModal(_('Directory Relay targets'), [body]);draw();
}

return baseclass.extend({
	render: function() {
		const node = E('section', {'class':'ar-role-surface ar-directory-catalog'});
		let current, canEdit=false, loading=false, message='';
		function draw() {
			dom.content(node,[
				E('h3',{},_('Directory Relay targets')),
				E('p',{'class':'ar-muted'},_('Manage Relays this Directory can assign. No JSON editing is required.')),
				message ? E('p',{role:'alert'},message) : E('span'),
				current ? E('ul',{'class':'ar-directory-relay-list'},current.relays.map(row => E('li',{},[
					E('strong',{},row.id), E('span',{'class':'ar-muted'},row.endpoint),
					E('span',{},row.open_mesh ? _('Open Mesh assignment') : row.identity_references ? _('Identity assignment') : _('Not assigned'))
				]))) : loading ? E('p',{role:'status'},_('Loading Relay targets…')) : E('span'),
				E('div',{'class':'ar-mesh-actions'},[
					canEdit && current ? button(_('Edit Relay targets'),()=>edit(current,refresh),loading || !!message) : E('span',{},canEdit ? '' : _('Read only')),
					button(_('Refresh status'),refresh,loading)
				])
			]);
		}
		async function refresh() {
			if (loading) return;
			loading=true;draw();
			try {
				const result=await getCatalog();
				if (!result || !result.ok) message=reason(result && result.code);
				else {current=result;message='';}
			} catch (error) {message=_('Could not refresh Directory targets. The existing configuration was not changed.');}
			finally {loading=false;draw();}
		}
		access('ubus','nexus-agent-ui','set_directory_relays').then(allowed=>{canEdit=allowed;return refresh();}).catch(()=>refresh());
		return node;
	}
});
