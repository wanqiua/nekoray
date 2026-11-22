package main

import (
	"archive/zip"
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"flag"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"
	"time"
)

func applyPortableUpdate(args []string) error {
	options := flag.NewFlagSet("portable-update", flag.ContinueOnError)
	archivePath := options.String("archive", "", "downloaded portable package")
	expectedDigest := options.String("sha256", "", "expected SHA-256 digest")
	installPath := options.String("install-dir", "", "portable application directory")
	executable := options.String("executable", "NekoRay_BY_WanQiu.exe", "application executable")
	waitPID := options.Int("wait-pid", 0, "GUI process to wait for")
	if err := options.Parse(args); err != nil {
		return err
	}
	if *archivePath == "" || *installPath == "" || filepath.Base(*executable) != *executable {
		return errors.New("invalid update arguments")
	}
	if *waitPID > 0 {
		waitForProcess(*waitPID)
	}
	installPathAbs, err := filepath.Abs(*installPath)
	if err != nil {
		return err
	}
	archiveAbs, err := filepath.Abs(*archivePath)
	if err != nil {
		return err
	}
	if *expectedDigest != "" {
		if !strings.HasPrefix(*expectedDigest, "sha256:") || len(strings.TrimPrefix(*expectedDigest, "sha256:")) != sha256.Size*2 {
			return errors.New("invalid expected update digest")
		}
		if _, err = hex.DecodeString(strings.TrimPrefix(*expectedDigest, "sha256:")); err != nil {
			return errors.New("invalid expected update digest")
		}
		file, openErr := os.Open(archiveAbs)
		if openErr != nil {
			return openErr
		}
		hash := sha256.New()
		_, copyErr := io.Copy(hash, file)
		closeErr := file.Close()
		if copyErr != nil {
			return copyErr
		}
		if closeErr != nil {
			return closeErr
		}
		if !strings.EqualFold(hex.EncodeToString(hash.Sum(nil)), strings.TrimPrefix(*expectedDigest, "sha256:")) {
			return errors.New("downloaded package SHA-256 does not match GitHub release metadata")
		}
	}
	parent := filepath.Dir(installPathAbs)
	stage, err := os.MkdirTemp(parent, ".nekoray-update-stage-")
	if err != nil {
		return err
	}
	defer os.RemoveAll(stage)
	packageRoot, err := extractPortablePackage(archiveAbs, stage, *executable)
	if err != nil {
		return err
	}
	if _, err = os.Stat(filepath.Join(installPathAbs, *executable)); err != nil {
		return fmt.Errorf("current application is missing: %w", err)
	}
	backupPath := installPathAbs + ".backup-" + time.Now().Format("20060102-150405")
	if err = copyTree(installPathAbs, backupPath, false); err != nil {
		return fmt.Errorf("create backup: %w", err)
	}
	if err = copyTree(packageRoot, installPathAbs, true); err != nil {
		removeManagedFiles(packageRoot, installPathAbs)
		if restoreErr := copyTree(backupPath, installPathAbs, false); restoreErr != nil {
			return fmt.Errorf("install failed: %v; rollback failed: %w; backup: %s", err, restoreErr, backupPath)
		}
		return fmt.Errorf("install failed and previous files were restored: %w", err)
	}
	if err = os.Remove(archiveAbs); err != nil && !errors.Is(err, os.ErrNotExist) {
		return fmt.Errorf("update installed, but could not remove downloaded package: %w", err)
	}
	return nil
}

func extractPortablePackage(archivePath, stage, executable string) (string, error) {
	archive, err := zip.OpenReader(archivePath)
	if err != nil {
		return "", err
	}
	defer archive.Close()
	var packageRoot string
	var totalBytes uint64
	for _, file := range archive.File {
		clean := filepath.Clean(filepath.FromSlash(file.Name))
		if filepath.IsAbs(clean) || clean == "." || clean == ".." || strings.HasPrefix(clean, ".."+string(filepath.Separator)) {
			return "", fmt.Errorf("unsafe path in update archive: %s", file.Name)
		}
		if file.Mode()&os.ModeSymlink != 0 {
			return "", fmt.Errorf("links are not allowed in update archive: %s", file.Name)
		}
		parts := strings.Split(clean, string(filepath.Separator))
		if parts[0] != "NekoRay_BY_WanQiu" {
			return "", fmt.Errorf("unexpected package root: %s", parts[0])
		}
		if packageRoot == "" {
			packageRoot = filepath.Join(stage, parts[0])
		}
		if file.FileInfo().IsDir() {
			if err = os.MkdirAll(filepath.Join(stage, clean), 0755); err != nil {
				return "", err
			}
			continue
		}
		totalBytes += file.UncompressedSize64
		if totalBytes > 2<<30 {
			return "", errors.New("update archive expands beyond 2 GiB")
		}
		destination := filepath.Join(stage, clean)
		if err = os.MkdirAll(filepath.Dir(destination), 0755); err != nil {
			return "", err
		}
		input, openErr := file.Open()
		if openErr != nil {
			return "", openErr
		}
		output, createErr := os.OpenFile(destination, os.O_CREATE|os.O_TRUNC|os.O_WRONLY, file.Mode().Perm())
		if createErr == nil {
			_, createErr = io.Copy(output, input)
		}
		closeErr := outputClose(output)
		inputCloseErr := input.Close()
		if createErr != nil {
			return "", createErr
		}
		if closeErr != nil {
			return "", closeErr
		}
		if inputCloseErr != nil {
			return "", inputCloseErr
		}
	}
	if packageRoot == "" {
		return "", errors.New("update archive is empty")
	}
	if _, err = os.Stat(filepath.Join(packageRoot, executable)); err != nil {
		return "", fmt.Errorf("update package does not contain %s", executable)
	}
	return packageRoot, nil
}

func outputClose(file *os.File) error {
	if file == nil {
		return nil
	}
	return file.Close()
}

func copyTree(source, destination string, preserveConfig bool) error {
	return filepath.WalkDir(source, func(sourcePath string, entry os.DirEntry, walkErr error) error {
		if walkErr != nil {
			return walkErr
		}
		relative, err := filepath.Rel(source, sourcePath)
		if err != nil {
			return err
		}
		if relative == "." {
			return os.MkdirAll(destination, 0755)
		}
		if preserveConfig && (relative == "config" || strings.HasPrefix(relative, "config"+string(filepath.Separator))) {
			if entry.IsDir() {
				return filepath.SkipDir
			}
			return nil
		}
		target := filepath.Join(destination, relative)
		if entry.IsDir() {
			return os.MkdirAll(target, 0755)
		}
		info, err := entry.Info()
		if err != nil {
			return err
		}
		if !info.Mode().IsRegular() {
			return fmt.Errorf("unsupported file type: %s", sourcePath)
		}
		if err = os.MkdirAll(filepath.Dir(target), 0755); err != nil {
			return err
		}
		input, err := os.Open(sourcePath)
		if err != nil {
			return err
		}
		output, err := os.OpenFile(target, os.O_CREATE|os.O_TRUNC|os.O_WRONLY, info.Mode().Perm())
		if err == nil {
			_, err = io.Copy(output, input)
		}
		if closeErr := input.Close(); err == nil {
			err = closeErr
		}
		if output != nil {
			if closeErr := output.Close(); err == nil {
				err = closeErr
			}
		}
		return err
	})
}

func removeManagedFiles(source, destination string) {
	_ = filepath.WalkDir(source, func(sourcePath string, entry os.DirEntry, walkErr error) error {
		if walkErr != nil || entry.IsDir() {
			return walkErr
		}
		relative, err := filepath.Rel(source, sourcePath)
		if err == nil && relative != "config" && !strings.HasPrefix(relative, "config"+string(filepath.Separator)) {
			_ = os.Remove(filepath.Join(destination, relative))
		}
		return nil
	})
}
