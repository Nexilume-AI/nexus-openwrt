'use strict';
'require view';
'require form';
'require uci';
'require rpc';
'require ui';
'require agent-router.mode as mode';

const callNetworkDump = rpc.declare({ object: 'network.interface', method: 'dump', expect: { interface: [] } });
const callAgentStats = rpc.declare({ object: 'agent', method: 'stats', expect: {} });

function configuredOr(option, config, fallback) {
	option.cfgvalue = function(sectionId) {
		const value = uci.get(config, sectionId, this.option);
		return value === null || value === undefined || value === '' ? fallback : value;
	};
	return option;
}

function setIfEmpty(config, sectionId, option, value) {
	if (value !== '' && !uci.get(config, sectionId, option))
		uci.set(config, sectionId, option, value);
}

function isPublicIpv6(address) {
	const text = String(address || '').toLowerCase().split('%')[0];
	if (!text.includes(':') || text === '::' || text === '::1') return false;
	const first = parseInt(text.split(':')[0] || '0', 16);
	if (!Number.isFinite(first)) return false;
	if ((first & 0xfe00) === 0xfc00) return false;
	if ((first & 0xffc0) === 0xfe80) return false;
	if ((first & 0xff00) === 0xff00) return false;
	if (text.startsWith('2001:db8:') || text === '2001:db8::') return false;
	return true;
}

function detectPublicIpv6(network) {
	const interfaces = Array.isArray(network) ? network : ((network || {}).interface || []);
	const candidates = [];
	interfaces.forEach(function(iface) {
		if (!iface || iface.up === false) return;
		const name = String(iface.interface || iface.name || 'network');
		const priority = /^(wan6|wan)$/i.test(name) ? 0 : 1;
		const add = function(item, delegated) {
			const address = String((item || {}).address || '').split('%')[0];
			const mask = Number((item || {}).mask);
			if (!isPublicIpv6(address) || !Number.isInteger(mask) || mask < 48 || mask > 64) return;
			candidates.push({
				prefix: address + '/' + mask,
				interface: name,
				delegated: delegated,
				priority: priority
			});
		};
		(iface['ipv6-prefix'] || iface.ipv6_prefix || []).forEach(item => add(item, true));
	});
	candidates.sort(function(a, b) {
		if (a.delegated !== b.delegated) return a.delegated ? -1 : 1;
		if (a.priority !== b.priority) return a.priority - b.priority;
		return Number(a.prefix.split('/')[1]) - Number(b.prefix.split('/')[1]);
	});
	return candidates[0] || { prefix: '', interface: '', delegated: false };
}

function detectUpstreamOnlinkIpv6(network) {
	const interfaces = Array.isArray(network) ? network : ((network || {}).interface || []);
	const candidates = [];
	interfaces.forEach(function(iface) {
		if (!iface || iface.up === false) return;
		const name = String(iface.interface || iface.name || '');
		if (!/^(wan6|wan)$/i.test(name)) return;
		(iface['ipv6-address'] || iface.ipv6_address || []).forEach(function(item) {
			const address = String((item || {}).address || '').split('%')[0];
			const mask = Number((item || {}).mask);
			if (isPublicIpv6(address) && mask === 64)
				candidates.push({ prefix: address + '/64', interface: name, delegated: false });
		});
	});
	return candidates[0] || { prefix: '', interface: '', delegated: false };
}

function commaList(option, config) {
	option.cfgvalue = function(sectionId) {
		const value = uci.get(config, sectionId, this.option);
		if (Array.isArray(value)) return value;
		return String(value || '').split(',').map(v => v.trim()).filter(Boolean);
	};
	option.write = function(sectionId, value) {
		const joined = (value || []).map(v => String(v).trim()).filter(Boolean).join(',');
		if (joined) return uci.set(config, sectionId, this.option, joined);
		return uci.unset(config, sectionId, this.option);
	};
	option.remove = function(sectionId) { return uci.unset(config, sectionId, this.option); };
	return option;
}

function routerIdentifier(option) {
	option.validate = function(sectionId, value) {
		const text = String(value || '');
		if (text.length < 1 || text.length > 64 ||
			!/^[a-z0-9](?:[a-z0-9._-]*[a-z0-9])?$/.test(text))
			return _('请输入 1–64 位标识：使用小写字母、数字、点、下划线或连字符，并以字母或数字开头和结尾。');
		return true;
	};
	option.placeholder = 'router-a';
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
			uci.load('agent'),
			uci.load('agent_gateway'),
			uci.load('agent_adapter'),
			callNetworkDump().catch(() => ({ interface: [] })),
			callAgentStats().catch(() => ({}))
		]);
	},

	render(data) {
		let m, s, o, gatewayMap, gatewaySection, adapterMap, adapterSection;
		const detectedIpv6 = detectPublicIpv6(data[3]);
		const detectedOnlinkIpv6 = detectUpstreamOnlinkIpv6(data[3]);
		const agentStats = data[4] || {};
		const resolverAddress = String(agentStats.cross_discovery_resolver_ipv4 || '127.0.0.1');
		const resolverPort = Number(agentStats.cross_discovery_resolver_port || 0);
		const resolverEndpoint = resolverPort > 0 ? resolverAddress + ':' + resolverPort : '';
		const resolverAutomatic = (uci.get('agent', 'main', 'cross_discovery_resolver_mode') || 'auto') !== 'manual';
		const resolverDetected = agentStats.cross_discovery_resolver_detected === true ||
			agentStats.cross_discovery_resolver_detected === 1;
		const resolverReady = !resolverAutomatic || resolverDetected;
		m = new form.Map('agent', _('Advanced Agent Router Settings'),
			_('Expert control-plane, discovery, Relay and static-route settings. Use Quick Setup for common deployments. Save & Apply triggers the native procd reload path; invalid candidate configuration is rejected atomically by agentd.'));

		s = m.section(form.NamedSection, 'main', 'core', _('Control plane'));
		s.addremove = false;
		s.tab('identity', _('Identity & capacity'));
		s.tab('arpx', _('ARPX transport'));
		s.tab('discovery', _('LAN discovery'));
		s.tab('cross', _('Cross-domain discovery'));
		s.tab('relay', _('Open Mesh Relay'));
		s.tab('public_ipv6', _('Public Agent IPv6'));

		o = s.taboption('identity', form.Flag, 'enabled', _('Enable agentd'));
		o.rmempty = false;
		o = s.taboption('identity', form.ListValue, 'router_mesh_mode', _('Router Mesh'));
		o.value('open', _('Open distributed mesh (zero configuration)'));
		o.value('off', _('Managed peer trust'));
		o.rmempty = false;
		o.description = _('Open Mesh automatically admits LAN, static-seed and DNSSEC/SVCB Routers, exchanges Agent routes across domains, and keeps Cloud Relay trust isolated.');
		o = routerIdentifier(s.taboption('identity', form.Value, 'router_id', _('Router ID')));
		o.rmempty = false;
		o = s.taboption('identity', form.Value, 'domain_id', _('Agent domain'));
		o.datatype = 'hostname'; o.rmempty = false;
		o = s.taboption('identity', form.Value, 'max_routes', _('Maximum ARIB routes'));
		o.datatype = 'range(1,100000)'; o.rmempty = false;
		o = s.taboption('identity', form.Value, 'default_lease_seconds', _('Default lease (seconds)'));
		o.datatype = 'range(5,3600)'; o.rmempty = false;

		o = s.taboption('arpx', form.Flag, 'peer_transport_enabled', _('Enable outbound peer transport'));
		o.rmempty = false;
		o = s.taboption('arpx', form.Flag, 'peer_listener_enabled', _('Enable inbound peer listener'));
		o.rmempty = false;
		o = s.taboption('arpx', form.Flag, 'reflector_enabled', _('Reflect learned routes'));
		o.rmempty = false;
		o = s.taboption('arpx', form.Value, 'peer_listen_ipv4', _('Listen IPv4'));
		o.datatype = 'ip4addr'; o.rmempty = false;
		o = s.taboption('arpx', form.Value, 'peer_listen_port', _('Listen port'));
		o.datatype = 'port'; o.rmempty = false;
		o = s.taboption('arpx', form.Value, 'peer_max_inbound', _('Maximum inbound sessions'));
		o.datatype = 'range(1,128)'; o.rmempty = false;
		o = s.taboption('arpx', form.Flag, 'relay_tunnel_enabled', _('Enable shared Invoke tunnel'));
		o.rmempty = false;

		o = s.taboption('discovery', form.Flag, 'discovery_enabled', _('Consume LAN DNS-SD'));
		o.rmempty = false;
		o = s.taboption('discovery', form.Flag, 'discovery_publish_enabled', _('Publish router DNS-SD record'));
		o.rmempty = false;
		o = s.taboption('discovery', form.ListValue, 'lan_auto_promotion_mode', _('Zero-configuration admission'));
		o.value('off', _('Off')); o.value('same-domain', _('Same domain')); o.value('allowlist', _('Allowlist')); o.value('all', _('All validated LAN routers'));
		o.rmempty = false;
		o = commaList(s.taboption('discovery', form.DynamicList, 'lan_auto_promotion_allowlist', _('Router allowlist')), 'agent');
		o.placeholder = 'router-branch-01';
		o.description = _('One Router ID per entry.');
		o.depends('lan_auto_promotion_mode', 'allowlist');
		o = s.taboption('discovery', form.Value, 'lan_auto_promotion_grace_seconds', _('Graceful restart (seconds)'));
		o.datatype = 'range(5,300)'; o.rmempty = false;

		o = s.taboption('cross', form.Flag, 'cross_discovery_enabled', _('Enable DNS SVCB discovery'));
		o.rmempty = false;
		o = s.taboption('cross', form.Value, 'cross_discovery_domain', _('Remote domain'));
		o.datatype = 'hostname';
		o.description = _('The remote Nexus Router administrative DNS domain. The router queries _agents.<domain>, not an individual Agent ID or URL.');
		o = s.taboption('cross', form.DummyValue, '_detected_cross_resolver', _('Local DNSSEC resolver'));
		o.cfgvalue = function() {
			if (!resolverEndpoint) return _('Available after agentd starts');
			return resolverReady ? resolverEndpoint : _('No local resolver detected');
		};
		o.description = resolverEndpoint && resolverReady ?
			(resolverAutomatic ?
				_('Selected automatically by the native service. DNS responses are still accepted only when the local validator sets the authenticated-data flag.') :
				_('Using the manual local endpoint below. DNS responses are still accepted only when the resolver sets the authenticated-data flag.')) :
			_('No effective resolver is currently reported. Start agentd after installing or enabling a local DNSSEC-validating resolver.');
		o = configuredOr(s.taboption('cross', form.ListValue, 'cross_discovery_resolver_mode', _('Resolver configuration')), 'agent', 'auto');
		o.value('auto', _('Automatic (recommended)'));
		o.value('manual', _('Manual override'));
		o.rmempty = false;
		o.description = _('Automatic mode detects a listening local Unbound validator, then a DNSSEC-enabled dnsmasq. Manual values are intended only for non-standard local resolver ports.');
		o = s.taboption('cross', form.Value, 'cross_discovery_resolver_ipv4', _('DNSSEC resolver IPv4'));
		o.datatype = 'ip4addr'; o.rmempty = false;
		o.depends('cross_discovery_resolver_mode', 'manual');
		o.description = _('Advanced override. Only a numeric 127.0.0.0/8 loopback address is accepted. This is not the remote router address.');
		o = s.taboption('cross', form.Value, 'cross_discovery_resolver_port', _('Resolver port'));
		o.datatype = 'port'; o.rmempty = false;
		o.depends('cross_discovery_resolver_mode', 'manual');
		o.description = _('Advanced override for the local DNSSEC validator listening port.');
		o = s.taboption('cross', form.ListValue, 'card_authorization_mode', _('Agent Card authorization'));
		o.value('off', _('Off')); o.value('same-domain', _('Same domain')); o.value('allowlist', _('Allowlist')); o.value('all-signed', _('All signed')); o.value('directory-trusted', _('Directory trusted'));
		o.rmempty = false;
		o = commaList(s.taboption('cross', form.DynamicList, 'card_authorization_allowlist', _('Agent Card router allowlist')), 'agent');
		o.placeholder = 'router-partner-01';
		o.description = _('One Router ID per entry.');
		o.depends('card_authorization_mode', 'allowlist');

		o = s.taboption('relay', form.DummyValue, '_open_mesh_relay_status', _('Open Mesh Relay status'));
		o.cfgvalue = function() {
			if (agentStats.open_mesh_relay_assignment_active) return _('Connected');
			if (agentStats.open_mesh_relay_bootstrap_enabled) return _('Connecting');
			return _('Disabled');
		};
		o.description = _('Cloud Relay status remains on the Cloud page.');
		o = s.taboption('relay', form.Flag, 'open_mesh_relay_enabled', _('Enable self-hosted Open Mesh Relay'));
		o.rmempty = false;
		o.description = _('This setting only controls a self-hosted OpenWrt Open Mesh seed. Nexus Cloud Relay is managed independently from the Cloud page.');
		o = commaList(s.taboption('relay', form.DynamicList, 'open_mesh_directory_endpoints', _('Open Mesh Directory URLs')), 'agent');
		o.depends('open_mesh_relay_enabled', '1');
		o.placeholder = 'https://directory-seed.mesh.local:18443/v1/open-mesh/assignment';
		o.description = _('Self-hosted Open Mesh Directory seeds, maximum four. Cloud-managed Relay endpoints are not shown or modified here.');
		o = commaList(s.taboption('relay', form.DynamicList, 'open_mesh_directory_connect_ipv4s', _('Optional fixed Directory IPv4 addresses')), 'agent');
		o.depends('open_mesh_relay_enabled', '1');
		o.allowduplicates = true;
		o.placeholder = '203.0.113.20';
		o.description = _('Advanced override. Leave empty for DNS. If set, provide exactly one IPv4 address for each Directory URL in the same order. TLS still verifies the hostname from the URL.');
		o = s.taboption('relay', form.Value, 'open_mesh_directory_poll_ms', _('Assignment poll (ms)'));
		o.depends('open_mesh_relay_enabled', '1');
		o.datatype = 'range(1000,3600000)'; o.rmempty = false;
		o = s.taboption('relay', form.Value, 'open_mesh_directory_timeout_ms', _('Directory timeout (ms)'));
		o.depends('open_mesh_relay_enabled', '1');
		o.datatype = 'range(100,60000)'; o.rmempty = false;

		o = configuredOr(s.taboption('public_ipv6', form.ListValue, 'public_ipv6_mode', _('IPv6 address source')), 'agent', 'auto');
		o.value('auto', _('Automatic (recommended)'));
		o.value('routed-prefix', _('Manual routed prefix'));
		o.value('upstream-relay', _('Manual upstream /64 relay'));
		o.rmempty = false;
		o.description = _('Automatic mode continuously prefers a delegated prefix and otherwise uses a verified WAN on-link /64. Network changes are reconciled by the Router; no prefix, interface or port is required.');
		o.write = function(sectionId, value) {
			uci.set('agent', sectionId, 'public_ipv6_mode', value);
			if (value !== 'auto') {
				const detected = value === 'upstream-relay' ? detectedOnlinkIpv6 : detectedIpv6;
				if (detected.prefix)
					uci.set('agent', sectionId, 'public_ipv6_prefix', detected.prefix);
				if (value === 'upstream-relay')
					setIfEmpty('agent', sectionId, 'public_ipv6_upstream_interface', detected.interface || 'wan6');
			}
		};

		o = s.taboption('public_ipv6', form.Flag, 'public_ipv6_enabled', _('Automatically provide public IPv6 to Agents'));
		o.rmempty = false;
		o.description = _('Enabled by default. Agents that request public_ipv6="auto" receive a bounded router-owned /128 when safe global IPv6 connectivity is available. Invalid or private prefixes fail closed and recover automatically after the network is repaired.');
		o.write = function(sectionId, value) {
			uci.set('agent', sectionId, 'public_ipv6_enabled', value);
			if (value === '1') {
				setIfEmpty('agent', sectionId, 'public_ipv6_max_addresses', '256');
				setIfEmpty('agent', sectionId, 'public_ipv6_interface', 'nexus-agent0');
			}
		};
		o = s.taboption('public_ipv6', form.DummyValue, '_detected_public_ipv6', _('Detected routed/PD prefix'));
		o.cfgvalue = function() { return detectedIpv6.prefix || _('No delegated /48 to /64 prefix detected'); };
		o.description = detectedIpv6.prefix ?
			_('Detected automatically from %s for routed-prefix mode.').format(detectedIpv6.interface) :
			_('No PD was received. Choose upstream relay if WAN6 has an on-link /64.');
		o = s.taboption('public_ipv6', form.DummyValue, '_detected_onlink_ipv6', _('Detected upstream on-link /64'));
		o.cfgvalue = function() { return detectedOnlinkIpv6.prefix || _('No usable WAN6 /64 address detected'); };
		o.description = detectedOnlinkIpv6.prefix ?
			_('Available from %s for no-PD upstream relay mode.').format(detectedOnlinkIpv6.interface) :
			_('The router must first receive a global on-link /64 from the upstream network.');
		o = s.taboption('public_ipv6', form.Value, 'public_ipv6_prefix', _('Agent IPv6 source prefix'));
		o.cfgvalue = function(sectionId) {
			const configured = uci.get('agent', sectionId, this.option);
			if (configured) return configured;
			const mode = uci.get('agent', sectionId, 'public_ipv6_mode') || 'auto';
			return mode === 'upstream-relay' ? detectedOnlinkIpv6.prefix : detectedIpv6.prefix;
		};
		o.placeholder = '2001:db8:1234:5600::/56';
		o.depends({ 'public_ipv6_enabled': '1', 'public_ipv6_mode': 'routed-prefix' });
		o.depends({ 'public_ipv6_enabled': '1', 'public_ipv6_mode': 'upstream-relay' });
		o.rmempty = false;
		o.description = _('Manual override only. Automatic mode ignores this stored value and derives a verified runtime prefix.');
		o.validate = function(sectionId, value) {
			const text = String(value || '').trim();
			const match = text.match(/^([0-9a-fA-F:]+)\/(\d{1,3})$/);
			if (!match || Number(match[2]) < 48 || Number(match[2]) > 64)
				return _('Enter a routed IPv6 prefix between /48 and /64, for example 2001:db8:1234:5600::/56.');
			if ((uci.get('agent', sectionId, 'public_ipv6_mode') || 'auto') === 'upstream-relay' && Number(match[2]) !== 64)
				return _('Upstream relay requires the on-link /64 advertised by the upstream router.');
			return true;
		};
		o = configuredOr(s.taboption('public_ipv6', form.Value, 'public_ipv6_upstream_interface', _('Upstream IPv6 interface')), 'agent', detectedOnlinkIpv6.interface || 'wan6');
		o.placeholder = 'wan6'; o.rmempty = false;
		o.depends({ 'public_ipv6_enabled': '1', 'public_ipv6_mode': 'upstream-relay' });
		o.description = _('Logical OpenWrt interface whose resolved device receives the upstream /64. The privileged helper may proxy only exact active Agent /128 leases on this device.');
		o = configuredOr(s.taboption('public_ipv6', form.Value, 'public_ipv6_max_addresses', _('Maximum active Agent addresses')), 'agent', '256');
		o.datatype = 'range(1,65535)'; o.rmempty = false;
		o.depends('public_ipv6_enabled', '1');
		o.description = _('Automatically defaults to 256 to keep address and connection state bounded.');
		o = configuredOr(s.taboption('public_ipv6', form.Value, 'public_ipv6_interface', _('Dedicated virtual interface')), 'agent', 'nexus-agent0');
		o.placeholder = 'nexus-agent0'; o.rmempty = false;
		o.depends('public_ipv6_enabled', '1');
		o.description = _('Automatically uses nexus-agent0. No default route or WAN address is changed.');

		s = m.section(form.GridSection, 'route', _('Static capability routes'));
		s.addremove = true;
		s.anonymous = false;
		s.nodescriptions = true;
		o = s.option(form.Flag, 'enabled', _('Enabled'));
		o.rmempty = false; o.default = o.enabled;
		o = s.option(form.Value, 'route_id', _('Route ID'));
		o.datatype = 'hexstring'; o.rmempty = false;
		o = s.option(form.Value, 'intent', _('Intent'));
		o.rmempty = false;
		o = s.option(form.Value, 'version', _('Version'));
		o.datatype = 'uinteger'; o.rmempty = false;
		o = s.option(form.Value, 'origin', _('Origin agent'));
		o.rmempty = false;
		o = s.option(form.Value, 'endpoint', _('Invoke endpoint'));
		o.placeholder = 'https://127.0.0.1:9001/invoke'; o.rmempty = false;
		o = s.option(form.Value, 'tenant', _('Tenant'));
		o.rmempty = false;
		o = s.option(form.Value, 'region', _('Region'));
		o.rmempty = false;
		o = s.option(form.Value, 'cost_microunits', _('Cost'));
		o.datatype = 'uinteger'; o.rmempty = false;
		o = s.option(form.Value, 'latency_ms', _('Latency (ms)'));
		o.datatype = 'uinteger'; o.rmempty = false;
		o = s.option(form.Value, 'trust', _('Trust'));
		o.datatype = 'range(0,100)'; o.rmempty = false;
		o = s.option(form.Value, 'load_permille', _('Load (‰)'));
		o.datatype = 'range(0,1000)'; o.rmempty = false;
		o = s.option(form.Value, 'hop_count', _('Hop count'));
		o.datatype = 'range(0,32)'; o.rmempty = false;

		gatewayMap = new form.Map('agent_gateway', _('Public IPv6 ingress readiness'),
			_('These controls complete the data path for an Agent public /128. Authentication can use JWT or the explicit No JWT mode. The destination /128 remains bound to the local Agent route in either mode.'));
		gatewaySection = gatewayMap.section(form.NamedSection, 'main', 'gateway', _('Public IPv6 entry point'));
		gatewaySection.addremove = false;

		o = gatewaySection.option(form.ListValue, 'public_transport', _('Direct IPv6 transport'));
		o.value('disabled', _('Disabled'));
		o.value('mtls', _('HTTPS with mutual TLS (recommended)'));
		o.value('http', _('Plain HTTP (no TLS)'));
		o.value('auto', _('Legacy automatic mode'));
		o.default = 'auto';
		o.rmempty = false;
		o.description = _('Plain HTTP sends the Agent payload unencrypted. It is explicit and the SDK never falls back to it after a TLS failure.');
		o.write = function(sectionId, value) {
			uci.set('agent_gateway', sectionId, 'public_transport', value);
			uci.set('agent_gateway', sectionId, 'mtls_enabled', value === 'mtls' ? '1' : '0');
			if (value === 'mtls' || value === 'http') {
				setIfEmpty('agent_gateway', sectionId, 'mtls_listen', '[::]:7443');
				setIfEmpty('agent_gateway', sectionId, 'public_ingress_port', '7443');
				setIfEmpty('agent_gateway', sectionId, 'public_ingress_max_connections', '32');
			}
		};

		o = gatewaySection.option(form.Flag, 'invoke_enabled', _('Allow Agents to call other Agents'));
		o.rmempty = false;
		o.description = _('Required for public HTTP, SSE, MCP and A2A invocation. Registration remains a separate control.');
		o = gatewaySection.option(form.ListValue, 'auth_mode', _('Agent call authentication'));
		o.value('none', _('No JWT'));
		o.value('jwt', _('JWT'));
		o.default = 'none';
		o.rmempty = false;
		o.description = _('No JWT permits calls without a token on this public /128 entry. Configure JWT details under Agent APIs & Protocols when JWT is selected.');
		o.cfgvalue = function(sectionId) {
			const value = uci.get('agent_gateway', sectionId, 'auth_mode');
			if (value === 'none' || value === 'jwt')
				return value;
			return uci.get('agent_gateway', sectionId, 'jwt_required') === '1' ? 'jwt' : 'none';
		};
		o.write = function(sectionId, value) {
			uci.set('agent_gateway', sectionId, 'auth_mode', value);
			uci.set('agent_gateway', sectionId, 'jwt_required', value === 'jwt' ? '1' : '0');
			if (value === 'none')
				uci.set('agent_gateway', sectionId, 'forwarding_assertion_enabled', '0');
		};
		o = gatewaySection.option(form.Flag, 'stream_enabled', _('Allow streaming and reconnect'));
		o.rmempty = false;
		o.description = _('Enables SSE streaming. A reconnect remains pinned to the same public /128 and Agent route.');
		o = configuredOr(gatewaySection.option(form.Value, 'mtls_listen', _('Public listener address')), 'agent_gateway', '[::]:7443');
		o.placeholder = '[::]:7443';
		o.rmempty = false;
		o.depends('public_transport', 'mtls');
		o.validate = function(sectionId, value) {
			const match = String(value || '').trim().match(/^\[([0-9a-fA-F:]+)\]:(\d{1,5})$/);
			if (!match || Number(match[2]) < 1 || Number(match[2]) > 65535)
				return _('Enter an IPv6 listener and port, for example [::]:7443.');
			return true;
		};
		o = configuredOr(gatewaySection.option(form.Value, 'public_ingress_port', _('Destination port on every Agent /128')), 'agent_gateway', '7443');
		o.datatype = 'port';
		o.rmempty = false;
		o.depends('public_transport', 'mtls');
		o.depends('public_transport', 'http');
		o.description = _('Normally this is the same port as the public listener. The original destination /128 selects the Agent; the URL path selects HTTP/SSE, MCP or A2A.');
		o = gatewaySection.option(form.Flag, 'public_descriptor_enabled', _('Publish a direct connection descriptor'));
		o.rmempty = false;
		o.depends('public_transport', 'mtls');
		o.depends('public_transport', 'http');
		o.description = _('Adds the selected HTTP(S) scheme and destination port to public IPv6 registration responses. HTTPS also publishes the certificate identity and CA bundle label.');
		o = gatewaySection.option(form.Value, 'public_tls_server_name', _('TLS certificate identity'));
		o.placeholder = 'router-a.example.com';
		o.rmempty = false;
		o.depends({ 'public_descriptor_enabled': '1', 'public_transport': 'mtls' });
		o.description = _('Enter a DNS name present in the public listener certificate Subject Alternative Name. It is verified even though TCP connects to the Agent IPv6 address.');
		o.validate = function(sectionId, value) {
			const text = String(value || '').trim();
			if (!text || text.length > 253 || !/^[A-Za-z0-9-]+(?:\.[A-Za-z0-9-]+)*$/.test(text))
				return _('Enter the exact DNS identity from the public TLS certificate, for example router-a.example.com.');
			return true;
		};
		o = gatewaySection.option(form.Value, 'public_ca_bundle_id', _('Caller CA bundle label'));
		o.placeholder = 'enterprise-agent-ca-v1';
		o.rmempty = false;
		o.depends({ 'public_descriptor_enabled': '1', 'public_transport': 'mtls' });
		o.description = _('A portable label callers map to their local trusted CA file. This is not a filesystem path and does not expose certificate contents.');
		o.validate = function(sectionId, value) {
			const text = String(value || '').trim();
			if (!text || text.length > 63 || !/^[A-Za-z0-9._-]+$/.test(text))
				return _('Use 1–63 letters, numbers, dots, underscores or hyphens.');
			return true;
		};
		o = configuredOr(gatewaySection.option(form.Value, 'public_ingress_max_connections', _('Maximum concurrent public connections')), 'agent_gateway', '32');
		o.datatype = 'range(1,128)';
		o.rmempty = false;
		o.depends('public_transport', 'mtls');
		o.depends('public_transport', 'http');

		adapterMap = new form.Map('agent_adapter', _('MCP and A2A public ingress'),
			_('Enable this adapter when callers use MCP or A2A. Plain HTTP and SSE invocation do not require it. Capability mappings still determine which protocol method maps to each Nexus intent.'));
		adapterSection = adapterMap.section(form.NamedSection, 'main', 'adapter', _('Protocol adapter'));
		adapterSection.addremove = false;
		o = adapterSection.option(form.Flag, 'enabled', _('Enable MCP and A2A adapter'));
		o.rmempty = false;
		o = adapterSection.option(form.Flag, 'stream_enabled', _('Allow MCP and A2A streaming'));
		o.rmempty = false;
		o.depends('enabled', '1');
		o.description = _('Use together with gateway streaming for streamed calls and reconnect.');

		return Promise.all([m.render(), gatewayMap.render(), adapterMap.render()])
			.then(nodes => E([], [
				E('link', { 'rel': 'stylesheet', 'href': L.resource('agent-router/agent-router.css') + '?v=#PKG_VERSION' }),
				mode.render('developer')
			].concat(nodes)));
	}
});
