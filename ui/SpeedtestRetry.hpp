#pragma once

#include "db/ProfileTestResult.hpp"

// Only transient URL transport failures qualify. Configuration, certificate,
// authentication and HTTP-status errors must retain their original outcome.
inline bool ShouldRetryUrlTest(const NekoGui::ProfileTestResult &result) {
    if (result.mode != "url" || result.status != NekoGui::TestResultStatus::ConnectionFailure) return false;
    const auto error = result.error.toLower();
    return error.contains("context deadline exceeded") || error.contains("i/o timeout") ||
           error.contains("timed out") || error.contains("timeout awaiting response headers") ||
           error.endsWith(": eof") || error.endsWith(": unexpected eof") ||
           error.contains("read/write on closed pipe") || error.contains("connection reset by peer") ||
           error.contains("forcibly closed by the remote host");
}
