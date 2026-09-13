package grpc_server

import (
	"crypto/sha256"
	"encoding/hex"
	"grpc_server/gen"
	"io"
	"os"
	"runtime/debug"
	"sort"
	"strings"
	"sync"

	"github.com/matsuridayo/libneko/neko_common"
)

var executableIdentity struct {
	sync.Once
	id string
}

// BuildIdentity binds results to the bytes of this process, including wrapper
// changes and custom build tags. A version string alone is not a build identity.
// An unreadable executable returns an empty identity; the GUI must then avoid
// persisting results as current after this process generation ends.
func BuildIdentity() string {
	executableIdentity.Do(func() {
		path, err := os.Executable()
		if err != nil {
			return
		}
		file, err := os.Open(path)
		if err != nil {
			return
		}
		defer file.Close()
		hash := sha256.New()
		if _, err = io.Copy(hash, file); err == nil {
			executableIdentity.id = "sha256:" + hex.EncodeToString(hash.Sum(nil))
		}
	})
	return executableIdentity.id
}

func ConfigHash(config []byte) string {
	hash := sha256.Sum256(config)
	return "sha256:" + hex.EncodeToString(hash[:])
}

func CoreCapabilities(family, version, module string, features []string) *gen.CapabilitiesResp {
	response := &gen.CapabilitiesResp{
		CoreFamily: family, CoreVersion: version, BuildId: BuildIdentity(),
		WrapperVersion: neko_common.Version_neko,
		Features:       append([]string(nil), features...),
	}
	response.Features = append(response.Features, "owner-process-watchdog")
	if info, ok := debug.ReadBuildInfo(); ok {
		for _, dep := range info.Deps {
			if dep.Path == module {
				if dep.Replace != nil {
					dep = dep.Replace
				}
				response.SourceRevision = dep.Path + "@" + dep.Version
				break
			}
		}
		for _, setting := range info.Settings {
			if setting.Key == "-tags" {
				response.BuildTags = strings.FieldsFunc(setting.Value, func(r rune) bool { return r == ',' || r == ' ' })
			}
		}
	}
	sort.Strings(response.Features)
	sort.Strings(response.BuildTags)
	return response
}

func CheckResponse(capabilities *gen.CapabilitiesResp) *gen.CheckResp {
	return &gen.CheckResp{
		CoreFamily: capabilities.CoreFamily, CoreVersion: capabilities.CoreVersion, BuildId: capabilities.BuildId,
	}
}

func AttachTestIdentity(response *gen.TestResp, capabilities *gen.CapabilitiesResp) {
	response.CoreFamily = capabilities.CoreFamily
	response.CoreVersion = capabilities.CoreVersion
	response.BuildId = capabilities.BuildId
}
