package compat

import (
	"fmt"
	"net"
	"net/url"
	"strconv"
	"strings"
)

func (m *migrator) dns() error {
	dns := obj(m.root["dns"])
	if dns == nil {
		return nil
	}
	fake := obj(dns["fakeip"])
	delete(dns, "fakeip")
	var servers []any
	var legacyFinal string
	for n, value := range list(dns["servers"]) {
		server := obj(value)
		if server == nil {
			return fmt.Errorf("dns.servers[%d] must be an object", n)
		}
		if str(server["type"]) != "" && str(server["type"]) != "legacy" {
			servers = append(servers, server)
			continue
		}
		address, tag := str(server["address"]), str(server["tag"])
		if tag == "" {
			tag = fmt.Sprintf("nekoray-dns-%d", n)
			server["tag"] = tag
		}
		if legacyFinal == "" {
			legacyFinal = tag
		}
		m.legacy = true
		m.dnsStrategies[tag] = str(server["strategy"])
		if strings.HasPrefix(address, "rcode://") {
			codes := map[string]string{"success": "NOERROR", "format_error": "FORMERR", "server_failure": "SERVFAIL", "name_error": "NXDOMAIN", "not_implemented": "NOTIMP", "refused": "REFUSED"}
			code, ok := codes[strings.TrimPrefix(address, "rcode://")]
			if !ok {
				return fmt.Errorf("unsupported legacy DNS response code %q", address)
			}
			m.dnsRcodes[tag] = code
			continue
		}
		if address == "fakeip" {
			server = object{"type": "fakeip", "tag": tag}
			for _, key := range []string{"inet4_range", "inet6_range"} {
				copyIfPresent(server, key, fake, key)
			}
			if server["inet4_range"] == nil && server["inet6_range"] == nil {
				server["inet4_range"] = "198.18.0.0/15"
				server["inet6_range"] = "fc00::/18"
			}
			m.fakeServers[tag] = true
		} else {
			if err := migrateDNSServer(server); err != nil {
				return fmt.Errorf("DNS server %s: %w", tag, err)
			}
			detour := str(server["detour"])
			for _, outbound := range list(m.root["outbounds"]) {
				out := obj(outbound)
				if out != nil && str(out["tag"]) == detour && str(out["type"]) == "direct" {
					delete(server, "detour")
					for _, key := range []string{"bind_interface", "inet4_bind_address", "inet6_bind_address", "routing_mark", "protect_path", "connect_timeout"} {
						if server[key] == nil {
							copyIfPresent(server, key, out, key)
						}
					}
				}
			}
		}
		servers = append(servers, server)
	}
	if _, exists := dns["servers"]; exists {
		dns["servers"] = servers
	}
	final := str(dns["final"])
	if final == "" {
		final = legacyFinal
	}
	if code, exists := m.dnsRcodes[final]; exists {
		appendValues(dns, "rules", object{"action": "predefined", "rcode": code})
		delete(dns, "final")
	} else if strategy := m.dnsStrategies[final]; strategy != "" && dns["strategy"] == nil {
		dns["strategy"] = strategy
	}
	return nil
}

func migrateDNSServer(server object) error {
	address := str(server["address"])
	resolver, resolverStrategy := str(server["address_resolver"]), str(server["address_strategy"])
	for _, key := range []string{"address", "address_resolver", "address_strategy", "strategy"} {
		delete(server, key)
	}
	if resolver != "" {
		value := object{"server": resolver}
		if resolverStrategy != "" {
			value["strategy"] = resolverStrategy
		}
		server["domain_resolver"] = value
	}
	if address == "local" || address == "localhost" || strings.HasPrefix(address, "underlying://") {
		server["type"] = "local"
		delete(server, "domain_resolver")
		if strings.HasPrefix(address, "underlying://") {
			server["prefer_go"] = true
		}
		return nil
	}
	if strings.HasPrefix(address, "dhcp://") {
		server["type"] = "dhcp"
		name := strings.TrimPrefix(address, "dhcp://")
		if name != "" && name != "auto" {
			server["interface"] = name
		}
		delete(server, "domain_resolver")
		return nil
	}
	address = strings.Replace(address, "+local://", "://", 1)
	scheme := "udp"
	if !strings.Contains(address, "://") {
		host := address
		if net.ParseIP(host) != nil && strings.Contains(host, ":") {
			address = "[" + host + "]"
		}
		address = "udp://" + address
	}
	u, err := url.Parse(address)
	if err != nil {
		return err
	}
	scheme = u.Scheme
	if u.Hostname() == "" {
		return fmt.Errorf("missing DNS server address")
	}
	switch scheme {
	case "udp", "tcp", "tls", "https", "quic", "h3":
	default:
		return fmt.Errorf("unsupported DNS scheme %q", scheme)
	}
	server["type"], server["server"] = scheme, u.Hostname()
	if port := u.Port(); port != "" {
		number, err := strconv.ParseUint(port, 10, 16)
		if err != nil || number == 0 {
			return fmt.Errorf("invalid DNS port %q", port)
		}
		server["server_port"] = number
	}
	if scheme == "tls" || scheme == "https" || scheme == "quic" || scheme == "h3" {
		tls := object{"enabled": true}
		if net.ParseIP(u.Hostname()) == nil {
			tls["server_name"] = u.Hostname()
		}
		server["tls"] = tls
	}
	if (scheme == "https" || scheme == "h3") && u.RequestURI() != "" && u.RequestURI() != "/" {
		server["path"] = u.RequestURI()
	}
	return nil
}
