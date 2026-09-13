package grpc_server

import (
	"context"
	"grpc_server/gen"
	"net/http"
	"net/http/httptest"
	"testing"
	"time"

	"github.com/matsuridayo/libneko/neko_common"
)

func TestFullTestSpeedCancellationAndConfigurationFailures(t *testing.T) {
	previous := neko_common.CreateProxyHttpClient
	defer func() { neko_common.CreateProxyHttpClient = previous }()
	requestArrived := make(chan struct{}, 1)
	target := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		requestArrived <- struct{}{}
		<-r.Context().Done()
	}))
	defer target.Close()
	neko_common.CreateProxyHttpClient = func(interface{}) *http.Client { return target.Client() }
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	result := make(chan *gen.TestResp, 1)
	go func() {
		response, _ := DoFullTest(ctx, &gen.TestReq{FullSpeed: true, FullSpeedUrl: target.URL, FullSpeedTimeout: 30}, nil)
		result <- response
	}()
	select {
	case <-requestArrived:
	case <-time.After(3 * time.Second):
		t.Fatal("loopback speed request did not arrive")
	}
	cancel()
	select {
	case response := <-result:
		if response.FailureCategory != gen.FailureCategory_FailureCancelled {
			t.Fatalf("cancelled full test: %v", response)
		}
	case <-time.After(time.Second):
		t.Fatal("full test ignored cancellation")
	}
	response, _ := DoFullTest(context.Background(), &gen.TestReq{FullSpeed: true, FullSpeedUrl: "::bad-url"}, nil)
	if response.FailureCategory != gen.FailureCategory_FailureConfig {
		t.Fatalf("invalid speed URL must not be a dead node: %v", response)
	}
}
