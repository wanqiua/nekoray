package main

import (
	"context"
	"fmt"
	"grpc_server"
	"os"
	"os/signal"
	"runtime/debug"
	"strings"
	"syscall"

	"nekobox_core/internal/compat"

	"github.com/matsuridayo/libneko/neko_common"
	box "github.com/sagernet/sing-box"
	C "github.com/sagernet/sing-box/constant"
	"github.com/spf13/cobra"
)

func singBoxVersion() string {
	if info, ok := debug.ReadBuildInfo(); ok {
		for _, dep := range info.Deps {
			if dep.Path == "github.com/sagernet/sing-box" {
				return strings.TrimPrefix(dep.Version, "v")
			}
		}
	}
	return C.Version
}

func main() {
	C.Version = singBoxVersion()
	if len(os.Args) > 1 && os.Args[1] == "nekobox" {
		fmt.Println("sing-box:", C.Version, "NekoBox:", neko_common.Version_neko)
		neko_common.RunMode = neko_common.RunMode_NekoBox_Core
		grpc_server.RunCore(setupCore, &server{})
		return
	}
	if err := command().Execute(); err != nil {
		os.Exit(1)
	}
}

func command() *cobra.Command {
	var configPath, directory, protectPath string
	var protectMark int
	var disableColor bool
	root := &cobra.Command{Use: "nekobox_core", SilenceUsage: true}
	root.PersistentFlags().StringVarP(&configPath, "config", "c", "config.json", "Configuration file")
	root.PersistentFlags().StringVarP(&directory, "directory", "D", "", "Working directory")
	root.PersistentFlags().BoolVar(&disableColor, "disable-color", false, "Disable color output")
	root.PersistentFlags().StringVar(&protectPath, "protect-listen-path", "", "Linux socket protection path")
	root.PersistentFlags().IntVar(&protectMark, "protect-fwmark", 0, "Linux socket protection mark")
	root.AddCommand(&cobra.Command{Use: "version", Run: func(*cobra.Command, []string) {
		fmt.Println("sing-box:", singBoxVersion(), "NekoBox:", neko_common.Version_neko)
	}})
	for _, name := range []string{"run", "check", "migrate"} {
		name := name
		root.AddCommand(&cobra.Command{Use: name, Args: cobra.NoArgs, RunE: func(cmd *cobra.Command, args []string) error {
			if directory != "" {
				if err := os.Chdir(directory); err != nil {
					return err
				}
			}
			config, err := os.ReadFile(configPath)
			if err != nil {
				return err
			}
			if name == "migrate" {
				if len(compat.LegacySniffOverrides(config)) != 0 {
					return fmt.Errorf("legacy sniff destination overrides are supported by run, but cannot be exported as upstream JSON")
				}
				normalized, err := compat.Normalize(config)
				if err != nil {
					return err
				}
				fmt.Fprintln(cmd.OutOrStdout(), string(normalized))
				return nil
			}
			if name == "check" {
				ctx, cancel, options, err := boxOptions(config)
				defer cancel()
				if err != nil {
					return err
				}
				i, err := box.New(box.Options{Options: options, Context: ctx})
				if err != nil {
					return err
				}
				return i.Close()
			}
			if protectPath != "" {
				if err := startProtection(protectPath, protectMark); err != nil {
					return err
				}
			}
			ctx, cancel := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
			defer cancel()
			i, err := NewBoxInstance(config, nil)
			if err != nil {
				return err
			}
			defer logClose(i)
			<-ctx.Done()
			return nil
		}})
	}
	return root
}
