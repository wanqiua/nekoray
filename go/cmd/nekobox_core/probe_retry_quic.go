//go:build with_quic

package main

import (
	"errors"

	"github.com/sagernet/quic-go"
)

func isLocalQUICClose(err error) bool {
	var closeError *quic.ApplicationError
	return errors.As(err, &closeError) && !closeError.Remote && closeError.ErrorCode == 0
}
