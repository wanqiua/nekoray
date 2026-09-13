package compat

import (
	"bufio"
	"encoding/binary"
	"fmt"
	"io"
	"os"
	"strings"

	"github.com/oschwald/maxminddb-golang"
)

// Read the version-0 geosite.db format from sing-box <= 1.11. No old core is
// linked. Existing local databases are expanded into native matching fields.
type siteEntry struct{ offset, count uint64 }
type siteItem struct {
	kind  byte
	value string
}
type geoData struct {
	sitePath, ipPath string
	site             *os.File
	base             int64
	index            map[string]siteEntry
	sites            map[string][]siteItem
	countries        map[string][]any
}

func newGeoData(route object) *geoData {
	g := &geoData{sitePath: "geosite.db", ipPath: "geoip.db", sites: map[string][]siteItem{}}
	if value := str(obj(route["geosite"])["path"]); value != "" {
		g.sitePath = value
	}
	if value := str(obj(route["geoip"])["path"]); value != "" {
		g.ipPath = value
	}
	return g
}
func (g *geoData) close() {
	if g.site != nil {
		g.site.Close()
	}
}

func readString(reader *bufio.Reader) (string, error) {
	size, err := binary.ReadUvarint(reader)
	if err != nil {
		return "", err
	}
	if size > 1024*1024 {
		return "", fmt.Errorf("invalid geosite string length")
	}
	value := make([]byte, int(size))
	_, err = io.ReadFull(reader, value)
	return string(value), err
}

func (g *geoData) openSite() error {
	f, err := os.Open(g.sitePath)
	if err != nil {
		return fmt.Errorf("open legacy geosite database: %w", err)
	}
	g.site = f
	reader := bufio.NewReader(f)
	version, err := reader.ReadByte()
	if err != nil {
		return err
	}
	if version != 0 {
		return fmt.Errorf("unsupported geosite database version %d", version)
	}
	count, err := binary.ReadUvarint(reader)
	if err != nil {
		return err
	}
	if count > 1000000 {
		return fmt.Errorf("invalid geosite entry count")
	}
	g.index = make(map[string]siteEntry, int(count))
	for n := uint64(0); n < count; n++ {
		code, err := readString(reader)
		if err != nil {
			return err
		}
		offset, err := binary.ReadUvarint(reader)
		if err != nil {
			return err
		}
		length, err := binary.ReadUvarint(reader)
		if err != nil {
			return err
		}
		g.index[code] = siteEntry{offset, length}
	}
	position, err := f.Seek(0, io.SeekCurrent)
	if err != nil {
		return err
	}
	g.base = position - int64(reader.Buffered())
	return nil
}

func (g *geoData) readSite(code string) ([]siteItem, error) {
	if items, exists := g.sites[code]; exists {
		return items, nil
	}
	if g.site == nil {
		if err := g.openSite(); err != nil {
			return nil, err
		}
	}
	entry, exists := g.index[code]
	if !exists {
		return nil, fmt.Errorf("geosite category %q is missing from %s", code, g.sitePath)
	}
	if entry.count > 10000000 || entry.offset > 1<<40 {
		return nil, fmt.Errorf("invalid geosite entry")
	}
	if _, err := g.site.Seek(g.base+int64(entry.offset), io.SeekStart); err != nil {
		return nil, err
	}
	reader := bufio.NewReader(g.site)
	items := make([]siteItem, 0, int(entry.count))
	for n := uint64(0); n < entry.count; n++ {
		kind, err := reader.ReadByte()
		if err != nil {
			return nil, err
		}
		value, err := readString(reader)
		if err != nil {
			return nil, err
		}
		if kind > 3 {
			return nil, fmt.Errorf("unknown geosite rule type %d", kind)
		}
		items = append(items, siteItem{kind, value})
	}
	g.sites[code] = items
	return items, nil
}

func (g *geoData) openIP() error {
	db, err := maxminddb.Open(g.ipPath)
	if err != nil {
		return fmt.Errorf("open legacy geoip database: %w", err)
	}
	defer db.Close()
	if db.Metadata.DatabaseType != "sing-geoip" {
		return fmt.Errorf("expected sing-geoip database, got %s", db.Metadata.DatabaseType)
	}
	g.countries = map[string][]any{}
	networks := db.Networks(maxminddb.SkipAliasedNetworks)
	for networks.Next() {
		var country string
		network, err := networks.Network(&country)
		if err != nil {
			return err
		}
		g.countries[country] = append(g.countries[country], network.String())
	}
	return networks.Err()
}

func (g *geoData) expand(rule object) error {
	for _, value := range list(rule["geosite"]) {
		code := strings.ToLower(str(value))
		items, err := g.readSite(code)
		if err != nil {
			return err
		}
		keys := []string{"domain", "domain_suffix", "domain_keyword", "domain_regex"}
		for _, item := range items {
			appendValues(rule, keys[item.kind], item.value)
		}
	}
	delete(rule, "geosite")
	for _, key := range []string{"geoip", "source_geoip"} {
		cidrKey, privateKey := "ip_cidr", "ip_is_private"
		if key == "source_geoip" {
			cidrKey, privateKey = "source_ip_cidr", "source_ip_is_private"
		}
		for _, value := range list(rule[key]) {
			code := strings.ToLower(str(value))
			if code == "private" {
				rule[privateKey] = true
				continue
			}
			if g.countries == nil {
				if err := g.openIP(); err != nil {
					return err
				}
			}
			prefixes, exists := g.countries[code]
			if !exists {
				return fmt.Errorf("geoip category %q is missing from %s", code, g.ipPath)
			}
			appendValues(rule, cidrKey, prefixes...)
		}
		delete(rule, key)
	}
	return nil
}
