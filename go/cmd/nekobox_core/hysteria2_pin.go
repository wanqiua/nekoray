//go:build with_quic

package main

import (
	"context"
	"crypto/sha256"
	"crypto/subtle"
	stdtls "crypto/tls"
	"crypto/x509"
	"encoding/hex"
	"net"
	"os"
	"strings"
	"time"

	"github.com/sagernet/sing-box/adapter"
	"github.com/sagernet/sing-box/adapter/outbound"
	"github.com/sagernet/sing-box/common/dialer"
	"github.com/sagernet/sing-box/common/tls"
	C "github.com/sagernet/sing-box/constant"
	"github.com/sagernet/sing-box/log"
	"github.com/sagernet/sing-box/option"
	native "github.com/sagernet/sing-box/protocol/hysteria2"
	qtls "github.com/sagernet/sing-quic"
	"github.com/sagernet/sing-quic/hysteria"
	"github.com/sagernet/sing-quic/hysteria2"
	"github.com/sagernet/sing/common"
	"github.com/sagernet/sing/common/bufio"
	E "github.com/sagernet/sing/common/exceptions"
	"github.com/sagernet/sing/common/logger"
	M "github.com/sagernet/sing/common/metadata"
	N "github.com/sagernet/sing/common/network"
)

type pinnedHysteria2Options struct {
	option.Hysteria2OutboundOptions
	CertificateSHA256 string `json:"tls_certificate_sha256,omitempty"`
}

func registerPinnedHysteria2(registry *outbound.Registry) {
	outbound.Register[pinnedHysteria2Options](registry, C.TypeHysteria2, newPinnedHysteria2)
}

// Clash fingerprint and Hysteria pinSHA256 hash the leaf certificate DER.
// sing-box's certificate_public_key_sha256 hashes SubjectPublicKeyInfo instead
// and is not an equivalent setting. ChromeParrot supports VerifyPeerCertificate
// but not VerifyConnection. Disable resumption on pinned clients so every new
// connection presents a certificate and executes the composed verifier.
func installCertificatePin(config *stdtls.Config, fingerprint string) error {
	text := strings.TrimSpace(fingerprint)
	if strings.Contains(text, ":") {
		parts := strings.Split(text, ":")
		if len(parts) != sha256.Size {
			return E.New("invalid certificate SHA-256 fingerprint")
		}
		for _, part := range parts {
			if len(part) != 2 {
				return E.New("invalid certificate SHA-256 fingerprint")
			}
		}
		text = strings.Join(parts, "")
	}
	pin, err := hex.DecodeString(text)
	if err != nil || len(pin) != sha256.Size {
		return E.New("invalid certificate SHA-256 fingerprint")
	}
	config.ClientSessionCache = nil
	config.SessionTicketsDisabled = true
	previous := config.VerifyPeerCertificate
	config.VerifyPeerCertificate = func(rawCerts [][]byte, verifiedChains [][]*x509.Certificate) error {
		if previous != nil {
			if err := previous(rawCerts, verifiedChains); err != nil {
				return err
			}
		}
		if len(rawCerts) == 0 {
			return E.New("certificate SHA-256 pin: server did not provide a certificate")
		}
		actual := sha256.Sum256(rawCerts[0])
		if subtle.ConstantTimeCompare(actual[:], pin) != 1 {
			return E.New("certificate SHA-256 fingerprint mismatch")
		}
		return nil
	}
	return nil
}

type pinnedHysteria2 struct {
	outbound.Adapter
	logger logger.ContextLogger
	client *hysteria2.Client
}

// The pinned path follows sing-box v1.14.0 protocol/hysteria2/outbound.go.
// Unpinned configurations use the upstream constructor, including Realm.
func newPinnedHysteria2(ctx context.Context, router adapter.Router, logger log.ContextLogger, tag string, extended pinnedHysteria2Options) (adapter.Outbound, error) {
	options := extended.Hysteria2OutboundOptions
	if extended.CertificateSHA256 == "" {
		return native.NewOutbound(ctx, router, logger, tag, options)
	}
	if options.Realm != nil {
		return nil, E.New("certificate SHA-256 pin is not supported with Hysteria2 Realm")
	}
	options.UDPFragmentDefault = true
	if options.TLS == nil || !options.TLS.Enabled {
		return nil, C.ErrTLSRequired
	}
	// The pin replaces CA-chain and hostname validation, as Clash Meta's
	// fingerprint and sing-box's certificate_public_key_sha256 do. Subscriptions
	// pin self-signed servers with insecure=false. Setting it here, not on the
	// built config, also drops the disable_sni VerifyConnection check.
	tlsOptions := *options.TLS
	tlsOptions.Insecure = true
	tlsConfig, err := tls.NewClient(ctx, logger, options.Server, tlsOptions)
	if err != nil {
		return nil, err
	}
	config, err := tlsConfig.STDConfig()
	if err != nil {
		return nil, E.Cause(err, "certificate SHA-256 pin requires Go TLS")
	}
	if err = installCertificatePin(config, extended.CertificateSHA256); err != nil {
		return nil, err
	}
	var salamanderPassword, geckoPassword string
	var geckoMinPacketSize, geckoMaxPacketSize int
	if options.Obfs != nil {
		if options.Obfs.Password == "" {
			return nil, E.New("missing obfs password")
		}
		switch options.Obfs.Type {
		case hysteria2.ObfsTypeSalamander:
			salamanderPassword = options.Obfs.Password
		case hysteria2.ObfsTypeGecko:
			geckoPassword = options.Obfs.Password
			geckoMinPacketSize = options.Obfs.GeckoOptions.MinPacketSize
			geckoMaxPacketSize = options.Obfs.GeckoOptions.MaxPacketSize
		default:
			return nil, E.New("unknown obfs type: ", options.Obfs.Type)
		}
	}
	outboundDialer, err := dialer.NewWithOptions(dialer.Options{
		Context: ctx, Options: options.DialerOptions, RemoteIsDomain: options.ServerIsDomain(),
	})
	if err != nil {
		return nil, err
	}
	networkList := options.Network.Build()
	client, err := hysteria2.NewClient(hysteria2.ClientOptions{
		Context: ctx, Dialer: outboundDialer, Logger: logger, BrutalDebug: options.BrutalDebug,
		ServerAddress: options.ServerOptions.Build(), ServerPorts: options.ServerPorts,
		HopInterval: time.Duration(options.HopInterval), HopIntervalMax: time.Duration(options.HopIntervalMax),
		SendBPS: uint64(options.UpMbps * hysteria.MbpsToBps), ReceiveBPS: uint64(options.DownMbps * hysteria.MbpsToBps),
		SalamanderPassword: salamanderPassword, GeckoPassword: geckoPassword,
		GeckoMinPacketSize: geckoMinPacketSize, GeckoMaxPacketSize: geckoMaxPacketSize,
		Password: options.Password, TLSConfig: tlsConfig,
		QUICOptions: qtls.QUICOptions{
			IdleTimeout: options.IdleTimeout.Build(), KeepAlivePeriod: options.KeepAlivePeriod.Build(),
			StreamReceiveWindow: options.StreamReceiveWindow.Value(), ConnectionReceiveWindow: options.ConnectionReceiveWindow.Value(),
			MaxConcurrentStreams: options.MaxConcurrentStreams, InitialPacketSize: options.InitialPacketSize,
			DisablePathMTUDiscovery: options.DisablePathMTUDiscovery,
		},
		UDPDisabled: !common.Contains(networkList, N.NetworkUDP),
		BBRProfile:  options.BBRProfile, ChromeParrot: !options.DisableChromeParrot,
	})
	if err != nil {
		return nil, err
	}
	return &pinnedHysteria2{
		Adapter: outbound.NewAdapterWithDialerOptions(C.TypeHysteria2, tag, networkList, options.DialerOptions),
		logger:  logger, client: client,
	}, nil
}

func (h *pinnedHysteria2) DialContext(ctx context.Context, network string, destination M.Socksaddr) (net.Conn, error) {
	switch N.NetworkName(network) {
	case N.NetworkTCP:
		h.logger.InfoContext(ctx, "outbound connection to ", destination)
		return h.client.DialConn(ctx, destination)
	case N.NetworkUDP:
		conn, err := h.ListenPacket(ctx, destination)
		if err != nil {
			return nil, err
		}
		return bufio.NewBindPacketConn(conn, destination), nil
	default:
		return nil, E.New("unsupported network: ", network)
	}
}

func (h *pinnedHysteria2) ListenPacket(ctx context.Context, destination M.Socksaddr) (net.PacketConn, error) {
	h.logger.InfoContext(ctx, "outbound packet connection to ", destination)
	return h.client.ListenPacket(ctx)
}

func (h *pinnedHysteria2) InterfaceUpdated(ctx context.Context) {
	h.client.CloseWithError(E.New("network changed"))
}

func (h *pinnedHysteria2) Close() error {
	return h.client.CloseWithError(os.ErrClosed)
}
