package main

import (
	"context"
	"encoding/json"
	"sync"
	"sync/atomic"
	"time"

	appOutbound "github.com/xtls/xray-core/app/proxyman/outbound"
	"github.com/xtls/xray-core/common/session"
	"github.com/xtls/xray-core/transport"
)

const connectionHistorySize = 512

// Preserve the JSON consumed by the 3.26 connection table using per-instance
// outbound tracking, independent of other instances in the process.
type connectionInfo struct {
	ID         uint64
	Start, End int64
	Tag        string
	Dest       string
	RDest      string
}

type connectionRegistry struct {
	enabled atomic.Bool
	mu      sync.Mutex
	next    uint64
	recent  [connectionHistorySize]connectionInfo
	active  map[uint64]context.CancelFunc
}

func newConnectionRegistry() *connectionRegistry {
	return &connectionRegistry{active: make(map[uint64]context.CancelFunc)}
}

func (r *connectionRegistry) begin(tag, dest string, cancel context.CancelFunc) uint64 {
	r.mu.Lock()
	defer r.mu.Unlock()
	r.next++
	id := r.next
	r.active[id] = cancel
	if r.enabled.Load() {
		r.recent[id%connectionHistorySize] = connectionInfo{ID: id, Start: time.Now().Unix(), Tag: tag, Dest: dest}
	}
	return id
}

func (r *connectionRegistry) end(id uint64, dest string) {
	r.mu.Lock()
	defer r.mu.Unlock()
	delete(r.active, id)
	item := &r.recent[id%connectionHistorySize]
	if item.ID == id {
		item.End = time.Now().Unix()
		item.RDest = dest
	}
}

func (r *connectionRegistry) snapshot() string {
	r.mu.Lock()
	defer r.mu.Unlock()
	items := make([]connectionInfo, 0, connectionHistorySize)
	if r.enabled.Load() {
		for n := uint64(0); n < connectionHistorySize && n < r.next; n++ {
			item := r.recent[(r.next-n)%connectionHistorySize]
			if item.ID != 0 {
				items = append(items, item)
			}
		}
	}
	b, _ := json.Marshal(items)
	return string(b)
}

func (r *connectionRegistry) close() {
	r.mu.Lock()
	cancels := make([]context.CancelFunc, 0, len(r.active))
	for _, cancel := range r.active {
		cancels = append(cancels, cancel)
	}
	r.mu.Unlock()
	for _, cancel := range cancels {
		cancel()
	}
}

type trackedOutbound struct {
	*appOutbound.Handler
	registry *connectionRegistry
}

func (h *trackedOutbound) Dispatch(ctx context.Context, link *transport.Link) {
	ctx, cancel := context.WithCancel(ctx)
	defer cancel()
	destination := func() string {
		outbounds := session.OutboundsFromContext(ctx)
		if len(outbounds) == 0 {
			return ""
		}
		item := outbounds[len(outbounds)-1]
		if item == nil || item.Target.Address == nil {
			return ""
		}
		return item.Target.String()
	}
	id := h.registry.begin(h.Tag(), destination(), cancel)
	defer func() { h.registry.end(id, destination()) }()
	h.Handler.Dispatch(ctx, link)
}
