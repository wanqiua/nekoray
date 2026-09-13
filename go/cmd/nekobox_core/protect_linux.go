package main

import "github.com/matsuridayo/libneko/protect_server"

func startProtection(path string, mark int) error {
	go protect_server.ServeProtect(path, mark)
	return nil
}
