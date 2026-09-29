package main

import (
	"fmt"
	"grpc_server"
	"os"

	"github.com/matsuridayo/libneko/neko_common"
	"github.com/xtls/xray-core/core"
)

func main() {
	fmt.Println("Xray:", core.Version(), "(wanqiua/Xray-core)", "NekoRay:", neko_common.Version_neko)
	fmt.Println()

	// nekoray_core
	if len(os.Args) > 1 && os.Args[1] == "nekoray" {
		neko_common.RunMode = neko_common.RunMode_NekoRay_Core
		grpc_server.RunCore(setupCore, &server{})
		return
	}

	// tool
	if len(os.Args) > 2 && os.Args[1] == "tool" {
		ToolBox()
		return
	}
}
