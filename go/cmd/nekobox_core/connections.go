package main

import (
	"context"
	"encoding/json"
	"net"
	"sort"
	"sync"
	"time"

	"github.com/sagernet/sing-box/adapter"
	tun "github.com/sagernet/sing-tun"
	"github.com/sagernet/sing/common/bufio"
	N "github.com/sagernet/sing/common/network"
)

const connectionHistorySize = 512

type connectionInfo struct {
	ID         uint64
	Start, End int64
	Tag        string
	Dest       string
	RDest      string
}

type connectionRegistry struct {
	outbound     adapter.OutboundManager
	mu           sync.Mutex
	next         uint64
	closed       bool
	active       map[uint64]connectionInfo
	history      [connectionHistorySize]connectionInfo
	historyNext  int
	historyCount int
}

var _ adapter.ConnectionTracker = (*connectionRegistry)(nil)

func newConnectionRegistry(outbound adapter.OutboundManager) *connectionRegistry {
	return &connectionRegistry{outbound: outbound, active: make(map[uint64]connectionInfo)}
}

func connectionDetails(metadata adapter.InboundContext, tag string) connectionInfo {
	destination := metadata.Destination
	original := destination
	if metadata.RouteOriginalDestination.IsValid() {
		original = metadata.RouteOriginalDestination
	}
	if metadata.FakeIP && metadata.OriginDestination.IsValid() {
		original = metadata.OriginDestination
	}
	item := connectionInfo{Tag: tag, Dest: metadata.Network + ":" + original.String()}
	if original != destination {
		item.RDest = metadata.Network + ":" + destination.String()
	}
	return item
}

func (registry *connectionRegistry) outboundTag(outbound adapter.Outbound) string {
	if outbound == nil {
		outbound = registry.outbound.Default()
	}
	seen := make(map[string]bool)
	for outbound != nil {
		tag := outbound.Tag()
		group, isGroup := outbound.(adapter.OutboundGroup)
		if !isGroup || seen[tag] {
			return tag
		}
		seen[tag] = true
		selected, exists := registry.outbound.Outbound(group.Now())
		if !exists {
			return tag
		}
		outbound = selected
	}
	return ""
}

func (registry *connectionRegistry) begin(metadata adapter.InboundContext, tag string) uint64 {
	item := connectionDetails(metadata, tag)
	registry.mu.Lock()
	defer registry.mu.Unlock()
	if registry.closed {
		return 0
	}
	registry.next++
	item.ID = registry.next
	item.Start = time.Now().Unix()
	registry.active[item.ID] = item
	return item.ID
}

func (registry *connectionRegistry) end(id uint64) {
	registry.mu.Lock()
	defer registry.mu.Unlock()
	item, exists := registry.active[id]
	if !exists {
		return
	}
	delete(registry.active, id)
	item.End = time.Now().Unix()
	registry.history[registry.historyNext] = item
	registry.historyNext = (registry.historyNext + 1) % connectionHistorySize
	if registry.historyCount < connectionHistorySize {
		registry.historyCount++
	}
}

func (registry *connectionRegistry) snapshot() string {
	registry.mu.Lock()
	items := make([]connectionInfo, 0, len(registry.active)+registry.historyCount)
	for _, item := range registry.active {
		items = append(items, item)
	}
	items = append(items, registry.history[:registry.historyCount]...)
	registry.mu.Unlock()
	sort.Slice(items, func(first, second int) bool { return items[first].ID > items[second].ID })
	content, _ := json.Marshal(items)
	return string(content)
}

func (registry *connectionRegistry) close() {
	registry.mu.Lock()
	defer registry.mu.Unlock()
	registry.closed = true
	clear(registry.active)
	registry.history = [connectionHistorySize]connectionInfo{}
	registry.historyCount = 0
}

func (registry *connectionRegistry) RoutedConnection(ctx context.Context, conn net.Conn, metadata adapter.InboundContext, rule adapter.Rule, outbound adapter.Outbound) net.Conn {
	return &trackedConnection{ExtendedConn: bufio.NewExtendedConn(conn), registry: registry, id: registry.begin(metadata, registry.outboundTag(outbound))}
}

func (registry *connectionRegistry) RoutedPacketConnection(ctx context.Context, conn N.PacketConn, metadata adapter.InboundContext, rule adapter.Rule, outbound adapter.Outbound) N.PacketConn {
	return &trackedPacketConnection{PacketConn: conn, registry: registry, id: registry.begin(metadata, registry.outboundTag(outbound))}
}

func (registry *connectionRegistry) RoutedFlow(ctx context.Context, metadata adapter.InboundContext, rule adapter.Rule, outbound adapter.Outbound) tun.FlowTracker {
	return &trackedFlow{registry: registry, metadata: metadata, tag: registry.outboundTag(outbound)}
}

type trackedConnection struct {
	N.ExtendedConn
	registry *connectionRegistry
	id       uint64
}

func (connection *trackedConnection) Close() error {
	defer connection.registry.end(connection.id)
	return connection.ExtendedConn.Close()
}

func (connection *trackedConnection) Upstream() any           { return connection.ExtendedConn }
func (connection *trackedConnection) ReaderReplaceable() bool { return true }
func (connection *trackedConnection) WriterReplaceable() bool { return true }

type trackedPacketConnection struct {
	N.PacketConn
	registry *connectionRegistry
	id       uint64
}

func (connection *trackedPacketConnection) Close() error {
	defer connection.registry.end(connection.id)
	return connection.PacketConn.Close()
}

func (connection *trackedPacketConnection) Upstream() any           { return connection.PacketConn }
func (connection *trackedPacketConnection) ReaderReplaceable() bool { return true }
func (connection *trackedPacketConnection) WriterReplaceable() bool { return true }

type trackedFlow struct {
	registry *connectionRegistry
	metadata adapter.InboundContext
	tag      string
	mu       sync.Mutex
	id       uint64
	closed   bool
}

func (flow *trackedFlow) AttachFlow(handle tun.FlowHandle) {
	flow.mu.Lock()
	defer flow.mu.Unlock()
	if !flow.closed && flow.id == 0 {
		flow.id = flow.registry.begin(flow.metadata, flow.tag)
	}
}

func (flow *trackedFlow) CountForward(amount int) {}
func (flow *trackedFlow) CountReverse(amount int) {}
func (flow *trackedFlow) FlowEstablished()        {}

func (flow *trackedFlow) CloseFlow(reason tun.FlowCloseReason) {
	flow.mu.Lock()
	defer flow.mu.Unlock()
	flow.closed = true
	flow.registry.end(flow.id)
}
