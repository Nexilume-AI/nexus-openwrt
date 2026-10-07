'use strict';
'require view';
'require form';
'require rpc';
'require uci';
'require ui';
'require agent-router.mode as mode';

const callOverview = rpc.declare({ object: 'nexus-agent-ui', method: 'overview', expect: {} });
const callService = rpc.declare({ object: 'service', method: 'list', params: [ 'name' ], expect: {} });

function serviceRunning(payload, name) {
	const service = (payload || {})[name] || {};
	const instances = service.instances || {};
	return Object.keys(instances).some(key => !!instances[key].running);
}

function cloudManagedAuthentication() {
	return uci.get('agent_gateway', 'main', 'auth_mode') === 'jwt' &&
		uci.get('agent_gateway', 'main', 'jwt_jwks_file') === '/etc/agent-gw/nexus-cloud-jwks.json' &&
		!!uci.get('agent_gateway', 'main', 'jwt_issuer') &&
		!!uci.get('agent_gateway', 'main', 'jwt_audience') &&
		!!uci.get('agent_gateway', 'main', 'jwt_jwks_url');
}

function authenticationSummary(data) {
	const cloud = data[4] && data[4].cloud;
	const managed = !!cloud && cloud.paired === true && cloudManagedAuthentication();
	const lan = uci.get('agent_gateway', 'main', 'lan_sdk_enabled') === '1';
	const gatewayUp = serviceRunning(data[2], 'agent-gw');
	const jwksUp = serviceRunning(data[3], 'agent-jwks');
	const cloudLabel = !cloud ? _('Status unavailable') : !cloud.paired ? _('Not paired') :
		managed ? _('Managed by Nexus Cloud') : _('Cloud authentication needs synchronization');
	return E('section', { 'class': 'ar-role-summary ar-role-surface' }, [
		E('h2', {}, _('Authentication & trust')),
		E('p', { 'class': 'ar-muted' }, _('Cloud pairing manages the caller identity issuer, audience, signing-key refresh and Cloud certificate trust. No token, key URL or certificate path needs to be entered here.')),
		E('div', { 'class': 'ar-role-facts' }, [
			E('div', { 'class': 'ar-role-fact' }, [E('span', {}, _('Cloud calls')), E('strong', {}, cloudLabel)]),
			E('div', { 'class': 'ar-role-fact' }, [E('span', {}, _('LAN SDK')), E('strong', {}, lan ? _('Automatic LAN authentication') : _('Agent services not enabled'))]),
			E('div', { 'class': 'ar-role-fact' }, [E('span', {}, _('Agent gateway')), E('strong', {}, data[2] == null ? _('Status unavailable') : gatewayUp ? _('Running') : _('Stopped'))]),
			managed ? E('div', { 'class': 'ar-role-fact' }, [E('span', {}, _('Cloud signing-key refresh')), E('strong', {}, data[3] == null ? _('Status unavailable') : jwksUp ? _('Running') : _('Needs attention'))]) : E([], [])
		]),
		E('p', { 'class': 'ar-muted' }, _('LAN Agents obtain short-lived sessions automatically. Cloud pairing is not required for local registration. LAN bootstrap is separate from public and Relay caller authorization. Existing standalone policies are preserved.')),
		E('a', { 'class': 'btn', 'href': L.url('admin/network/agent-router/developer/cloud') }, cloud && cloud.paired ? _('View Cloud connection') : _('Pair with Nexus Cloud')),
		!lan ? E('a', { 'class': 'btn', 'href': L.url('admin/network/agent-router/home') }, _('Enable Agent services')) : E([], [])
	]);
}

function gatewayDefaultFlag(option, name, fallback) {
	option.cfgvalue = function() {
		const value = uci.get('agent_gateway', 'main', name);
		return value == null ? fallback : value;
	};
	option.write = function(sectionId, value) {
		return uci.set('agent_gateway', 'main', name, value || '0');
	};
	option.remove = function() {
		return uci.set('agent_gateway', 'main', name, fallback);
	};
	return option;
}

function pythonAgentRouterUrl() {
	// Registration belongs on the trusted LAN bridge, never the LuCI web port.
	const automatic = uci.get('agent_gateway', 'main', 'lan_sdk_enabled') === '1';
	const legacy = uci.get('agent_gateway', 'main', 'no_jwt_lan_enabled') === '1' &&
		uci.get('agent_gateway', 'main', 'auth_mode') === 'none';
	if (!automatic && !legacy) return null;
	const fallback = automatic ? '7446' : '7445';
	const listen = uci.get('agent_gateway', 'main', automatic ? 'lan_sdk_listen' : 'no_jwt_lan_listen') || '0.0.0.0:' + fallback;
	const match = String(listen).match(/:(\d{1,5})$/);
	let host = window.location.hostname || 'ROUTER_LAN_IP';
	if (host.includes(':') && host.charAt(0) !== '[')
		host = '[' + host + ']';
	return 'http://%s:%s'.format(host, match ? match[1] : fallback);
}

function requireLanAccess() {
	if (pythonAgentRouterUrl()) return true;
	ui.showModal(_('Enable Agent services'), [
		E('p', {}, _('Enable Agent services in User mode to make automatic LAN registration available.')),
		E('a', { 'class': 'btn', 'href': L.url('admin/network/agent-router/home') }, _('Open User mode'))
	]);
	return false;
}

function showPythonAgentQuickStart() {
	if (!requireLanAccess()) return;
	const router = pythonAgentRouterUrl();
	const code = [
		'# Install: python -m pip install nexilume',
		'from nexus_agent import NexusAgent',
		'',
		'agent = NexusAgent(',
		'    router=%s,'.format(JSON.stringify(router)),
		'    tenant="local",',
		')',
		'',
		'@agent.capability("demo.echo", public_ipv6=False)',
		'def echo(payload):',
		'    return {"echo": payload}',
		'',
		'# Starts the server, registers it, renews its lease, and unregisters on exit.',
		'agent.run()'
	].filter(line => line != null).join('\n');
	const copy = E('button', {
		'class': 'btn cbi-button cbi-button-positive',
		'click': function() {
			if (!navigator.clipboard || !navigator.clipboard.writeText) {
				ui.addNotification(null, E('p', {},
					_('Clipboard access is unavailable. Select the code and copy it manually.')));
				return;
			}
			navigator.clipboard.writeText(code).then(function() {
				ui.addNotification(null, E('p', {}, _('Python Agent example copied.')));
			});
		}
	}, _('Copy code'));
	ui.showModal(_('Automatic Python Agent'), [
		E('p', {}, _('Run this on a LAN computer. The SDK publishes its callback address to the router automatically; no endpoint row is required.')),
		E('pre', { 'class': 'ar-code-wrap' }, code),
		E('div', { 'class': 'right' }, [ copy ])
	]);
}

function showPythonHttpsAgentQuickStart() {
	if (!requireLanAccess()) return;
	const router = pythonAgentRouterUrl();
	const caBundleId = uci.get('agent_gateway', 'main', 'remote_backend_ca_bundle_id') || 'system';
	const code = [
		'# The certificate must contain a lowercase dotted DNS SAN.',
		'from nexus_agent import NexusAgent',
		'',
		'agent = NexusAgent(',
		'    router=%s,'.format(JSON.stringify(router)),
		'    tenant="local",',
		'    cert_file="agent-fullchain.pem",',
		'    key_file="agent-key.pem",',
		caBundleId === 'system' ? null : '    server_ca_bundle_id=%s,'.format(JSON.stringify(caBundleId)),
		')',
		'',
		'@agent.capability("demo.secure-echo", public_ipv6=False)',
		'def echo(payload):',
		'    return {"echo": payload}',
		'',
		'# DNS identity, numeric address, port and fingerprint are registered automatically.',
		'agent.run()'
	].filter(line => line != null).join('\n');
	ui.showModal(_('Automatic HTTPS Python Agent'), [
		E('p', {}, _('The SDK reads the server certificate and renews the router mapping with the Agent lease. No fixed endpoint mapping is required.')),
		E('pre', { 'class': 'ar-code-wrap' }, code),
		E('div', { 'class': 'right' }, [
			E('button', {
				'class': 'btn cbi-button cbi-button-positive',
				'click': function() {
					if (navigator.clipboard && navigator.clipboard.writeText)
						navigator.clipboard.writeText(code);
				}
			}, _('Copy code'))
		])
	]);
}

function a2aEdgeUrl(sectionId) {
	const authority = uci.get('agent_adapter', sectionId, 'authority') || 'AGENT_CARD_ID';
	const skill = uci.get('agent_adapter', sectionId, 'selector') || 'SKILL_ID';
	return '%s//%s/a2a/%s/%s'.format(
		window.location.protocol, window.location.host,
		encodeURIComponent(authority), encodeURIComponent(skill));
}

function mcpEdgeUrl(sectionId) {
	const authority = uci.get('agent_adapter', sectionId, 'authority') || 'MCP_SERVER_ID';
	return '%s//%s/mcp/%s'.format(
		window.location.protocol, window.location.host,
		encodeURIComponent(authority));
}

function fastmcpPythonExample(sectionId) {
	const authority = uci.get('agent_adapter', sectionId, 'authority') || 'eda-tools';
	const tool = uci.get('agent_adapter', sectionId, 'selector') || 'lint_verilog';
	const intent = uci.get('agent_adapter', sectionId, 'intent') || 'chip.verilog.verify.lint';
	const tenant = uci.get('agent_adapter', 'main', 'default_tenant') || 'local';
	const router = pythonAgentRouterUrl();
	return [
		'# Install: python -m pip install "nexilume[fastmcp]"',
		'from fastmcp import Context, FastMCP',
		'from nexus_agent import AutoTokenProvider, CapabilityRegistration, NexusAgentClient, NexusAgentServer',
		'from nexus_agent.fastmcp import FastMCPBridge',
		'',
		'mcp = FastMCP(%s)'.format(JSON.stringify(authority)),
		'',
		'@mcp.tool(name=%s)'.format(JSON.stringify(tool)),
		'async def nexus_tool(payload: dict, ctx: Context) -> dict:',
		'    await ctx.report_progress(1, 2, "started")',
		'    result = {"ok": True, "input": payload}  # replace with your Tool logic',
		'    await ctx.report_progress(2, 2, "complete")',
		'    return result',
		'',
		'server = NexusAgentServer(',
		'    "0.0.0.0", 9443,',
		'    cert_file="agent.crt", key_file="agent.key",',
		')',
		'router_url = %s'.format(JSON.stringify(router)),
		'router = NexusAgentClient(router_url, token_provider=AutoTokenProvider(router_url))',
		'bridge = FastMCPBridge(',
		'    mcp, server,',
		'    {%s: CapabilityRegistration('.format(JSON.stringify(tool)),
		'        intent=%s,'.format(JSON.stringify(intent)),
		'        origin="agent://%s/MY_AGENT",'.format(tenant),
		'        endpoint="https://AGENT_HOST:9443/invoke",',
		'        tenant=%s,'.format(JSON.stringify(tenant)),
		'    )},',
		')',
		'bridge.serve_registered(router)',
		'',
		'# MCP client URL: %s'.format(mcpEdgeUrl(sectionId)),
		'# Use Accept: application/json, text/event-stream for progress + result.',
		'# Standard MCP returns JSON-RPC events; GET resumption is not supported.',
		'# Nexus cursor replay: set X-Nexus-Stream-Format: envelope on every POST.',
		'# In that custom mode, keep a unique task ID and send Last-Event-ID on reconnect.'
	].join('\n');
}

function a2aPythonExample(sectionId) {
	const authority = uci.get('agent_adapter', sectionId, 'authority') || 'echo-card';
	const skill = uci.get('agent_adapter', sectionId, 'selector') || 'echo';
	const intent = uci.get('agent_adapter', sectionId, 'intent') || 'demo.a2a.echo';
	const tenant = uci.get('agent_adapter', 'main', 'default_tenant') || 'local';
	const router = '%s//%s'.format(window.location.protocol, window.location.host);
	return [
		'# Install: python -m pip install "nexilume[a2a]"',
		'',
		'# Supply at most one short-lived credential outside source code.',
		'import os',
		'jwt = os.environ.get("NEXUS_AGENT_TOKEN")',
		'transaction_token = os.environ.get("NEXUS_AGENT_TRANSACTION_TOKEN")',
		'if jwt and transaction_token:',
		'    raise RuntimeError("set JWT or Transaction Token, not both")',
		'',
		'# Caller',
		'import asyncio',
		'from nexus_agent.a2a import NexusA2AClient',
		'',
		'async def call():',
		'    async with NexusA2AClient(',
		'        %s,'.format(JSON.stringify(router)),
		'        card_id=%s,'.format(JSON.stringify(authority)),
		'        skill=%s,'.format(JSON.stringify(skill)),
		'        token=jwt,',
		'        transaction_token=transaction_token,',
		'    ) as client:',
		'        # Access JWTs support automatic reconnect; one-time tokens do not.',
		'        async for event in client.stream("hello", resume=bool(jwt)):',
		'            print(event.event_id, event.kind, event.state, event.text)',
		'',
		'asyncio.run(call())',
		'',
		'# Called Agent: use your official AgentExecutor class as MyExecutor',
		'from nexus_agent.a2a import NexusA2AAgent',
		'',
		'agent = NexusA2AAgent(',
		'    MyExecutor(),',
		'    router=%s,'.format(JSON.stringify(pythonAgentRouterUrl())),
		'    identity="agent://%s/MY_AGENT",'.format(tenant),
		'    endpoint="https://AGENT_HOST:9443/invoke",',
		'    tenant=%s,'.format(JSON.stringify(tenant)),
		'    host="0.0.0.0", port=9443,',
		'    cert_file="agent.crt", key_file="agent.key",',
		'    card_url=%s,'.format(JSON.stringify(a2aEdgeUrl(sectionId))),
		')',
		'agent.expose(skill=%s, intent=%s)'.format(
			JSON.stringify(skill), JSON.stringify(intent)),
		'agent.run()'
	].join('\n');
}

function showA2AExample(sectionId) {
	if (!requireLanAccess()) return;
	const code = a2aPythonExample(sectionId);
	const copy = E('button', {
		'class': 'btn cbi-button cbi-button-positive',
		'click': function() {
			if (!navigator.clipboard || !navigator.clipboard.writeText) {
				ui.addNotification(null, E('p', {},
					_('Clipboard access is unavailable. Select the code and copy it manually.')));
				return;
			}
			navigator.clipboard.writeText(code).then(function() {
				ui.addNotification(null, E('p', {}, _('Python example copied.')));
			});
		}
	}, _('Copy code'));
	ui.showModal(_('Python A2A caller and Agent'), [
		E('p', {}, _('Agent registration uses automatic LAN authentication. The standalone A2A API caller below is separate: it still needs an authorized caller JWT or Transaction Token. Cloud pairing is not caller authorization. LuCI never stores or displays the token.')),
		E('pre', { 'class': 'ar-code-wrap' }, code),
		E('div', { 'class': 'right' }, [ copy ])
	]);
}

function showFastMCPExample(sectionId) {
	if (!requireLanAccess()) return;
	const code = fastmcpPythonExample(sectionId);
	const copy = E('button', {
		'class': 'btn cbi-button cbi-button-positive',
		'click': function() {
			if (!navigator.clipboard || !navigator.clipboard.writeText) {
				ui.addNotification(null, E('p', {},
					_('Clipboard access is unavailable. Select the code and copy it manually.')));
				return;
			}
			navigator.clipboard.writeText(code).then(function() {
				ui.addNotification(null, E('p', {}, _('FastMCP example copied.')));
			});
		}
	}, _('Copy code'));
	ui.showModal(_('Streaming FastMCP Agent'), [
		E('p', {}, _('The SDK automatically acquires registration credentials on the trusted LAN listener. This HTTPS server example still requires its own server certificate; Cloud pairing does not replace that certificate.')),
		E('pre', { 'class': 'ar-code-wrap' }, code),
		E('div', { 'class': 'right' }, [ copy ])
	]);
}

function gatewayFlag(option, name) {
	option.cfgvalue = function() {
		return uci.get('agent_gateway', 'main', name) || '0';
	};
	option.write = function(sectionId, value) {
		return uci.set('agent_gateway', 'main', name, value || '0');
	};
	option.remove = function() {
		return uci.set('agent_gateway', 'main', name, '0');
	};
	return option;
}

function gatewayValue(option, name) {
	option.cfgvalue = function() {
		return uci.get('agent_gateway', 'main', name);
	};
	option.write = function(sectionId, value) {
		return uci.set('agent_gateway', 'main', name, value);
	};
	option.remove = function() {
		return uci.unset('agent_gateway', 'main', name);
	};
	return option;
}

function gatewayList(option, name) {
	option.cfgvalue = function() {
		const value = uci.get('agent_gateway', 'main', name);
		if (Array.isArray(value)) return value;
		return value ? [ value ] : [];
	};
	option.write = function(sectionId, value) {
		const values = (value || []).map(v => String(v).trim()).filter(Boolean);
		if (values.length) return uci.set('agent_gateway', 'main', name, values);
		return uci.unset('agent_gateway', 'main', name);
	};
	option.remove = function() {
		return uci.unset('agent_gateway', 'main', name);
	};
	return option;
}

function synchronizedStreamFlag(option) {
	option.cfgvalue = function(sectionId) {
		return uci.get('agent_adapter', sectionId, 'stream_enabled') || '0';
	};
	option.write = function(sectionId, value) {
		uci.set('agent_adapter', sectionId, 'stream_enabled', value || '0');
		return uci.set('agent_gateway', 'main', 'stream_enabled', value || '0');
	};
	option.remove = function(sectionId) {
		uci.set('agent_adapter', sectionId, 'stream_enabled', '0');
		return uci.set('agent_gateway', 'main', 'stream_enabled', '0');
	};
	return option;
}

function mappingComponent(option, label) {
	option.validate = function(sectionId, value) {
		const text = String(value || '');
		if (text.length < 1 || text.length > 95 || !/^[A-Za-z0-9._:@\/-]+$/.test(text))
			return _('%s must be 1–95 characters using letters, numbers, dot, underscore, colon, slash, @ or hyphen.').format(label);
		return true;
	};
	return option;
}

function intentName(option) {
	option.validate = function(sectionId, value) {
		const text = String(value || '');
		if (text.length < 1 || text.length > 127 ||
			text.startsWith('.') || text.endsWith('.') || text.includes('..') ||
			!/^[A-Za-z0-9._-]+$/.test(text))
			return _('Intent must be 1–127 characters, use letters, numbers, dot, underscore or hyphen, and must not contain empty dot segments.');
		return true;
	};
	return option;
}

return view.extend({
	handleSave: function(ev) {
		return this.super('handleSave', [ev]).then(function() {
			return ui.changes.apply(false);
		});
	},


	load() {
		return Promise.all([
			uci.load('agent_adapter'),
			uci.load('agent_gateway'),
			callService('agent-gw').catch(() => null),
			callService('agent-jwks').catch(() => null),
			callOverview().catch(() => null)
		]);
	},

	render(data) {
		let m, s, o;
		m = new form.Map('agent_adapter', _('Agent APIs & Protocols'),
			_('Enable Python SDK access and publish friendly MCP/A2A protocol mappings. For A2A: add one mapping, choose A2A, fill Agent Card ID, Skill ID and capability intent, save, then use Show Python example. No UCI command is required.'));

		s = m.section(form.NamedSection, 'main', 'adapter', _('Agent access'));
		s.addremove = false;
		s.tab('common', _('Common setup'));
		s.tab('backends', _('Python Agent Servers'));
		s.tab('advanced', _('Advanced limits'));

		o = gatewayFlag(s.taboption('common', form.Flag, '_invoke_enabled', _('Allow Agents to call other Agents')), 'invoke_enabled');
		o.rmempty = false;
		o.description = _('Enables the Nexus Envelope route-and-invoke data plane used by the Python SDK and protocol adapters.');

		o = gatewayFlag(s.taboption('common', form.Flag, '_registration_enabled', _('Allow Python SDK registration')), 'registration_enabled');
		o.rmempty = false;
		o.description = _('Allows registration, renewal and withdrawal. LAN SDK credentials are obtained automatically when Agent services is enabled.');

		o = s.taboption('common', form.Flag, 'enabled', _('Enable MCP/A2A compatibility'));
		o.rmempty = false;
		o.description = _('Accepts configured MCP tools/call and A2A message:send requests and maps them to capability intents below.');

		o = synchronizedStreamFlag(s.taboption('common', form.Flag, 'stream_enabled', _('Enable streaming responses')));
		o.rmempty = false;
		o.description = _('Enables MCP Streamable HTTP, A2A message:stream and Nexus SSE forwarding in both adapter and gateway.');

		o = gatewayFlag(s.taboption('common', form.Flag, '_stream_resume_enabled', _('Resume long-running calls after a disconnect')), 'stream_resume_enabled');
		o.rmempty = false;
		o.default = o.enabled;
		o.depends('stream_enabled', '1');
		o.description = _('Keeps a bounded task-to-route binding so an MCP or A2A caller reconnects to the original Agent and receives only events after Last-Event-ID. The task is never restarted on another Agent.');

		o = s.taboption('common', form.Value, 'default_tenant', _('Default tenant'));
		o.rmempty = false;
		o.placeholder = 'local';
		o.description = _('Local routing namespace for protocol requests. Cloud ownership is assigned by the paired Cloud, not by this value.');

		o = s.taboption('common', form.Value, 'source_agent', _('Protocol adapter identity'));
		o.rmempty = false;
		o.placeholder = 'agent://local/adapterd';

		o = s.taboption('backends', form.DummyValue, '_python_agent_help', _('What the router calls'));
		o.cfgvalue = function() { return _('Automatic for SDK-managed Agents'); };
		o.description = _('A Python Agent Server is the function process that receives an invocation after this router selects its capability route. The SDK reports the server callback address during registration and removes the route when its lease ends.');

		o = gatewayDefaultFlag(s.taboption('backends', form.Flag, '_lan_backend_enabled', _('Automatically call registered LAN Agents')), 'lan_backend_enabled', '1');
		o.rmempty = false;
		o.default = o.enabled;
		o.description = _('Recommended. Python SDK Agents using a private IPv4 or IPv6 ULA address become callable immediately after registration. There is no endpoint list to maintain.');

		o = s.taboption('backends', form.Button, '_python_agent_example', _('Python quick start'));
		o.inputtitle = _('Show automatic Agent example');
		o.inputstyle = 'apply';
		o.onclick = showPythonAgentQuickStart;
		o.description = _('Generates a minimal called-Agent server using this router address.');

		o = gatewayDefaultFlag(s.taboption('backends', form.Flag, '_remote_backend_enabled', _('Automatically call registered HTTPS Agents')), 'remote_backend_enabled', '1');
		o.rmempty = false;
		o.default = o.enabled;
		o.description = _('Recommended. The Python SDK submits the Agent address, certificate DNS name, trusted CA label and certificate fingerprint. The router creates a route-bound mapping and removes it when the Agent lease ends.');

		o = s.taboption('backends', form.DummyValue, '_python_agent_ca_policy', _('HTTPS Agent certificate trust'));
		o.cfgvalue = function() {
			const label = uci.get('agent_gateway', 'main', 'remote_backend_ca_bundle_id') || 'system';
			const file = uci.get('agent_gateway', 'main', 'remote_backend_ca_file') || '/etc/ssl/certs/ca-certificates.crt';
			return label === 'system' && file === '/etc/ssl/certs/ca-certificates.crt' ?
				_('System certificate authorities') : _('Existing administrator-approved trust');
		};
		o.write = function() {};
		o.remove = function() {};
		o.description = _('Read only. HTTPS Agent server certificates are separate from Cloud identity. Existing administrator-approved CA configuration is preserved; Cloud pairing does not authorize arbitrary Agent certificates. Ordinary LAN Agents need no certificate setup.');

		o = s.taboption('backends', form.Button, '_python_https_agent_example', _('HTTPS Python quick start'));
		o.inputtitle = _('Show automatic HTTPS example');
		o.inputstyle = 'apply';
		o.onclick = showPythonHttpsAgentQuickStart;
		o.depends('_remote_backend_enabled', '1');

		o = s.taboption('backends', form.Flag, '_show_fixed_https_mappings', _('Show legacy fixed mappings'));
		o.rmempty = true;
		o.cfgvalue = function() { return '0'; };
		o.write = function() {};
		o.remove = function() {};
		o.depends('_remote_backend_enabled', '1');
		o.description = _('Compatibility and recovery only. SDK-managed Agents do not need entries here.');
		o = gatewayList(s.taboption('backends', form.DynamicList, '_remote_backend_map', _('Fixed HTTPS endpoint mappings (legacy)')), 'remote_backend_map');
		o.placeholder = 'linter-1.example.test:9443=[2001:db8::20]';
		o.description = _('Manual emergency format: certificate-name:port=address. Dynamic SDK registrations always take precedence for their route.');
		o.depends('_show_fixed_https_mappings', '1');
		o.validate = function(sectionId, value) {
			const text = String(value || '');
			if (!/^[a-z0-9](?:[a-z0-9.-]*[a-z0-9])?:[1-9][0-9]{0,4}=(?:(?:[0-9]{1,3}\.){3}[0-9]{1,3}|\[[0-9A-Fa-f:]+\])$/.test(text))
				return _('Use TLS-name:port=IPv4 or TLS-name:port=[IPv6].');
			return true;
		};

		o = s.taboption('advanced', form.Value, 'default_hop_limit', _('Default hop limit'));
		o.datatype = 'range(2,255)'; o.rmempty = false;
		o = s.taboption('advanced', form.Value, 'gateway_timeout_ms', _('Call timeout (ms)'));
		o.datatype = 'range(100,60000)'; o.rmempty = false;
		o = s.taboption('advanced', form.Value, 'max_inflight', _('Concurrent protocol calls'));
		o.datatype = 'range(1,1024)'; o.rmempty = false;
		o = s.taboption('advanced', form.Value, 'max_request_bytes', _('Maximum request bytes'));
		o.datatype = 'range(1024,1048576)'; o.rmempty = false;
		o = s.taboption('advanced', form.Value, 'max_response_bytes', _('Maximum response bytes'));
		o.datatype = 'range(1024,4194304)'; o.rmempty = false;
		o = s.taboption('advanced', form.Value, 'stream_idle_timeout_ms', _('Stream idle timeout (ms)'));
		o.datatype = 'range(1000,300000)'; o.rmempty = false;
		o = s.taboption('advanced', form.Value, 'max_stream_event_bytes', _('Maximum SSE event bytes'));
		o.datatype = 'range(256,1048576)'; o.rmempty = false;
		o = gatewayValue(s.taboption('advanced', form.Value, '_stream_resume_capacity', _('Retained resumable tasks')), 'stream_resume_capacity');
		o.datatype = 'range(1,4096)'; o.rmempty = false; o.placeholder = '512';
		o.depends('_stream_resume_enabled', '1');
		o.description = _('Maximum task-to-route bindings retained by this router. New resumable calls fail safely when the bound is full.');
		o = gatewayValue(s.taboption('advanced', form.Value, '_stream_resume_ttl_seconds', _('Resume window (seconds)')), 'stream_resume_ttl_seconds');
		o.datatype = 'range(1,86400)'; o.rmempty = false; o.placeholder = '300';
		o.depends('_stream_resume_enabled', '1');
		o.description = _('How long a disconnected or completed task remains available for replay. Called Agents should use an equal or longer history window.');

		s = m.section(form.GridSection, 'mapping', _('MCP/A2A capability mappings'));
		s.addremove = true;
		s.anonymous = true;
		s.nodescriptions = false;
		s.description = _('A2A setup assistant: click Add, select A2A, then complete the three required routing fields. After saving, Show Python example produces ready-to-edit caller and called-Agent code. Request prompts and arguments are never used to guess routing.');
		s.sectiontitle = function(sectionId) {
			const protocol = uci.get('agent_adapter', sectionId, 'protocol') || 'mapping';
			const authority = uci.get('agent_adapter', sectionId, 'authority') || 'identity';
			const selector = uci.get('agent_adapter', sectionId, 'selector') || 'operation';
			return protocol.toUpperCase() + ': ' + authority + ' / ' + selector;
		};

		o = s.option(form.Flag, 'enabled', _('Enabled'));
		o.rmempty = false; o.default = o.enabled;
		o = s.option(form.ListValue, 'protocol', _('Protocol'));
		o.value('mcp', _('MCP tools/call'));
		o.value('a2a', _('A2A message:send / message:stream'));
		o.rmempty = false;
		o.description = _('Choose A2A for an official Python A2A AgentExecutor, or MCP for a Tool server.');
		o = mappingComponent(s.option(form.Value, 'authority', _('Server / Agent Card ID')), _('Identity'));
		o.rmempty = false; o.placeholder = 'echo-card';
		o.description = _('For A2A, enter a short stable Card ID such as echo-card. The caller uses this in the router A2A URL.');
		o = mappingComponent(s.option(form.Value, 'selector', _('Tool name / A2A Skill ID')), _('Tool name / Skill ID'));
		o.rmempty = false; o.placeholder = 'echo';
		o.description = _('For A2A, this must equal the skill passed to agent.expose().');
		o = intentName(s.option(form.Value, 'intent', _('Capability intent')));
		o.rmempty = false; o.placeholder = 'chip.verilog.verify.lint';
		o.description = _('The normal Nexus capability route. The called Agent must expose the same intent.');
		o = s.option(form.Value, 'version', _('Intent version'));
		o.datatype = 'range(1,4294967295)'; o.default = '1'; o.rmempty = false;
		o.modalonly = true;

		o = s.option(form.Value, 'title', _('Tool title'));
		o.rmempty = true; o.modalonly = true;
		o.depends('protocol', 'mcp');
		o.description = _('Optional human-readable MCP tool title.');
		o = s.option(form.Value, 'description', _('Tool description'));
		o.rmempty = true; o.modalonly = true;
		o.depends('protocol', 'mcp');
		o.description = _('If empty, tools/list uses the capability intent as the description.');
		o = s.option(form.TextValue, 'input_schema_json', _('Tool input schema (JSON)'));
		o.rmempty = true; o.modalonly = true;
		o.depends('protocol', 'mcp');
		o.rows = 5;
		o.description = _('Optional JSON Schema object returned by tools/list. Empty mappings accept any JSON object for compatibility.');
		o.validate = function(sectionId, value) {
			if (!value) return true;
			try {
				const parsed = JSON.parse(value);
				return parsed && !Array.isArray(parsed) && typeof parsed === 'object'
					? true : _('Input schema must be a JSON object.');
			} catch (error) {
				return _('Input schema must be valid JSON.');
			}
		};

		o = s.option(form.DummyValue, '_a2a_edge_url', _('A2A caller URL'));
		o.depends('protocol', 'a2a');
		o.modalonly = true;
		o.cfgvalue = function(sectionId) { return a2aEdgeUrl(sectionId); };
		o.description = _('The SDK appends /message:send for send() or /message:stream for stream().');

		o = s.option(form.Button, '_a2a_python', _('Python SDK'));
		o.depends('protocol', 'a2a');
		o.inputtitle = _('Show Python example');
		o.inputstyle = 'apply';
		o.onclick = function(sectionId) { showA2AExample(sectionId); };

		o = s.option(form.DummyValue, '_mcp_edge_url', _('MCP caller URL'));
		o.depends('protocol', 'mcp');
		o.modalonly = true;
		o.cfgvalue = function(sectionId) { return mcpEdgeUrl(sectionId); };
		o.description = _('Use both application/json and text/event-stream in Accept to receive FastMCP progress and the final result.');

		o = s.option(form.Button, '_fastmcp_python', _('Python SDK'));
		o.depends('protocol', 'mcp');
		o.inputtitle = _('Show streaming FastMCP example');
		o.inputstyle = 'apply';
		o.onclick = function(sectionId) { showFastMCPExample(sectionId); };

		return m.render().then(function(node) {
			return E([], [
				E('link', { 'rel': 'stylesheet', 'href': L.resource('agent-router/agent-router.css') + '?v=#PKG_VERSION' }),
				mode.render('developer'), authenticationSummary(data), node
			]);
		});
	}
});
