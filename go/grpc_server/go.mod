module grpc_server

// Keep the shared RPC implementation usable by the Go 1.21 Xray fork.
// The sing-box module independently selects its newer dependency versions.
go 1.21

require (
	github.com/matsuridayo/libneko v0.0.0-20230913024055-5277a5bfc889
	golang.org/x/net v0.28.0
	google.golang.org/grpc v1.67.3
	google.golang.org/protobuf v1.35.2
)

require (
	golang.org/x/sys v0.28.0 // indirect
	golang.org/x/text v0.17.0 // indirect
	google.golang.org/genproto/googleapis/rpc v0.0.0-20240814211410-ddb44dafa142 // indirect
)
