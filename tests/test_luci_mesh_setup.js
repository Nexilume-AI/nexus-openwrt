const { test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const source = fs.readFileSync(path.join(__dirname,
	'../feed/luci-app-agent-router/htdocs/luci-static/resources/agent-router/mesh-setup.js'), 'utf8');

function harness(canConfigure = true) {
	let modal, rendered;
	const calls = [], replies = { setup_mesh: { ok: true }, mesh_status: {
		generation: 123, can_create: true, can_join: true, lan_address_available: true
	} };
	const E = (tag, attrs = {}, children) => ({ tag, attrs, children, value: '', hidden: attrs.hidden });
	const context = { E, _: x => x, baseclass: { extend: x => x },
		rpc: { declare: ({method}) => async (...args) => { calls.push([method, args]);
			if (replies[method] instanceof Error) throw replies[method];
			return method === 'access' ? canConfigure : typeof replies[method] === 'function' ? replies[method]() : replies[method]; } },
		poll: { add() {} },
		ui: { showModal: (title, children) => { modal = { title, children }; }, hideModal: () => { modal = null; }, addNotification() {} },
		dom: { content: (node, children) => { node.children = children; rendered = children; } } };
	const api = vm.runInNewContext('(function(){' + source + '})()', context);
	function find(tree, text) {
		if (!tree || typeof tree !== 'object') return null;
		if (typeof text === 'function' ? text(tree) : tree.tag === 'button' && tree.children === text) return tree;
		for (const child of Array.isArray(tree) ? tree : [tree.children]) { const result = find(child, text); if (result) return result; }
		return null;
	}
	return { api, calls, replies, find, get modal() { return modal; }, get rendered() { return rendered; } };
}
const settle = () => new Promise(resolve => setImmediate(resolve));

test('seed sharing entries require review, preserve generation and do not touch Cloud', async () => {
	const h=harness(); const paths=[{directory_host:'seed.example',directory_port:28443,relay_host:'192.168.250.1',relay_port:27444}];
	Object.assign(h.replies.mesh_status,{configured:true,share_paths:JSON.stringify(paths)});
	h.replies.share_mesh={ok:true}; h.api.render(); await settle();
	await h.find(h.rendered,'Sharing entries').attrs.click();
	assert.match(JSON.stringify(h.modal),/does not create upstream NAT forwarding/);
	assert.equal(h.calls.filter(x=>x[0]==='share_mesh').length,0);
	await h.find(h.modal.children,'Review setup').attrs.click();
	const field=h.find(h.modal.children,x=>x.tag==='input'); assert.equal(field.readOnly,true);
	h.replies.mesh_status={...h.replies.mesh_status,generation:999};
	await h.find(h.modal.children,'Save sharing entries').attrs.click();
	assert.deepEqual(Array.from(h.calls.find(x=>x[0]==='share_mesh')[1]),[JSON.stringify(paths),123]);
	assert.equal(h.calls.filter(x=>x[0]==='setup_mesh'||x[0]==='pair_cloud').length,0);
});

test('sharing controls are seed-only, permission-bound and validate before apply',async()=>{
	for(const [allowed,clientOnly] of [[false,false],[true,true]]) {
		const h=harness(allowed);h.replies.mesh_status.configured=true;h.api.render({clientOnly});await settle();
		assert.equal(h.find(h.rendered,'Sharing entries'),null);
	}
	const h=harness();h.replies.mesh_status.configured=true;h.api.render();await settle();
	await h.find(h.rendered,'Sharing entries').attrs.click();
	await h.find(h.modal.children,'Add access path').attrs.click();
	await h.find(h.modal.children,'Review setup').attrs.click();
	assert.ok(h.find(h.modal.children,'Review setup'));
	assert.equal(h.calls.filter(x=>x[0]==='share_mesh').length,0);
});

test('LuCI boolean attributes never disable available actions or render null placeholders', async () => {
	const h = harness(); h.api.render(); await settle();
	const refresh = h.find(h.rendered, 'Refresh status');
	assert.equal(refresh.disabled, false);
	assert.equal(Object.hasOwn(refresh.attrs, 'disabled'), false);
	assert.doesNotMatch(JSON.stringify(h.rendered), /\bnull\b/);
});

test('read-only viewers cannot create or join Mesh', async () => {
	const h = harness(false); h.api.render(); await settle();
	assert.equal(h.find(h.rendered, 'Join Mesh'), null);
	assert.match(JSON.stringify(h.rendered), /Read only/);
	assert.equal(h.calls.filter(x => x[0] === 'setup_mesh').length, 0);
});

test('saved Mesh uses Change and confirmed Disconnect, not manual addresses', async () => {
	const h = harness();
	Object.assign(h.replies.mesh_status, { client_configured: true, client_enabled: true, client_state: 'assigned' });
	h.api.render(); await settle();
	assert.ok(h.find(h.rendered, 'Change Mesh'));
	assert.equal(h.find(h.rendered, 'Join Mesh'), null);
	await h.find(h.rendered, 'Disconnect').attrs.click();
	assert.match(JSON.stringify(h.modal), /Cloud Relay and this router’s hosted Relay and Directory remain running/);
	assert.equal(h.calls.filter(x => x[0] === 'setup_mesh').length, 0);
	h.replies.mesh_status = { ...h.replies.mesh_status, generation: 999 };
	await h.find(h.modal.children, 'Disconnect Mesh').attrs.click();
	assert.deepEqual(Array.from(h.calls.find(x => x[0] === 'setup_mesh')[1]), ['disconnect', '', '', 123]);
});

test('disconnected Mesh retains configuration and can reconnect without another link', async () => {
	const h = harness();
	Object.assign(h.replies.mesh_status, { client_configured: true, client_enabled: false, client_state: 'disconnected' });
	h.api.render({ clientOnly: true }); await settle();
	assert.match(JSON.stringify(h.rendered), /Disconnected/);
	assert.equal(h.find(h.rendered, 'Create Mesh seed'), null);
	assert.equal(h.find(h.rendered, 'Disconnect'), null);
	await h.find(h.rendered, 'Reconnect').attrs.click();
	await h.find(h.modal.children, 'Reconnect Mesh').attrs.click();
	assert.deepEqual(Array.from(h.calls.find(x => x[0] === 'setup_mesh')[1]), ['reconnect', '', '', 123]);
});

test('read only has no client controls and status failure disables stale operations', async () => {
	for (const readOnly of [true, false]) {
		const h = harness(!readOnly);
		Object.assign(h.replies.mesh_status, { client_configured: true, client_enabled: true });
		h.api.render(); await settle();
		if (readOnly) {
			assert.equal(h.find(h.rendered, 'Disconnect'), null);
			assert.equal(h.find(h.rendered, 'Change Mesh'), null);
		} else {
			h.replies.mesh_status = new Error('offline');
			await h.find(h.rendered, 'Refresh status').attrs.click();
			assert.equal(h.find(h.rendered, 'Disconnect').disabled, true);
		}
	}
});

test('advanced settings no longer exposes editable Mesh transport fields', () => {
	const settings = fs.readFileSync(path.join(__dirname,
		'../feed/luci-app-agent-router/htdocs/luci-static/resources/view/agent-router/settings.js'), 'utf8');
	for (const key of ['open_mesh_relay_enabled', 'open_mesh_directory_endpoints', 'open_mesh_directory_connect_ipv4s', 'open_mesh_directory_poll_ms', 'open_mesh_directory_timeout_ms'])
		assert.doesNotMatch(settings, new RegExp("form\\.(?:Flag|Value|DynamicList), '" + key + "'"));
	assert.match(settings, /meshSetup.render\(\{ clientOnly: true \}\)/);
});

test('join sends only public link and current generation, not Cloud pairing', async () => {
	const h = harness(); h.api.render(); await settle();
	await h.find(h.rendered, 'Join Mesh').attrs.click();
	const input = h.find(h.modal.children, x => x.tag === 'textarea');
	input.value = ' nexus-mesh://join/test ';
	await h.find(h.modal.children, 'Review setup').attrs.click();
	// A background refresh must not silently approve a newer configuration.
	h.replies.mesh_status = { ...h.replies.mesh_status, generation: 456 };
	await h.find(h.modal.children, 'Apply Mesh connection').attrs.click();
	assert.deepEqual(Array.from(h.calls.find(x => x[0] === 'setup_mesh')[1]), ['join', 'nexus-mesh://join/test', '', 123]);
	assert.match(JSON.stringify(h.modal), /not yet proof of an end-to-end/);
});

test('create failures preserve dialog and controls for explicit retry', async () => {
	const h = harness(); h.api.render(); await settle();
	h.replies.setup_mesh = { ok: false, code: 'SEED_COMPONENTS_MISSING' };
	await h.find(h.rendered, 'Create Mesh seed').attrs.click();
	await h.find(h.modal.children, 'Review setup').attrs.click();
	const button = h.find(h.modal.children, 'Initialize Relay & Directory');
	await button.attrs.click();
	assert.equal(h.find(h.modal.children, 'Reload checks').disabled, false);
	assert.match(JSON.stringify(h.modal), /Install the Seed profile/);
	assert.equal(h.find(h.modal.children, 'Initialize Relay & Directory').disabled, true);
	assert.deepEqual(Array.from(h.calls.find(x => x[0] === 'setup_mesh')[1]), ['create', '', '', 123]);
});

test('existing seed exposes only a link and clearly distinguishes reachability', async () => {
	const h = harness(); h.replies.mesh_status = { configured: true, join_link: 'nexus-mesh://join/example' };
	h.api.render(); await settle(); h.find(h.rendered, 'Show join link').attrs.click();
	assert.match(JSON.stringify(h.modal), /not Internet reachability/);
	assert.equal(h.calls.filter(x => x[0] === 'setup_mesh').length, 0);
});

test('initialization requires a review before any configuration is written', async () => {
	const h = harness();
	h.replies.mesh_status = { generation: 123, can_create: true, can_join: true, checks: [] };
	h.api.render(); await settle();
	await h.find(h.rendered, 'Create Mesh seed').attrs.click();
	assert.ok(h.find(h.modal.children, 'Review setup'));
	assert.equal(h.calls.filter(x => x[0] === 'setup_mesh').length, 0);
});

test('missing components explain installation and prohibit initialization', async () => {
	const h = harness(); h.replies.mesh_status.can_create = false;
	h.replies.mesh_status.code = 'SEED_COMPONENTS_MISSING';
	h.api.render(); await settle(); await h.find(h.rendered, 'Create Mesh seed').attrs.click();
	assert.match(JSON.stringify(h.modal), /nexus-agent-router-seed/);
	assert.equal(h.find(h.modal.children, 'Review setup').disabled, true);
});

test('invalid address and Cloud links cannot reach review', async () => {
	for (const [action, value, field] of [['Create Mesh seed', '127.0.0.1', 'input'], ['Join Mesh', 'nexus-router://pair/example', 'textarea']]) {
		const h = harness(); h.api.render(); await settle();
		await h.find(h.rendered, action).attrs.click();
		h.find(h.modal.children, x => x.tag === field).value = value;
		await h.find(h.modal.children, 'Review setup').attrs.click();
		assert.ok(h.find(h.modal.children, 'Review setup'));
		assert.ok(h.find(h.modal.children, x => x.attrs && x.attrs.role === 'alert'));
		assert.equal(h.calls.filter(x => x[0] === 'setup_mesh').length, 0);
	}
});

test('generation conflict requires rechecking and preserves user input', async () => {
	const h = harness(); h.api.render(); await settle();
	await h.find(h.rendered, 'Create Mesh seed').attrs.click();
	h.find(h.modal.children, x => x.tag === 'input').value = '192.168.2.3';
	await h.find(h.modal.children, 'Review setup').attrs.click();
	h.replies.setup_mesh = { ok: false, code: 'CONFIGURATION_CHANGED' };
	await h.find(h.modal.children, 'Initialize Relay & Directory').attrs.click();
	assert.match(JSON.stringify(h.modal), /Configuration changed/);
	assert.equal(h.find(h.modal.children, 'Initialize Relay & Directory').disabled, true);
	await h.find(h.modal.children, 'Reload checks').attrs.click();
	assert.equal(h.find(h.modal.children, x => x.tag === 'input').value, '192.168.2.3');
});

test('interrupted response never automatically repeats a write', async () => {
	const h = harness(); h.api.render(); await settle();
	await h.find(h.rendered, 'Create Mesh seed').attrs.click();
	await h.find(h.modal.children, 'Review setup').attrs.click();
	h.replies.setup_mesh = new Error('private transport details');
	await h.find(h.modal.children, 'Initialize Relay & Directory').attrs.click();
	assert.match(JSON.stringify(h.modal), /response was interrupted/);
	assert.doesNotMatch(JSON.stringify(h.modal), /private transport/);
	assert.equal(h.calls.filter(x => x[0] === 'setup_mesh').length, 1);
});

test('stopped configured seed is not misrepresented as reachable', async () => {
	const h = harness(); h.replies.mesh_status.configured = true;
	h.api.render(); await settle();
	assert.match(JSON.stringify(h.rendered), /service is not running/);
	assert.equal(h.find(h.rendered, 'Create Mesh seed'), null);
});

test('setup and cancel are disabled during apply; success offers the next step', async () => {
	const h = harness(); let resolve;
	h.api.render(); await settle(); await h.find(h.rendered, 'Create Mesh seed').attrs.click();
	await h.find(h.modal.children, 'Review setup').attrs.click();
	h.replies.setup_mesh = () => new Promise(r => { resolve = r; });
	const pending = h.find(h.modal.children, 'Initialize Relay & Directory').attrs.click();
	assert.equal(h.find(h.modal.children, 'Cancel').disabled, true);
	assert.equal(h.find(h.modal.children, 'Applying and verifying…').disabled, true);
	resolve({ configured: true, join_link: 'nexus-mesh://join/test' }); await pending;
	assert.match(JSON.stringify(h.modal), /another router/);
	assert.ok(h.find(h.modal.children, 'Select link'));
});
