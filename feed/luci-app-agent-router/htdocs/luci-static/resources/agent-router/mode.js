'use strict';
'require baseclass';

const SESSION_KEY = 'nexus.agentRouter.developerEnabled';
const DEVELOPER_SECTIONS = {
	overview: true, setup: true, cloud: true, roles: true, agents: true,
	protocols: true, routes: true, neighbors: true, trust: true,
	settings: true, peers: true, policies: true
};

function target(mode, section) {
	if (mode !== 'developer')
		return L.url('admin/network/agent-router/home');

	section = DEVELOPER_SECTIONS[section] ? section : 'overview';
	return L.url('admin/network/agent-router/developer/' + section);
}

function developerEnabled() {
	try {
		return window.sessionStorage.getItem(SESSION_KEY) === '1';
	}
	catch (error) {
		return false;
	}
}

function setDeveloperEnabled(enabled) {
	try {
		if (enabled)
			window.sessionStorage.setItem(SESSION_KEY, '1');
		else
			window.sessionStorage.removeItem(SESSION_KEY);
	}
	catch (error) {}
}

function isDeveloperPath() {
	return /\/agent-router\/developer(?:\/|$)/.test(window.location.pathname);
}

if (isDeveloperPath() && !developerEnabled())
	window.location.replace(target('user'));
else if (/\/admin\/status\/agent-router(?:\/|$)/.test(window.location.pathname)) {
	// Hidden menu aliases keep bookmarks working; move the browser to the
	// canonical Network URL without enabling Developer mode from a URL.
	const section = window.location.pathname.match(/\/developer\/([^/]+)\/?$/);
	window.location.replace(isDeveloperPath()
		? target('developer', section && section[1]) : target('user'));
}

return baseclass.extend({
	enterUser() {
		setDeveloperEnabled(false);
	},

	select(mode, section) {
		setDeveloperEnabled(mode === 'developer');
		window.location.href = target(mode, section);
	},

	render(current) {
		if (current === 'developer' && !developerEnabled())
			return E('div', { 'class': 'ar-shell' }, _('User mode'));

		return E('div', { 'class': 'ar-mode-bar ar-shell', 'aria-label': _('Agent Routing interface mode') }, [
			E('div', { 'class': 'ar-mode-copy' }, [
				E('strong', {}, _('Nexus Agent Network')),
				E('span', { 'class': 'ar-muted' }, current === 'developer'
					? _('Advanced configuration and diagnostics')
					: _('Simple status and one-click controls'))
			]),
			E('div', { 'class': 'ar-mode-switch', 'role': 'group', 'aria-label': _('Interface mode') }, [
				E('button', {
					'class': 'btn ar-mode-option' + (current === 'user' ? ' is-active' : ''),
					'type': 'button',
					'aria-pressed': current === 'user' ? 'true' : 'false',
					'click': L.bind(function() { this.select('user'); }, this)
				}, _('User mode')),
				E('button', {
					'class': 'btn ar-mode-option' + (current === 'developer' ? ' is-active' : ''),
					'type': 'button',
					'aria-pressed': current === 'developer' ? 'true' : 'false',
					'click': L.bind(function() { this.select('developer'); }, this)
				}, _('Developer mode'))
			])
		]);
	}
});
