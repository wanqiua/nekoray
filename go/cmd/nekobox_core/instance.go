package main

import (
	"context"
	"net"
	"sync"
	"sync/atomic"
	"time"

	"nekobox_core/internal/compat"

	box "github.com/sagernet/sing-box"
	"github.com/sagernet/sing-box/experimental/v2rayapi"
	"github.com/sagernet/sing-box/include"
	"github.com/sagernet/sing-box/option"
	R "github.com/sagernet/sing-box/route/rule"
	"github.com/sagernet/sing/common/json"
	M "github.com/sagernet/sing/common/metadata"
)

type BoxInstance struct {
	*box.Box
	stats      *v2rayapi.StatsService
	cancel     context.CancelFunc
	closeOnce  sync.Once
	closeError error
	closed     atomic.Bool
	startedAt  time.Time
}

func boxOptions(config []byte) (context.Context, context.CancelFunc, option.Options, error) {
	return boxOptionsContext(context.Background(), config)
}

func boxOptionsContext(parent context.Context, config []byte) (context.Context, context.CancelFunc, option.Options, error) {
	ctx, cancel := context.WithCancel(include.Context(parent))
	normalized, err := compat.Normalize(config)
	var options option.Options
	if err == nil {
		err = json.UnmarshalContext(ctx, normalized, &options)
	}
	if options.Log == nil {
		options.Log = &option.LogOptions{}
	}
	options.Log.DisableColor = true
	return ctx, cancel, options, err
}

func NewBoxInstance(config []byte, statsOutbounds []string) (*BoxInstance, error) {
	ctx, cancel, options, err := boxOptions(config)
	if err != nil {
		cancel()
		return nil, err
	}
	b, err := box.New(box.Options{Options: options, Context: ctx})
	if err != nil {
		cancel()
		return nil, err
	}
	i := &BoxInstance{Box: b, cancel: cancel}
	// Upstream retains the runtime switch, but removed its JSON field. Restore
	// it only for the sniff actions synthesized from legacy inbound options.
	rules := b.Router().Rules()
	for _, index := range compat.LegacySniffOverrides(config) {
		if index < len(rules) {
			if action, ok := rules[index].Action().(*R.RuleActionSniff); ok {
				action.OverrideDestination = true
			}
		}
	}
	if len(statsOutbounds) != 0 {
		i.stats = v2rayapi.NewStatsService(option.V2RayStatsServiceOptions{
			Enabled: true, Outbounds: statsOutbounds,
		})
		b.Router().AppendTracker(i.stats)
	}
	if err = b.Start(); err != nil {
		i.Close()
		return nil, err
	}
	i.startedAt = time.Now()
	return i, nil
}

func (i *BoxInstance) Close() error {
	i.closeOnce.Do(func() {
		i.closed.Store(true)
		i.cancel()
		i.closeError = i.Box.Close()
	})
	return i.closeError
}

func (i *BoxInstance) DialContext(ctx context.Context, network, address string) (net.Conn, error) {
	return i.Outbound().Default().DialContext(ctx, network, M.ParseSocksaddr(address))
}

func (i *BoxInstance) DialUDP(ctx context.Context) (net.PacketConn, error) {
	conn, err := i.Outbound().Default().ListenPacket(ctx, M.Socksaddr{})
	if err != nil {
		return nil, err
	}
	return conn, nil
}
