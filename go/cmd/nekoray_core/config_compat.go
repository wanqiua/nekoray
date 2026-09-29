package main

import (
	"encoding/json"
	"fmt"
	"strings"
)

func normalizeXrayConfig(input string) (string, error) {
	var config map[string]json.RawMessage
	if err := json.Unmarshal([]byte(input), &config); err != nil {
		return "", err
	}
	var outbounds []map[string]json.RawMessage
	if raw, ok := config["outbounds"]; ok {
		if err := json.Unmarshal(raw, &outbounds); err != nil {
			return "", err
		}
	}
	changed := false
	for _, out := range outbounds {
		if legacy, ok := out["domainStrategy"]; ok {
			var oldStrategy, newStrategy string
			if err := json.Unmarshal(legacy, &oldStrategy); err != nil {
				return "", fmt.Errorf("outbound domainStrategy: %w", err)
			}
			if modern, ok := out["targetStrategy"]; ok {
				if err := json.Unmarshal(modern, &newStrategy); err != nil {
					return "", fmt.Errorf("outbound targetStrategy: %w", err)
				}
				if !strings.EqualFold(oldStrategy, newStrategy) {
					return "", fmt.Errorf("outbound domainStrategy conflicts with targetStrategy")
				}
			} else {
				out["targetStrategy"] = legacy
			}
			delete(out, "domainStrategy")
			changed = true
		}
	}
	// Only the outbound-level strategy was renamed. Routing, freedom and
	// sockopt domainStrategy, legacy plugins and both pin formats stay native.
	if !changed {
		return input, nil
	}
	encoded, err := json.Marshal(outbounds)
	if err != nil {
		return "", err
	}
	config["outbounds"] = encoded
	encoded, err = json.Marshal(config)
	return string(encoded), err
}
