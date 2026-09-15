package main

import (
	"crypto/rand"
	"encoding/base64"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"strconv"
	"testing"
	"time"
)

// Protocol GUI fixture contract, schema 1:
//   - Each peer is a CLIENT node. Snell version=4 uses serverVersion=5.
//   - Durations are Go/sing-box duration strings; serverPorts are "from:to"
//     ranges, including singleton ports. Omitted network means TCP and UDP.
//   - caText is the PEM certificate trusted by this test client, not a path or
//     a request to install anything in the Windows certificate store.
//   - Reuse, idle-session and BBR fields describe the client settings to test;
//     the inbound peer is ready to accept those settings.
//   - Creating <manifest>.stop ends the fixture. The manifest remains as an
//     audit artifact, so every subsequent fixture requires a new absolute path.
//
// All credentials are generated for this fixture and are never logged.
type protocolGUIPeer struct {
	Name                     string   `json:"name"`
	Type                     string   `json:"type"`
	Server                   string   `json:"server"`
	Port                     int      `json:"port"`
	Password                 string   `json:"password,omitempty"`
	PSK                      string   `json:"psk,omitempty"`
	Version                  int      `json:"version,omitempty"`
	ServerVersion            int      `json:"serverVersion,omitempty"`
	UserKey                  string   `json:"userkey,omitempty"`
	Reuse                    bool     `json:"reuse,omitempty"`
	SNI                      string   `json:"sni,omitempty"`
	CAText                   string   `json:"caText,omitempty"`
	ObfsType                 string   `json:"obfsType,omitempty"`
	ObfsPassword             string   `json:"obfsPassword,omitempty"`
	ObfsMode                 string   `json:"obfsMode,omitempty"`
	ObfsHost                 string   `json:"obfsHost,omitempty"`
	Mode                     string   `json:"mode,omitempty"`
	IdleSessionCheckInterval string   `json:"idleSessionCheckInterval,omitempty"`
	IdleSessionTimeout       string   `json:"idleSessionTimeout,omitempty"`
	MinIdleSession           int      `json:"minIdleSession,omitempty"`
	ClientMetadata           string   `json:"clientMetadata,omitempty"`
	BBRProfile               string   `json:"bbrProfile,omitempty"`
	ServerPorts              []string `json:"serverPorts,omitempty"`
	HopInterval              string   `json:"hopInterval,omitempty"`
	HopIntervalMax           string   `json:"hopIntervalMax,omitempty"`
	DisableChromeParrot      bool     `json:"disableChromeParrot,omitempty"`
	GeckoMinPacketSize       int      `json:"geckoMinPacketSize,omitempty"`
	GeckoMaxPacketSize       int      `json:"geckoMaxPacketSize,omitempty"`
}

type protocolGUIManifest struct {
	Schema    int               `json:"schema"`
	ExpiresAt string            `json:"expiresAt"`
	Peers     []protocolGUIPeer `json:"peers"`
}

func protocolGUISecret(t *testing.T) string {
	t.Helper()
	secret := make([]byte, 24)
	if _, err := rand.Read(secret); err != nil {
		t.Fatal(err)
	}
	return base64.RawURLEncoding.EncodeToString(secret)
}

func publishProtocolGUIManifest(path string, manifest protocolGUIManifest) error {
	content, err := json.MarshalIndent(manifest, "", "  ")
	if err != nil {
		return err
	}
	file, err := os.CreateTemp(filepath.Dir(path), "."+filepath.Base(path)+"-*.tmp")
	if err != nil {
		return err
	}
	temporary := file.Name()
	defer os.Remove(temporary)
	if _, err = file.Write(append(content, '\n')); err == nil {
		err = file.Sync()
	}
	closeErr := file.Close()
	if err != nil {
		return err
	}
	if closeErr != nil {
		return closeErr
	}
	// Same-directory hard-link publication is atomic and fails if the path
	// already exists. os.Rename can replace an existing destination on some
	// supported hosts and must not be used for this one-time handoff.
	if err := os.Link(temporary, path); err != nil {
		return fmt.Errorf("publish new manifest without replacing an existing path: %w", err)
	}
	return nil
}

func TestProtocolGuiPeers(t *testing.T) {
	path := os.Getenv("NEKORAY_PROTOCOL_GUI_FIXTURE")
	if path == "" {
		t.Skip("set NEKORAY_PROTOCOL_GUI_FIXTURE to a new absolute JSON path to run GUI peer servers")
	}
	if !filepath.IsAbs(path) {
		t.Fatal("NEKORAY_PROTOCOL_GUI_FIXTURE must be an absolute path")
	}
	path = filepath.Clean(path)
	for _, candidate := range []string{path, path + ".stop"} {
		if _, err := os.Lstat(candidate); err == nil {
			t.Fatal("fixture requires a new manifest and stop-signal path")
		} else if !os.IsNotExist(err) {
			t.Fatalf("inspect fixture path: %v", err)
		}
	}
	if deadline, exists := t.Deadline(); exists && time.Until(deadline) < 190*time.Second {
		t.Fatal("run the opt-in GUI fixture with -timeout 210s or longer")
	}
	if err := os.MkdirAll(filepath.Dir(path), 0700); err != nil {
		t.Fatal(err)
	}
	manifest := protocolGUIManifest{Schema: 1}
	for _, fixture := range []struct {
		name, protocol, mode string
	}{
		{"anytls", "anytls", ""},
		{"snell4", "snell", "none"},
		{"snell4-http", "snell", "http"},
		{"snell4-tls", "snell", "tls"},
		{"snell6", "snell", "default"},
		{"hysteria2-salamander", "hysteria2", "salamander"},
		{"hysteria2-gecko", "hysteria2", "gecko"},
	} {
		in, out := protocolPeerOptions(t, fixture.protocol)
		peer := protocolGUIPeer{
			Name: fixture.name, Type: fixture.protocol, Server: "127.0.0.1", Port: in["listen_port"].(int),
		}
		secret := protocolGUISecret(t)
		if fixture.protocol == "snell" {
			in["psk"], peer.PSK = secret, secret
			peer.Reuse = true
			if fixture.name == "snell6" {
				in["version"], peer.Version, peer.ServerVersion = 6, 6, 6
				in["mode"], peer.Mode = fixture.mode, fixture.mode
				peer.UserKey = protocolGUISecret(t)
				in["users"] = []any{map[string]any{"name": "gui-test", "userkey": peer.UserKey}}
			} else {
				peer.Version, peer.ServerVersion = 4, 5
				in["obfs_mode"], peer.ObfsMode = fixture.mode, fixture.mode
				if fixture.mode != "none" {
					peer.ObfsHost = "peer.example.test"
				}
			}
		} else {
			in["users"] = []any{map[string]any{"name": "gui-test", "password": secret}}
			peer.Password, peer.SNI = secret, "localhost"
			peer.CAText = out["tls"].(map[string]any)["certificate"].([]string)[0]
			if fixture.protocol == "anytls" {
				peer.IdleSessionCheckInterval, peer.IdleSessionTimeout = "10s", "15s"
				peer.MinIdleSession, peer.ClientMetadata = 1, "nekoray-gui-acceptance"
			} else {
				peer.ObfsType, peer.ObfsPassword = fixture.mode, protocolGUISecret(t)
				obfs := map[string]any{"type": peer.ObfsType, "password": peer.ObfsPassword}
				peer.BBRProfile = "conservative"
				if fixture.mode == "gecko" {
					peer.GeckoMinPacketSize, peer.GeckoMaxPacketSize = 600, 1300
					obfs["min_packet_size"], obfs["max_packet_size"] = peer.GeckoMinPacketSize, peer.GeckoMaxPacketSize
					peer.BBRProfile, peer.DisableChromeParrot = "aggressive", true
					port := strconv.Itoa(peer.Port)
					peer.ServerPorts = []string{port + ":" + port}
					peer.HopInterval, peer.HopIntervalMax = "5s", "6s"
				}
				in["obfs"] = obfs
			}
		}
		startProtocolPeerCore(t, in, true)
		manifest.Peers = append(manifest.Peers, peer)
	}
	expires := time.Now().Add(180 * time.Second)
	manifest.ExpiresAt = expires.UTC().Format(time.RFC3339)
	if err := publishProtocolGUIManifest(path, manifest); err != nil {
		t.Fatal(err)
	}
	t.Logf("GUI protocol fixture ready: %d loopback peers; stop-file or 180s lifetime", len(manifest.Peers))
	timer := time.NewTimer(time.Until(expires))
	defer timer.Stop()
	ticker := time.NewTicker(100 * time.Millisecond)
	defer ticker.Stop()
	for {
		select {
		case <-timer.C:
			t.Log("GUI protocol fixture lifetime ended")
			return
		case <-ticker.C:
			if _, err := os.Stat(path + ".stop"); err == nil {
				t.Log("GUI protocol fixture stop signal received")
				return
			} else if !os.IsNotExist(err) {
				t.Fatalf("read GUI fixture stop signal: %v", err)
			}
		}
	}
}
