'use strict';
const assert = require('node:assert/strict');
const test = require('node:test');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const source = fs.readFileSync(path.join(__dirname,
	'../feed/luci-app-agent-router/htdocs/luci-static/resources/view/agent-router/roles.js'), 'utf8');

function harness() {
	const updates = [], polls = [];
	let formRenders = 0;
	const replies = {};
	function E(tag, attrs, children) { return { tag, attrs, children, classList: { add() {} } }; }
	function Map() {
		this.section = () => ({ option: () => ({ value() {} }) });
		this.render = async () => { formRenders++; return E('form'); };
	}
	const context = vm.createContext({
		E, _: text => text, view: { extend: obj => obj }, form: { Map },
		uci: { load: async () => {}, get: (_, section, key) => key === 'mode' ? 'all' : undefined },
		rpc: { declare: () => async name => {
			if (replies[name] instanceof Error) throw replies[name];
			return { [name]: { instances: replies[name] || {} } };
		} },
		fs: { stat: async () => ({ type: 'file' }), read: async () => '{}' },
		poll: { add: (fn, interval) => polls.push({ fn, interval }) },
		dom: { content: (node, content) => updates.push({ node, content }) },
		mode: { render: () => E('mode') }, L: { url: x => x, resource: x => x }
		, meshSetup: { render: () => E('mesh') }, directoryRelays: { render: () => E('directory-relays') }
	});
	vm.runInContext("String.prototype.format = function(...args) { let i=0; return this.replace(/%[sd]/g, () => args[i++]); };", context);
	const api = vm.runInContext('(function(){' + source.replace('return view.extend({',
		'const page = view.extend({') + '\nreturn { page, serviceState, roleStatus }; })()', context);
	return { ...api, replies, updates, polls, get formRenders() { return formRenders; } };
}

test('real procd states distinguish running, retrying, exhausted and unknown', () => {
	const h = harness();
	const state = instances => h.serviceState({ 'nexus-relayd': { instances } }, 'nexus-relayd');
	const ready = { installed: true, configured: true };
	const status = data => h.roleStatus('relay', true, { ...ready, ...data });
	assert.equal(status(state({ instance1: { running: true, exit_code: 1 } })).label, 'Ready');
	const retry = status(state({ instance1: { running: false, exit_code: 1,
		respawn: { threshold: 3600, timeout: 30, retry: 0 } } }));
	assert.equal(retry.label, 'Recovering');
	assert.match(retry.next, /exit: 1.*30 seconds/);
	assert.equal(status(state({ instance1: { running: false, exit_code: 1 } })).label, 'Recovery stopped');
	assert.equal(status(state({})).label, 'Stopped');
	assert.equal(status(h.serviceState(null, 'nexus-relayd')).label, 'Status unavailable');
});

test('disabled role is not falsely shown as a recovery failure', () => {
	const h = harness();
	assert.equal(h.roleStatus('relay', false, { running: false, exitCode: 1 }).label, 'Not enabled');
	assert.equal(h.roleStatus('relay', true, { installed: false }).label, 'Component missing');
	assert.equal(h.roleStatus('relay', true, { installed: true, configured: false }).label, 'Configuration missing');
	assert.equal(h.roleStatus('relay', true, { installed: true, configured: false, running: true }).label, 'Ready');
});

test('poll updates recovery without replacing forms; RPC failure recovers next poll', async () => {
	const h = harness();
	h.replies['nexus-relayd'] = new Error('unavailable');
	await h.page.render(await h.page.load());
	assert.equal(h.formRenders, 3);
	assert.equal(h.polls.length, 1);
	assert.equal(h.polls[0].interval, 5);
	await h.polls[0].fn();
	assert.match(JSON.stringify(h.updates.at(-1).content), /Status unavailable/);
	h.replies['nexus-relayd'] = { instance1: { running: false, exit_code: 1, respawn: { timeout: 30, retry: 0 } } };
	await h.polls[0].fn();
	assert.match(JSON.stringify(h.updates.at(-1).content), /Recovering/);
	h.replies['nexus-relayd'] = { instance1: { running: true } };
	h.replies['nexus-directoryd'] = { instance1: { running: true } };
	await h.polls[0].fn();
	const rendered = JSON.stringify(h.updates.at(-1).content);
	assert.match(rendered, /Running/);
	assert.doesNotMatch(rendered, /Status unavailable|Recovering|Recovery stopped/);
	assert.equal(h.formRenders, 3);
	assert.ok(h.updates.every(update => update.node === h.updates[0].node));
});
