package main

import "syscall"

func waitForProcess(pid int) {
	handle, err := syscall.OpenProcess(syscall.SYNCHRONIZE, false, uint32(pid))
	if err != nil {
		return
	}
	defer syscall.CloseHandle(handle)
	_, _ = syscall.WaitForSingleObject(handle, 0xffffffff)
}
