//go:build with_quic

package main

import (
	"crypto/sha256"
	stdtls "crypto/tls"
	"crypto/x509"
	"encoding/hex"
	"encoding/pem"
	"errors"
	"fmt"
	"strings"
	"testing"
)

func TestHysteria2CertificatePin(t *testing.T) {
	for _, variant := range []string{"trusted", "insecure", "colon", "gecko_hopping", "self_signed", "self_signed_no_sni"} {
		t.Run(variant, func(t *testing.T) {
			in, out := protocolPeerOptions(t, "hysteria2")
			tls := out["tls"].(map[string]any)
			certificate, _ := pem.Decode([]byte(tls["certificate"].([]string)[0]))
			pin := sha256.Sum256(certificate.Bytes)
			fingerprint := hex.EncodeToString(pin[:])
			if variant != "trusted" {
				delete(tls, "certificate")
				tls["insecure"] = true
			}
			if variant == "colon" {
				parts := make([]string, len(pin))
				for n, value := range pin {
					parts[n] = fmt.Sprintf("%02X", value)
				}
				fingerprint = strings.Join(parts, ":")
			}
			if variant == "gecko_hopping" {
				in["obfs"] = map[string]any{"type": "gecko", "password": "pin-fixture-obfs"}
				out["obfs"] = map[string]any{"type": "gecko", "password": "pin-fixture-obfs", "min_packet_size": 600, "max_packet_size": 1300}
				port := out["server_port"].(int)
				delete(out, "server_port")
				out["server_ports"] = []string{fmt.Sprintf("%d:%d", port, port)}
				out["hop_interval"], out["hop_interval_max"] = "5s", "6s"
				out["bbr_profile"], out["disable_chrome_parrot"] = "conservative", true
			}
			selfSigned := strings.HasPrefix(variant, "self_signed")
			if selfSigned {
				// Subscription shape: an untrusted self-signed server, a camouflage
				// SNI absent from its certificate, and insecure=false.
				tls["insecure"], tls["server_name"] = false, "www.apple.com"
				tls["disable_sni"] = variant == "self_signed_no_sni"
			}
			startProtocolPeerCore(t, in, true)
			if selfSigned {
				expected := "certificate"
				if variant == "self_signed_no_sni" {
					expected = "" // ChromeParrot rejects the unpinned VerifyConnection check.
				}
				verifyProtocolPeerHTTPFailure(t, startProtocolPeerCore(t, out, false), expected)
			}
			out["tls_certificate_sha256"] = fingerprint
			client := startProtocolPeerCore(t, out, false)
			verifyProtocolPeerHTTP(t, client, 2)
			verifyProtocolPeerUDP(t, client, 4096)
			out["tls_certificate_sha256"] = strings.Repeat("00", sha256.Size)
			verifyProtocolPeerHTTPFailure(t, startProtocolPeerCore(t, out, false), "fingerprint mismatch")
		})
	}
}

func TestCertificatePinValidationAndResumption(t *testing.T) {
	for _, fingerprint := range []string{"", "chrome", strings.Repeat("a", 63), strings.Repeat("a", 65), ":" + strings.Repeat("a", 64), strings.Repeat("zz", 32)} {
		if installCertificatePin(&stdtls.Config{}, fingerprint) == nil {
			t.Fatalf("accepted invalid fingerprint %q", fingerprint)
		}
	}
	leaf := &x509.Certificate{Raw: []byte("synthetic leaf DER")}
	other := &x509.Certificate{Raw: []byte("different leaf DER")}
	pin := sha256.Sum256(leaf.Raw)
	config := &stdtls.Config{InsecureSkipVerify: true, ClientSessionCache: stdtls.NewLRUClientSessionCache(1)}
	if err := installCertificatePin(config, hex.EncodeToString(pin[:])); err != nil {
		t.Fatal(err)
	}
	if !config.SessionTicketsDisabled || config.ClientSessionCache != nil {
		t.Fatal("pinned TLS can resume without checking the certificate")
	}
	if err := config.VerifyPeerCertificate([][]byte{leaf.Raw}, nil); err != nil {
		t.Fatal(err)
	}
	if err := config.VerifyPeerCertificate([][]byte{other.Raw, leaf.Raw}, nil); err == nil {
		t.Fatal("pin accepted a different leaf certificate")
	}
	if err := config.VerifyPeerCertificate(nil, nil); err == nil {
		t.Fatal("pin accepted a missing certificate")
	}
	expected := errors.New("existing verifier rejected certificate")
	config = &stdtls.Config{VerifyPeerCertificate: func([][]byte, [][]*x509.Certificate) error { return expected }}
	if err := installCertificatePin(config, hex.EncodeToString(pin[:])); err != nil {
		t.Fatal(err)
	}
	if err := config.VerifyPeerCertificate([][]byte{leaf.Raw}, nil); !errors.Is(err, expected) {
		t.Fatal("pin bypassed the existing certificate verifier")
	}
}
