package main

import (
	"encoding/json"
	"fmt"
)

func normalizeXrayConfig(input string) (string, error) {
	var config struct {
		Outbounds []struct {
			TargetStrategy string `json:"targetStrategy"`
			StreamSettings struct {
				TLSSettings struct {
					PinnedPeerCertSha256 string `json:"pinnedPeerCertSha256"`
					VerifyPeerCertByName string `json:"verifyPeerCertByName"`
				} `json:"tlsSettings"`
			} `json:"streamSettings"`
		} `json:"outbounds"`
	}
	if err := json.Unmarshal([]byte(input), &config); err != nil {
		return "", err
	}
	for _, out := range config.Outbounds {
		if out.TargetStrategy != "" && out.TargetStrategy != "AsIs" {
			return "", fmt.Errorf("Xray 1.8.4 compatibility core does not support outbound targetStrategy; use its native routing/DNS settings")
		}
		tls := out.StreamSettings.TLSSettings
		if tls.PinnedPeerCertSha256 != "" || tls.VerifyPeerCertByName != "" {
			return "", fmt.Errorf("Xray 1.8.4 compatibility core does not support pinnedPeerCertSha256/verifyPeerCertByName; use a trusted certificate or explicitly enable allowInsecure")
		}
	}
	// 3.26's native fields (including allowInsecure and legacy SS methods/plugins)
	// already match this fork. Do not rewrite settings or silently drop pins.
	return input, nil
}
