package main

import (
	"io/ioutil"
	"log"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
)

func main() {
	// update & launcher
	exe, err := os.Executable()
	if err != nil {
		panic(err.Error())
	}

	wd := filepath.Dir(exe)
	os.Chdir(wd)
	exe = filepath.Base(os.Args[0])
	log.Println("exe:", exe, "exe dir:", wd)

	if strings.HasPrefix(strings.ToLower(exe), "updater") {
		if runtime.GOOS == "windows" {
			if strings.HasPrefix(strings.ToLower(exe), "updater.old") {
				if len(os.Args) < 2 {
					MessageBoxPlain("NekoRay Updater", "Missing update arguments.")
					return
				}
				if err := applyPortableUpdate(os.Args[1:]); err != nil {
					MessageBoxPlain("NekoRay Updater", "Update failed. The backup has been kept.\n\n"+err.Error())
					return
				}
				if err := exec.Command("./NekoRay_BY_WanQiu.exe").Start(); err != nil {
					MessageBoxPlain("NekoRay Updater", "Update completed, but NekoRay could not be restarted.\n\n"+err.Error())
				}
			} else {
				if err := Copy("./updater.exe", "./updater.old"); err != nil {
					MessageBoxPlain("NekoRay Updater", "Could not prepare the update helper.\n\n"+err.Error())
					return
				}
				if err := exec.Command("./updater.old", os.Args[1:]...).Start(); err != nil {
					MessageBoxPlain("NekoRay Updater", "Could not start the update helper.\n\n"+err.Error())
				}
			}
		} else {
			// 1. update files
			Updater()
			// 2. start
			if os.Getenv("NKR_FROM_LAUNCHER") == "1" {
				Launcher()
			} else {
				exec.Command("./nekoray").Start()
			}
		}
		return
	} else if strings.HasPrefix(strings.ToLower(exe), "launcher") {
		Launcher()
		return
	}
	log.Fatalf("wrong name")
}

func Copy(src string, dst string) error {
	data, err := ioutil.ReadFile(src)
	if err == nil {
		err = ioutil.WriteFile(dst, data, 0644)
	}
	if err != nil {
		log.Println("copy updater:", err)
	}
	return err
}
