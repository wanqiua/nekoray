package testutil

import (
	"context"
	"fmt"
	"grpc_server/gen"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
	"time"

	"google.golang.org/grpc"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/credentials/insecure"
	"google.golang.org/grpc/metadata"
	"google.golang.org/grpc/status"
)

// Optional release test: exercise the built executables with the same process
// arguments, stdin token and TCP gRPC transport used by the original GUI.
func TestCoreProcesses(t *testing.T) {
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
			port := FreePort(t)
			logPath := filepath.Join(t.TempDir(), "core.log")
			logFile, err := os.Create(logPath)
			if err != nil {
				t.Fatal(err)
			}
			defer logFile.Close()
			command := exec.Command(filepath.Join(directory, mode+"_core"+extension), mode, "-port", fmt.Sprint(port))
			command.Dir = directory
			command.Stdin = strings.NewReader("local-test-token\n")
			command.Stdout, command.Stderr = logFile, logFile
			if err := command.Start(); err != nil {
				t.Fatal(err)
			}
			done := make(chan struct{})
			var processError error
			go func() {
				processError = command.Wait()
				close(done)
			}()
			t.Cleanup(func() {
				select {
				case <-done:
				default:
					command.Process.Kill()
					select {
					case <-done:
					case <-time.After(5 * time.Second):
						t.Error("core process did not terminate")
					}
				}
				if t.Failed() {
					contents, _ := os.ReadFile(logPath)
					t.Log(string(contents))
				}
			})
			connection, err := grpc.NewClient(fmt.Sprintf("127.0.0.1:%d", port), grpc.WithTransportCredentials(insecure.NewCredentials()))
			if err != nil {
				t.Fatal(err)
			}
			defer connection.Close()
			client := gen.NewLibcoreServiceClient(connection)
			readyContext, readyCancel := context.WithTimeout(context.Background(), 5*time.Second)
			defer readyCancel()
			_, err = client.QueryStats(readyContext, &gen.QueryStatsReq{}, grpc.WaitForReady(true))
			if status.Code(err) != codes.Unauthenticated {
				t.Fatalf("core startup: %v", err)
			}
			makeConfig := func(port int) string {
				if mode == "nekoray" {
					return fmt.Sprintf(`{"log":{"loglevel":"none"},"inbounds":[{"listen":"127.0.0.1","port":%d,"protocol":"socks","settings":{"udp":true}}],"outbounds":[{"tag":"proxy","protocol":"freedom","settings":{}}],"stats":{},"policy":{"system":{"statsOutboundUplink":true,"statsOutboundDownlink":true}}}`, port)
				}
				return fmt.Sprintf(`{"log":{"level":"error"},"inbounds":[{"type":"mixed","tag":"mixed-in","listen":"127.0.0.1","listen_port":%d,"sniff":true}],"outbounds":[{"type":"direct","tag":"proxy"}]}`, port)
			}
			runClientLifecycle(t, client, makeConfig, mode == "nekoray")
			runParallelURLTests(t, client, makeConfig)
			exitContext, exitCancel := context.WithTimeout(context.Background(), 3*time.Second)
			defer exitCancel()
			exitContext = metadata.NewOutgoingContext(exitContext, metadata.Pairs("nekoray_auth", "local-test-token"))
			// Exit terminates the server before it can send a response.
			client.Exit(exitContext, &gen.EmptyReq{})
			select {
			case <-done:
				if processError != nil {
					t.Fatalf("core exit: %v", processError)
				}
			case <-exitContext.Done():
				t.Fatal("Exit RPC did not terminate the core")
			}
		})
	}
}
