package main

import (
	"context"
	"fmt"
	"grpc_server/probe"
	"log"
	"net"
	"net/http"
	"os"
	"sync"

	"github.com/matsuridayo/libneko/neko_common"
	"github.com/matsuridayo/libneko/neko_log"

	appLog "github.com/xtls/xray-core/app/log"
	commonLog "github.com/xtls/xray-core/common/log"
	v2rayNet "github.com/xtls/xray-core/common/net"
	core "github.com/xtls/xray-core/core"
	"github.com/xtls/xray-core/features/dns"
)

var instance *NekoV2RayInstance
var instanceMu sync.RWMutex

func currentInstance() *NekoV2RayInstance {
	instanceMu.RLock()
	defer instanceMu.RUnlock()
	return instance
}

var getNekorayTunIndex = func() int { return 0 } // Windows only
var underlyingNetDialer *net.Dialer

type v2rayLogWriter struct {
}

func (w *v2rayLogWriter) Write(s string) error {
	log.Print(s)
	return nil
}

func (w *v2rayLogWriter) Close() error {
	return nil
}

func setupCore() {
	// core setup
	os.Setenv("v2ray.conf.geoloader", "memconservative")
	neko_log.SetupLog(50*1024, "./neko.log")
	_ = appLog.RegisterHandlerCreator(appLog.LogType_Console, func(lt appLog.LogType, options appLog.HandlerCreatorOptions) (commonLog.Handler, error) {
		return commonLog.NewLogger(func() commonLog.Writer {
			return &v2rayLogWriter{}
		}), nil
	})
	// localdns setup
	resolver_def := &net.Resolver{PreferGo: false}
	resolver_go := &net.Resolver{PreferGo: true}
	if underlyingNetDialer != nil && os.Getenv("NKR_CORE_RAY_DIRECT_DNS") == "1" {
		resolver_def.Dial = underlyingNetDialer.DialContext
		resolver_go.Dial = underlyingNetDialer.DialContext
		log.Println("using NKR_CORE_RAY_DIRECT_DNS")
	}
	v2rayNet.LookupIP = func(host string) (ips []net.IP, err error) {
		// fix old sekai
		defer func() {
			if len(ips) == 0 {
				log.Println("LookupIP error:", host, err)
				err = dns.ErrEmptyResponse
			}
		}()
		// Normal mode use system resolver (go bug)
		if getNekorayTunIndex() == 0 {
			return resolver_def.LookupIP(context.Background(), "ip", host)
		}
		// Windows Tun Mode use Go resolver
		return resolver_go.LookupIP(context.Background(), "ip", host)
	}
	neko_common.GetCurrentInstance = func() interface{} {
		return currentInstance()
	}
	neko_common.DialContext = func(ctx context.Context, specifiedInstance interface{}, network, addr string) (net.Conn, error) {
		dest, err := v2rayNet.ParseDestination(fmt.Sprintf("%s:%s", network, addr))
		if err != nil {
			return nil, err
		}
		if i, ok := specifiedInstance.(*NekoV2RayInstance); ok {
			return core.Dial(ctx, i.Instance, dest)
		}
		if active := currentInstance(); active != nil {
			return core.Dial(ctx, active.Instance, dest)
		}
		return neko_common.DialContextSystem(ctx, network, addr)
	}
	neko_common.DialUDP = func(ctx context.Context, specifiedInstance interface{}) (net.PacketConn, error) {
		if i, ok := specifiedInstance.(*NekoV2RayInstance); ok {
			return core.DialUDP(ctx, i.Instance)
		}
		if active := currentInstance(); active != nil {
			return core.DialUDP(ctx, active.Instance)
		}
		return neko_common.DialUDPSystem(ctx)
	}
	neko_common.CreateProxyHttpClient = func(specifiedInstance interface{}) *http.Client {
		if i, ok := specifiedInstance.(*NekoV2RayInstance); ok {
			return createProxyHttpClient(i)
		}
		return createProxyHttpClient(currentInstance())
	}
}

// PROXY

func createProxyHttpClient(i *NekoV2RayInstance) *http.Client {
	if i == nil {
		return probe.NewHTTPClient(nil)
	}
	return probe.NewHTTPClient(func(ctx context.Context, network, addr string) (net.Conn, error) {
		dest, err := v2rayNet.ParseDestination(fmt.Sprintf("%s:%s", network, addr))
		if err != nil {
			return nil, err
		}
		return core.Dial(ctx, i.Instance, dest)
	})
}
