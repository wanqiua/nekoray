package grpc_server

import (
	"context"
	"grpc_server/gen"
)

// The archived upstream updater would replace this fork with 4.x and remove
// Xray support. A fork must publish its own compatible update feed first.
func (s *BaseServer) Update(ctx context.Context, in *gen.UpdateReq) (*gen.UpdateResp, error) {
	return &gen.UpdateResp{Error: "Automatic upstream updates are disabled for this dual-core build. Update using this fork's release package."}, nil
}
