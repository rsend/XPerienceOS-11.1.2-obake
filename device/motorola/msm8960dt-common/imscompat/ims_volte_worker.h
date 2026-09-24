/* Copyright (C) 2026 The XPerience Project. Licensed under Apache-2.0. */
#ifndef IMS_VOLTE_WORKER_H
#define IMS_VOLTE_WORKER_H
#include <stdint.h>
#include <sys/types.h>

namespace imscompat {
struct VolteWorkerResult {
    uint32_t token = 0;
    int exitCode = -1;
    bool successful = false;
    bool retryable = false;
};

// One child process across all relay connections. Never wait synchronously in
// the call/hangup relay. A cancelled child is reaped before any new one starts.
class VolteWorker {
  public:
    bool start(uint32_t token, bool enabled, uint64_t nowMs, uint64_t timeoutMs = 48000);
    bool poll(uint64_t nowMs, VolteWorkerResult* result);
    void cancel();
    bool busy() const { return pid_ > 0; }

  private:
    pid_t pid_ = -1;
    uint32_t token_ = 0;
    uint64_t deadlineMs_ = 0;
    bool discard_ = false;
    bool killed_ = false;
};
}  // namespace imscompat
#endif
