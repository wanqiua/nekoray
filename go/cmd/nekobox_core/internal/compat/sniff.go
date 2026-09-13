package compat

import "encoding/json"

// LegacySniffOverrides returns indexes of synthesized sniff actions whose
// destination override must be restored using the upstream runtime API.
func LegacySniffOverrides(input []byte) []int {
	var root object
	if json.Unmarshal(input, &root) != nil {
		return nil
	}
	var indexes []int
	index := 0
	for _, value := range list(root["inbounds"]) {
		in := obj(value)
		if in == nil {
			continue
		}
		if strategy := str(in["domain_strategy"]); strategy != "" && strategy != "as_is" {
			index++
		}
		if in["sniff"] == true {
			if in["sniff_override_destination"] == true {
				indexes = append(indexes, index)
			}
			index++
		}
	}
	return indexes
}
