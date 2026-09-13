package main

import (
	"context"
	"fmt"
	"strings"
	"sync"
	"sync/atomic"

	appOutbound "github.com/xtls/xray-core/app/proxyman/outbound"
	"github.com/xtls/xray-core/core"
	"github.com/xtls/xray-core/features/outbound"
	"github.com/xtls/xray-core/features/stats"
)

type NekoV2RayInstance struct {
	*core.Instance
	StatsManager stats.Manager
	connections  *connectionRegistry
	cancel       context.CancelFunc
	closed       atomic.Bool
	closeOnce    sync.Once
	closeError   error
}

func NewNekoV2rayInstance(configStr string) (*NekoV2RayInstance, error) {
	configStr, err := normalizeXrayConfig(configStr)
	if err != nil {
		return nil, err
	}
	config, err := core.LoadConfig("json", strings.NewReader(configStr))
	if err != nil {
		return nil, err
	}
	// Upstream's manager can replace only tagged handlers.
	tags := make(map[string]bool)
	for _, handler := range config.Outbound {
		tags[handler.Tag] = true
	}
	for n, handler := range config.Outbound {
		if handler.Tag == "" {
			tag := fmt.Sprintf("nekoray-untagged-%d", n)
			for tags[tag] {
				tag += "-"
			}
			handler.Tag = tag
			tags[tag] = true
		}
	}
	ctx, cancel := context.WithCancel(context.Background())
	c, err := core.NewWithContext(ctx, config)
	if err != nil {
		cancel()
		return nil, err
	}
	i := &NekoV2RayInstance{Instance: c, cancel: cancel, connections: newConnectionRegistry()}
	i.StatsManager, _ = c.GetFeature(stats.ManagerType()).(stats.Manager)
	manager := c.GetFeature(outbound.ManagerType()).(outbound.Manager)
	for _, config := range config.Outbound {
		handler, ok := manager.GetHandler(config.Tag).(*appOutbound.Handler)
		if !ok {
			continue
		}
		if err = manager.RemoveHandler(ctx, config.Tag); err == nil {
			err = manager.AddHandler(ctx, &trackedOutbound{Handler: handler, registry: i.connections})
		}
		if err != nil {
			i.Close()
			return nil, err
		}
	}
	return i, nil
}

func (i *NekoV2RayInstance) Close() error {
	i.closeOnce.Do(func() {
		i.closed.Store(true)
		i.cancel()
		i.connections.close()
		i.closeError = i.Instance.Close()
	})
	return i.closeError
}
