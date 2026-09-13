package probe

import (
	"context"
	"net"
	"time"
)

func TCP(ctx context.Context, address string, timeoutMS int32) (int32, error) {
	if _, _, err := net.SplitHostPort(address); err != nil {
		return 0, &ConfigError{Message: "TCP test address must contain a host and port"}
	}
	timeout := time.Duration(timeoutMS) * time.Millisecond
	if timeout <= 0 {
		timeout = DefaultTimeout
	}
	testContext, cancel := context.WithTimeout(ctx, timeout)
	defer cancel()
	started := time.Now()
	connection, err := (&net.Dialer{}).DialContext(testContext, "tcp", address)
	if err != nil {
		return 0, err
	}
	defer connection.Close()
	return int32(max(1, time.Since(started).Milliseconds())), nil
}
