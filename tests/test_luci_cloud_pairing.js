'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const resources = path.join(__dirname, '../feed/luci-app-agent-router/htdocs/luci-static/resources');
const link = 'nexus-router://pair/v1/eyJ0ZXN0Ijp0cnVlfQ';

function harness({ canPair = true, paired = false, pair, overview, changes = {} } = {}) {
	const nodes = [], calls = [], options = [], notices = [];
	let modal = null, refreshed = 0, reloaded = 0, overviewCount = 0;
	function E(tag, attrs = {}, children = []) {
		const node = {
			tag, attrs, children, value: attrs.value || '', textContent: typeof children === 'string' ? children : '',
			listeners: {}, disabled: !!attrs.disabled,
			addEventListener(event, cb) { this.listeners[event] = cb; },
			focus() { this.focused = true; },
			click() { return this.disabled ? undefined : (this.listeners.click || attrs.click || (() => {}))(); }
		};
		nodes.push(node);
		return node;
	}
	const uci = { load: async () => {}, changes: async () => changes, get: (_, __, key) => ({
		enrollment_url: 'https://cloud.example:28443/path?ignored=value', base_url: 'https://edge.example:28444'
	})[key] };
	function Map() {
		this.section = () => {
			const section = {
				tab() {},
				option(type, name, label) {
					const option = { type, name, label, value() {}, depends() {} };
					options.push(option);
					return option;
				},
				taboption(_, ...args) { return section.option(...args); }
			};
			return section;
		};
		this.render = async () => E('form');
	}
	const context = vm.createContext({
		console, URL, E, _: text => text, baseclass: { extend: obj => obj }, view: { extend: obj => obj },
		form: { Map, Value: 'Value', DummyValue: 'DummyValue', Flag: 'Flag', Button: 'Button', ListValue: 'ListValue' },
		uci, fs: { read: async () => '{}' }, poll: { add() {} }, dom: { content() { refreshed++; } },
		mode: { render() {}, enterUser() {} }, L: { resource: x => x, bind: (fn, self) => fn.bind(self) },
		window: { location: { reload() { reloaded++; } } },
		rpc: { declare: ({ method }) => async (...args) => {
			calls.push({ method, args });
			if (method === 'access') return canPair;
			if (method === 'overview') return overview ? overview(++overviewCount) : { generation: 17, cloud: { paired } };
			if (method === 'pair_cloud') return pair ? pair(...args) : { ok: true };
			return {};
		} },
		ui: {
			showModal(title, content) { modal = { title, content }; }, hideModal() { modal = null; },
			addNotification(title, content) { notices.push({ title, content }); }, changes: { apply: async () => {} }
		}
	});
	vm.runInContext("String.prototype.format = function(...args) { let i=0; return this.replace(/%[sd]/g, () => args[i++]); };", context);
	const load = file => vm.runInContext('(function(){' + fs.readFileSync(path.join(resources, file), 'utf8') + '\n})()', context);
	context.pairing = load('agent-router/cloud-pairing.js');
	return {
		context, load, nodes, calls, options, notices, pairing: context.pairing,
		get modal() { return modal; }, get reloaded() { return reloaded; }, get refreshed() { return refreshed; },
		input: () => nodes.findLast(node => node.attrs.id === 'nexus-cloud-pairing-link'),
		submit: () => nodes.findLast(node => node.tag === 'button' && node.attrs.class.includes('important')),
		cancel: () => nodes.findLast(node => node.tag === 'button' && node.textContent === 'Cancel')
	};
}

test('shared dialog submits only the pairing link and observed generation, then clears the secret', async () => {
	const h = harness();
	let succeeded = 0;
	await h.pairing.open(() => succeeded++);
	h.input().value = '  ' + link + '  ';
	await h.submit().click();
	assert.equal(succeeded, 1);
	assert.equal(h.input().value, '');
	assert.equal(h.modal, null);
	const invocation = h.calls.find(call => call.method === 'pair_cloud');
	assert.equal(invocation.args[0], link);
	assert.equal(invocation.args[1], 17);
});

test('URL, old code, unknown version and oversized link cannot reach RPC', async () => {
	const h = harness();
	await h.pairing.open();
	for (const invalid of ['', 'https://cloud.example', 'pair_legacy', 'nexus-router://pair/v2/abc', link + '?x=1', link + 'a'.repeat(49152)]) {
		h.input().value = invalid;
		await h.submit().click();
	}
	assert.equal(h.calls.filter(call => call.method === 'pair_cloud').length, 0);
	assert.match(h.nodes.find(node => node.attrs.role === 'alert').textContent, /complete pairing link/);
});

test('read-only access and unavailable overview fail closed', async () => {
	for (const params of [{ canPair: false }, { overview: () => ({}) }]) {
		const h = harness(params);
		await h.pairing.open();
		assert.equal(h.modal, null);
		assert.equal(h.notices.length, 1);
	}
});

test('cancel clears the link, makes no changes, and allows reopening', async () => {
	const h = harness({ paired: true });
	await h.pairing.open();
	assert.equal(h.modal.title, 'Pair again');
	assert(h.nodes.some(node => /replaces this Router/.test(node.textContent)));
	h.input().value = link;
	h.cancel().click();
	assert.equal(h.input().value, '');
	assert.equal(h.calls.filter(call => call.method === 'pair_cloud').length, 0);
	await h.pairing.open();
	assert.equal(h.modal.title, 'Pair again');
});

test('rejection keeps input for explicit retry; generation conflict refreshes without re-pairing', async () => {
	const h = harness({
		pair: () => ({ ok: false, code: 'CONFIGURATION_CHANGED', message: 'Refresh and retry.' }),
		overview: count => ({ generation: count + 10 })
	});
	await h.pairing.open();
	h.input().value = link;
	await h.submit().click();
	assert.equal(h.input().value, link);
	assert.equal(h.calls.filter(call => call.method === 'pair_cloud').length, 1);
	await h.submit().click();
	assert.equal(h.calls.filter(call => call.method === 'pair_cloud')[1].args[1], 12);
});

test('uncertain transport errors do not retry or expose raw error text', async () => {
	const h = harness({ pair: () => { throw new Error(link); } });
	await h.pairing.open();
	h.input().value = link;
	await h.submit().click();
	assert.equal(h.calls.filter(call => call.method === 'pair_cloud').length, 1);
	assert.match(h.nodes.find(node => node.attrs.role === 'alert').textContent, /could not be confirmed/);
	assert.equal(h.submit().disabled, false);
});

test('while pairing, submit/input/cancel are disabled and duplicate clicks are ignored', async () => {
	let finish;
	const h = harness({ pair: () => new Promise(resolve => { finish = resolve; }) });
	await h.pairing.open();
	h.input().value = link;
	const pending = h.submit().click();
	assert(h.input().disabled && h.cancel().disabled && h.submit().disabled);
	await h.submit().click();
	finish({ ok: true });
	await pending;
	assert.equal(h.calls.filter(call => call.method === 'pair_cloud').length, 1);
});

test('Developer form shows safe read-only public origin and never registers URL or code fields', async () => {
	const h = harness();
	const view = h.load('view/agent-router/cloud.js');
	await view.render([null, { status: {}, running: true }]);
	for (const forbidden of ['base_url', 'enrollment_url', 'pairing_code'])
		assert(!h.options.some(option => option.name === forbidden));
	const address = h.options.find(option => option.name === '_cloud_origin');
	assert.equal(address.type, 'DummyValue');
	assert.equal(address.cfgvalue(), 'https://cloud.example:28443');
	h.context.uci.get = () => 'https://user:secret@cloud.example';
	assert.equal(address.cfgvalue(), 'Not configured');
	assert(!h.options.some(option => option.name === 'device_token'));
});

test('Developer pairing blocks unsaved edits and staged UCI changes', async () => {
	for (const staged of [false, true]) {
		const h = harness({ changes: staged ? { nexus_cloud: [['set']] } : {} });
		const view = h.load('view/agent-router/cloud.js');
		view.formEdited = !staged;
		await view.openPairing();
		assert.equal(h.modal, null);
		assert.match(h.notices[0].content.textContent, /pending configuration/);
	}
});

test('identity is read only, with no certificate, trust, fingerprint or MCP path fields', async () => {
	const h = harness();
	const view = h.load('view/agent-router/cloud.js');
	await view.render([null, { status: {}, running: true }]);
	for (const name of ['identity_mode', 'client_cert', 'client_key', 'ca_file', 'public_ca_file', 'device_cert_sha256', 'mcp_path'])
		assert(!h.options.some(option => option.name === name), name);
	const identity = h.options.find(option => option.name === '_identity_management');
	assert.equal(identity.type, 'DummyValue');
	assert.equal(identity.cfgvalue(), 'Managed by Nexus Cloud');
	h.context.uci.get = () => 'manual';
	assert.match(identity.cfgvalue(), /Existing identity.*Pair again/);
	assert(!h.calls.some(call => ['set', 'delete', 'apply'].includes(call.method)));
});

test('Developer success reloads the managed configuration', async () => {
	const h = harness();
	const view = h.load('view/agent-router/cloud.js');
	await view.openPairing();
	h.input().value = link;
	await h.submit().click();
	assert.equal(h.reloaded, 1);
});

test('User mode uses the same dialog, without inline address/code inputs', async () => {
	const h = harness();
	const view = h.load('view/agent-router/home.js');
	view.payload = { canConfigure: true, overview: {} };
	view.renderCloud({ cloud: { paired: false } });
	assert(!h.nodes.some(node => node.tag === 'input'));
	let refreshed = false;
	view.refresh = async () => { refreshed = true; };
	await view.pairCloud();
	h.input().value = link;
	await h.submit().click();
	assert(refreshed);
});
