package grpc_server

import (
	"context"
	"encoding/hex"
	"fmt"
	"grpc_server/gen"
	"grpc_server/probe"
	"io"
	"math"
	"net"
	"net/http"
	"strings"
	"time"

	"github.com/matsuridayo/libneko/neko_common"
)

const (
	KiB = 1024
	MiB = 1024 * KiB
)

type fullTestResult struct {
	value string
	err   error
}

func getBetweenStr(str, start, end string) string {
	_, remainder, found := strings.Cut(str, start)
	if !found {
		return ""
	}
	value, _, _ := strings.Cut(remainder, end)
	return value
}

func DoFullTest(ctx context.Context, in *gen.TestReq, instance interface{}) (out *gen.TestResp, _ error) {
	out = &gen.TestResp{}
	recordFailure := func(err error) {
		if err != nil && (out.Error == "" || ctx.Err() != nil) {
			SetTestError(out, err, RemoteFailure(ctx, err))
		}
	}
	if err := ctx.Err(); err != nil {
		recordFailure(err)
		return
	}
	if !in.FullLatency && !in.FullUdpLatency && !in.FullSpeed && !in.FullInOut {
		recordFailure(&probe.ConfigError{Message: "no full test options selected"})
		return
	}
	httpClient := neko_common.CreateProxyHttpClient(instance)
	defer httpClient.CloseIdleConnections()

	// Latency
	var latency string
	if in.FullLatency {
		t, err := probe.URL(ctx, httpClient, in.Url, in.Timeout)
		out.Ms = t
		if err == nil {
			latency = fmt.Sprint(t, "ms")
		} else {
			SetTestError(out, err, RemoteFailure(ctx, err))
			latency = "Error: " + err.Error()
		}
	}

	// UDP Latency
	var udpLatency string
	if in.FullUdpLatency && ctx.Err() == nil {
		requestContext, cancel := context.WithTimeout(ctx, time.Second*3)
		result := make(chan fullTestResult, 1)

		go func() {
			var startTime = time.Now()
			pc, err := neko_common.DialContext(requestContext, instance, "udp", "8.8.8.8:53")
			if err == nil {
				defer pc.Close()
				stopClose := context.AfterFunc(requestContext, func() { pc.Close() })
				defer stopClose()
				if deadline, ok := requestContext.Deadline(); ok {
					pc.SetDeadline(deadline)
				}
				dnsPacket, _ := hex.DecodeString("0000010000010000000000000377777706676f6f676c6503636f6d0000010001")
				_, err = pc.Write(dnsPacket)
				if err == nil {
					var buf [1400]byte
					_, err = pc.Read(buf[:])
				}
			}
			if err == nil {
				var endTime = time.Now()
				result <- fullTestResult{value: fmt.Sprint(endTime.Sub(startTime).Abs().Milliseconds(), "ms")}
			} else {
				result <- fullTestResult{value: "Error", err: err}
			}
			close(result)
		}()

		select {
		case <-requestContext.Done():
			udpLatency = "Timeout"
			recordFailure(requestContext.Err())
		case r := <-result:
			udpLatency = r.value
			recordFailure(r.err)
		}
		cancel()
	}

	// 入口 IP
	var in_ip string
	if in.FullInOut && ctx.Err() == nil {
		requestContext, cancel := context.WithTimeout(ctx, probe.DefaultTimeout)
		addresses, err := net.DefaultResolver.LookupIPAddr(requestContext, in.InAddress)
		if err == nil && len(addresses) != 0 {
			in_ip = addresses[0].String()
		} else {
			in_ip = "Error"
			recordFailure(err)
		}
		cancel()
	}

	// 出口 IP
	var out_ip string
	if in.FullInOut && ctx.Err() == nil {
		requestContext, cancel := context.WithTimeout(ctx, probe.DefaultTimeout)
		request, _ := http.NewRequestWithContext(requestContext, http.MethodGet, "https://www.cloudflare.com/cdn-cgi/trace", nil)
		resp, err := httpClient.Do(request)
		if err == nil {
			b, readErr := io.ReadAll(io.LimitReader(resp.Body, 64*1024))
			out_ip = getBetweenStr(string(b), "ip=", "\n")
			recordFailure(readErr)
			resp.Body.Close()
		} else {
			out_ip = "Error"
			recordFailure(err)
		}
		cancel()
	}

	// 下载
	var speed string
	if in.FullSpeed && ctx.Err() == nil {
		timeout := in.FullSpeedTimeout
		if timeout <= 0 {
			timeout = 30
		}

		requestContext, cancel := context.WithTimeout(ctx, time.Second*time.Duration(timeout))
		result := make(chan fullTestResult, 1)

		go func() {
			req, err := http.NewRequestWithContext(requestContext, "GET", in.FullSpeedUrl, nil)
			if err != nil || req.URL.Host == "" || (req.URL.Scheme != "http" && req.URL.Scheme != "https") {
				result <- fullTestResult{value: "Error", err: &probe.ConfigError{Message: "speed test URL must start with http:// or https://"}}
				return
			}
			resp, err := httpClient.Do(req)
			if err == nil && resp != nil && resp.Body != nil {
				defer resp.Body.Close()
				if resp.StatusCode < 200 || resp.StatusCode >= 400 {
					result <- fullTestResult{value: "Error", err: fmt.Errorf("speed test endpoint returned HTTP %d", resp.StatusCode)}
					return
				}

				timeStart := time.Now()
				n, readErr := io.Copy(io.Discard, resp.Body)
				timeEnd := time.Now()

				duration := math.Max(timeEnd.Sub(timeStart).Seconds(), 0.000001)
				resultSpeed := (float64(n) / duration) / MiB
				result <- fullTestResult{value: fmt.Sprintf("%.2fMiB/s", resultSpeed), err: readErr}
			} else {
				result <- fullTestResult{value: "Error", err: err}
			}
			close(result)
		}()

		select {
		case <-requestContext.Done():
			speed = "Timeout"
			recordFailure(requestContext.Err())
		case s := <-result:
			speed = s.value
			recordFailure(s.err)
		}

		cancel()
	}

	fr := make([]string, 0)
	if latency != "" {
		fr = append(fr, fmt.Sprintf("Latency: %s", latency))
	}
	if udpLatency != "" {
		fr = append(fr, fmt.Sprintf("UDPLatency: %s", udpLatency))
	}
	if speed != "" {
		fr = append(fr, fmt.Sprintf("Speed: %s", speed))
	}
	if in_ip != "" {
		fr = append(fr, fmt.Sprintf("In: %s", in_ip))
	}
	if out_ip != "" {
		fr = append(fr, fmt.Sprintf("Out: %s", out_ip))
	}

	out.FullReport = strings.Join(fr, " / ")
	recordFailure(ctx.Err())

	return
}
