package main

import (
	"context"
	"grpc_server/probe"
	"net"
	"net/http"
	"sync"

	"github.com/matsuridayo/libneko/neko_common"
	"github.com/matsuridayo/libneko/neko_log"
)

var instance *BoxInstance
var instanceMu sync.RWMutex

func currentInstance() *BoxInstance {
	instanceMu.RLock()
	defer instanceMu.RUnlock()
	return instance
}

func setupCore() {
	neko_log.SetupLog(50*1024, "./neko.log")
	neko_common.GetCurrentInstance = func() interface{} { return currentInstance() }
	neko_common.DialContext = func(ctx context.Context, specified interface{}, network, address string) (net.Conn, error) {
		i, _ := specified.(*BoxInstance)
		if i == nil {
			i = currentInstance()
		}
		if i != nil {
			return i.DialContext(ctx, network, address)
		}
		return neko_common.DialContextSystem(ctx, network, address)
	}
	neko_common.DialUDP = func(ctx context.Context, specified interface{}) (net.PacketConn, error) {
		i, _ := specified.(*BoxInstance)
		if i == nil {
			i = currentInstance()
		}
		if i != nil {
			return i.DialUDP(ctx)
		}
		return neko_common.DialUDPSystem(ctx)
	}
	neko_common.CreateProxyHttpClient = func(specified interface{}) *http.Client {
		i, _ := specified.(*BoxInstance)
		if i == nil {
			i = currentInstance()
		}
		return createProxyHttpClient(i)
	}
}

func createProxyHttpClient(i *BoxInstance) *http.Client {
	if i == nil {
		return probe.NewHTTPClient(nil)
	}
	client := probe.NewHTTPClient(i.DialContext)
	client.Transport = &startupRetryTransport{base: client.Transport, instance: i}
	return client
}
