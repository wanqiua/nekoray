package main

import (
	"archive/zip"
	"crypto/sha256"
	"encoding/hex"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestPortableUpdatePreservesConfigAndBacksUpOldFiles(t *testing.T) {
	root := t.TempDir()
	install := filepath.Join(root, "NekoRay_BY_WanQiu")
	if err := os.MkdirAll(filepath.Join(install, "config"), 0755); err != nil {
		t.Fatal(err)
	}
	writeFixtureFile(t, filepath.Join(install, "NekoRay_BY_WanQiu.exe"), "old executable")
	writeFixtureFile(t, filepath.Join(install, "nekoray_core.exe"), "old core")
	writeFixtureFile(t, filepath.Join(install, "config", "profiles.json"), "user settings")
	archive := filepath.Join(root, "update.zip")
	writeUpdateArchive(t, archive, map[string]string{
		"NekoRay_BY_WanQiu/NekoRay_BY_WanQiu.exe": "new executable",
		"NekoRay_BY_WanQiu/nekoray_core.exe":      "new core",
		"NekoRay_BY_WanQiu/updater.exe":           "update helper",
	})

	packageBytes, err := os.ReadFile(archive)
	if err != nil {
		t.Fatal(err)
	}
	digest := sha256.Sum256(packageBytes)
	if err := applyPortableUpdate([]string{"-archive", archive, "-sha256", "sha256:" + hex.EncodeToString(digest[:]), "-install-dir", install}); err != nil {
		t.Fatal(err)
	}
	assertFixtureFile(t, filepath.Join(install, "NekoRay_BY_WanQiu.exe"), "new executable")
	assertFixtureFile(t, filepath.Join(install, "config", "profiles.json"), "user settings")
	backups, err := filepath.Glob(install + ".backup-*")
	if err != nil || len(backups) != 1 {
		t.Fatalf("expected one backup, got %v (%v)", backups, err)
	}
	assertFixtureFile(t, filepath.Join(backups[0], "NekoRay_BY_WanQiu.exe"), "old executable")
	if _, err = os.Stat(archive); !os.IsNotExist(err) {
		t.Fatalf("downloaded archive was not removed: %v", err)
	}
}

func TestPortableUpdateRejectsWrongDigest(t *testing.T) {
	root := t.TempDir()
	archive := filepath.Join(root, "update.zip")
	writeUpdateArchive(t, archive, map[string]string{"NekoRay_BY_WanQiu/NekoRay_BY_WanQiu.exe": "new executable"})
	err := applyPortableUpdate([]string{"-archive", archive, "-sha256", "sha256:" + strings.Repeat("0", 64), "-install-dir", filepath.Join(root, "install")})
	if err == nil || !strings.Contains(err.Error(), "SHA-256") {
		t.Fatalf("expected digest mismatch rejection, got %v", err)
	}
}

func TestPortableUpdateRejectsArchivePathTraversal(t *testing.T) {
	root := t.TempDir()
	archive := filepath.Join(root, "unsafe.zip")
	writeUpdateArchive(t, archive, map[string]string{
		"NekoRay_BY_WanQiu/NekoRay_BY_WanQiu.exe": "new executable",
		"NekoRay_BY_WanQiu/../../outside.txt":     "outside",
	})
	_, err := extractPortablePackage(archive, filepath.Join(root, "stage"), "NekoRay_BY_WanQiu.exe")
	if err == nil || !strings.Contains(err.Error(), "unsafe path") {
		t.Fatalf("expected path traversal rejection, got %v", err)
	}
}

func writeUpdateArchive(t *testing.T, path string, files map[string]string) {
	t.Helper()
	archive, err := os.Create(path)
	if err != nil {
		t.Fatal(err)
	}
	writer := zip.NewWriter(archive)
	for name, contents := range files {
		entry, createErr := writer.Create(name)
		if createErr != nil {
			t.Fatal(createErr)
		}
		if _, createErr = entry.Write([]byte(contents)); createErr != nil {
			t.Fatal(createErr)
		}
	}
	if err = writer.Close(); err != nil {
		t.Fatal(err)
	}
	if err = archive.Close(); err != nil {
		t.Fatal(err)
	}
}

func writeFixtureFile(t *testing.T, path, contents string) {
	t.Helper()
	if err := os.WriteFile(path, []byte(contents), 0644); err != nil {
		t.Fatal(err)
	}
}

func assertFixtureFile(t *testing.T, path, want string) {
	t.Helper()
	contents, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	if string(contents) != want {
		t.Fatalf("%s contains %q, want %q", path, contents, want)
	}
}
