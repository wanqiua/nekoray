package testutil

import (
	"context"
	"fmt"
	"grpc_server/gen"
	"io"
	"net"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"testing"
	"time"

	"google.golang.org/grpc"
	"google.golang.org/grpc/credentials/insecure"
	"google.golang.org/grpc/metadata"
)

func TestOwnedDaemonParentHelper(t *testing.T) {
	if os.Getenv("NEKORAY_TEST_OWNER_HELPER") != "1" {
		return
	}
	// A synthetic GUI owner whose lifetime is controlled by an anonymous pipe.
	io.Copy(io.Discard, os.Stdin)
}

func TestCoreOwnerExit(t *testing.T) {
	directory := os.Getenv("NEKORAY_CORE_BIN_DIR")
	if directory == "" {
		t.Skip("set NEKORAY_CORE_BIN_DIR to test built core executables")
	}
	directory, err := filepath.Abs(directory)
	if err != nil {
		t.Fatal(err)
	}
	extension := ""
	if runtime.GOOS == "windows" {
		extension = ".exe"
	}
	for _, mode := range []string{"nekoray", "nekobox"} {
		t.Run(mode, func(t *testing.T) {
			owner := exec.Command(os.Args[0], "-test.run=^TestOwnedDaemonParentHelper$")
			owner.Env = append(os.Environ(), "NEKORAY_TEST_OWNER_HELPER=1")
			ownerInput, err := owner.StdinPipe()
			if err != nil {
				t.Fatal(err)
			}
			if err = owner.Start(); err != nil {
				t.Fatal(err)
			}
			defer ownerInput.Close()
			defer owner.Process.Kill()
			port := FreePort(t)
			command := exec.Command(filepath.Join(directory, mode+"_core"+extension), mode,
				"-port", fmt.Sprint(port), "-token", "local-owner-test-token", "-parent-pid", fmt.Sprint(owner.Process.Pid))
			command.Dir = directory
			if err = command.Start(); err != nil {
				t.Fatal(err)
			}
			defer command.Process.Kill()
			done := make(chan error, 1)
			go func() { done <- command.Wait() }()
			connection, err := grpc.NewClient(fmt.Sprintf("127.0.0.1:%d", port), grpc.WithTransportCredentials(insecure.NewCredentials()))
			if err != nil {
				t.Fatal(err)
			}
			defer connection.Close()
			client := gen.NewLibcoreServiceClient(connection)
			ctx, cancel := context.WithTimeout(context.Background(), 8*time.Second)
			defer cancel()
			ctx = metadata.NewOutgoingContext(ctx, metadata.Pairs("nekoray_auth", "local-owner-test-token"))
			capabilities, err := client.Capabilities(ctx, &gen.EmptyReq{}, grpc.WaitForReady(true))
			if err != nil {
				t.Fatal(err)
			}
			watchdog := false
			for _, feature := range capabilities.Features {
				watchdog = watchdog || feature == "owner-process-watchdog"
			}
			if !watchdog {
				t.Fatal("daemon did not advertise owner monitoring")
			}
			listenerPort := FreePort(t)
			config := fmt.Sprintf(`{"inbounds":[{"listen":"127.0.0.1","port":%d,"protocol":"socks","settings":{"udp":true}}],"outbounds":[{"tag":"proxy","protocol":"freedom","settings":{}}]}`, listenerPort)
			if mode == "nekobox" {
				config = fmt.Sprintf(`{"inbounds":[{"type":"mixed","tag":"mixed-in","listen":"127.0.0.1","listen_port":%d}],"outbounds":[{"type":"direct","tag":"proxy"}]}`, listenerPort)
			}
			started, err := client.Start(ctx, &gen.LoadConfigReq{CoreConfig: config})
			if err != nil || started.GetError() != "" {
				t.Fatalf("start loopback listener: %v, %s", err, started.GetError())
			}
			connectionTest, err := net.DialTimeout("tcp", fmt.Sprintf("127.0.0.1:%d", listenerPort), time.Second)
			if err != nil {
				t.Fatal(err)
			}
			connectionTest.Close()
			ownerInput.Close()
			if err = owner.Wait(); err != nil {
				t.Fatal(err)
			}
			select {
			case err = <-done:
				if err != nil {
					t.Fatalf("owned core exit: %v", err)
				}
			case <-time.After(6 * time.Second):
				t.Fatal("core survived the explicitly selected owner process")
			}
			if connectionTest, err = net.DialTimeout("tcp", fmt.Sprintf("127.0.0.1:%d", listenerPort), time.Second); err == nil {
				connectionTest.Close()
				t.Fatal("listener survived owner cleanup")
			}
		})
	}
}
