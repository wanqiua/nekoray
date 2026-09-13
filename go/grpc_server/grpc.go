package grpc_server

import (
	"bufio"
	"context"
	"errors"
	"flag"
	"fmt"
	"grpc_server/auth"
	"grpc_server/gen"
	"log"
	"net"
	"os"
	"runtime"
	"strconv"
	"strings"
	"sync"
	"syscall"
	"time"

	"github.com/matsuridayo/libneko/neko_common"

	"google.golang.org/grpc"
)

type BaseServer struct {
	gen.UnimplementedLibcoreServiceServer
	exitHandler func()
}

// SetExitHandler is promoted by both concrete servers. RunCore installs a
// bounded, graceful stop before a daemon exits, including after owner loss.
func (s *BaseServer) SetExitHandler(handler func()) { s.exitHandler = handler }

func (s *BaseServer) Exit(ctx context.Context, in *gen.EmptyReq) (out *gen.EmptyResp, _ error) {
	out = &gen.EmptyResp{}
	if s.exitHandler != nil {
		go s.exitHandler()
	} else {
		os.Exit(0)
	}
	return
}

// Elevation creates a shell/service parent on several platforms. The GUI may
// therefore explicitly name its own PID rather than rely on os.Getppid(). On
// Windows os.FindProcess retains a handle, so PID reuse cannot change the owner.
func watchParent(parentPID int, exited func()) {
	parent, err := os.FindProcess(parentPID)
	if err != nil {
		exited()
		return
	}
	defer parent.Release()
	if runtime.GOOS == "windows" {
		parent.Wait()
		exited()
		return
	}
	for {
		// EPERM means the owner is alive but belongs to another user. An
		// elevated daemon must not mistake this for a terminated GUI.
		err = parent.Signal(syscall.Signal(0))
		if err != nil && !errors.Is(err, syscall.EPERM) {
			exited()
			return
		}
		time.Sleep(time.Second)
	}
}

func RunCore(setupCore func(), server gen.LibcoreServiceServer) {
	_token := flag.String("token", "", "")
	_port := flag.Int("port", 19810, "")
	_debug := flag.Bool("debug", false, "")
	_parentPID := flag.Int("parent-pid", os.Getppid(), "owning GUI process ID")
	flag.CommandLine.Parse(os.Args[2:])
	if *_parentPID <= 0 || *_parentPID == os.Getpid() {
		log.Fatal("invalid owning process")
	}

	neko_common.Debug = *_debug

	// Libcore
	setupCore()

	// GRPC
	lis, err := net.Listen("tcp", "127.0.0.1:"+strconv.Itoa(*_port))
	if err != nil {
		log.Fatalf("failed to listen: %v", err)
	}

	token := *_token
	if token == "" {
		os.Stderr.WriteString("Please set a token: ")
		s := bufio.NewScanner(os.Stdin)
		if s.Scan() {
			token = strings.TrimSpace(s.Text())
		}
	}
	if token == "" {
		fmt.Println("You must set a token")
		os.Exit(0)
	}
	os.Stderr.WriteString("token is set\n")

	auther := auth.Authenticator{
		Token: token,
	}

	s := grpc.NewServer(
		grpc.StreamInterceptor(auther.StreamServerInterceptor),
		grpc.UnaryInterceptor(auther.UnaryServerInterceptor),
	)
	var shutdownOnce sync.Once
	shutdown := func() {
		shutdownOnce.Do(func() {
			// Cancel in-flight starts before closing the active instance. Stop
			// releases TUN routes/adapters instead of relying on abrupt exit.
			s.Stop()
			ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
			defer cancel()
			stopped := make(chan struct{})
			go func() {
				defer close(stopped)
				defer func() { _ = recover() }()
				server.Stop(ctx, &gen.EmptyReq{})
			}()
			select {
			case <-stopped:
			case <-ctx.Done():
			}
			os.Exit(0)
		})
	}
	if configurable, ok := server.(interface{ SetExitHandler(func()) }); ok {
		configurable.SetExitHandler(shutdown)
	}
	go watchParent(*_parentPID, shutdown)
	gen.RegisterLibcoreServiceServer(s, server)

	name := "nekoray_core"
	if neko_common.RunMode == neko_common.RunMode_NekoBox_Core {
		name = "nekobox_core"
	}

	log.Printf("%s grpc server listening at %v\n", name, lis.Addr())
	if err := s.Serve(lis); err != nil && !errors.Is(err, grpc.ErrServerStopped) {
		log.Fatalf("failed to serve: %v", err)
	}
	// Serve may return before the goroutine performing shutdown has finished
	// closing TUN. Keep main alive until that cleanup exits the process.
	shutdown()
}
