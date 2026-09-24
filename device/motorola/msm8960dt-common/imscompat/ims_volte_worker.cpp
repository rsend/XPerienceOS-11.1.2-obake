/* Copyright (C) 2026 The XPerience Project. Licensed under Apache-2.0. */
#define LOG_TAG "ims_volte_worker"
#include "ims_volte_worker.h"
#include <errno.h>
#include <fcntl.h>
#include <log/log.h>
#include <signal.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

namespace imscompat {

namespace {
const char* exitReason(int code) {
    switch (code) {
        case 2: return "invalid_arguments";
        case 3: return "library_or_dependency_closure";
        case 4: return "client_registration_or_release";
        case 5: return "qmi_request_rejected";
        case 6: return "qmi_callback_timeout";
        case 7: return "modem_error_or_readback_mismatch";
        case 8: return "malformed_response_or_abi";
        case 9: return "unrelated_qipcall_field_changed";
        case 124: return "helper_watchdog";
        case 125: return "child_setup_failed";
        case 127: return "helper_exec_failed";
        default: return "child_signalled_or_wait_error";
    }
}
}  // namespace

bool VolteWorker::start(uint32_t token, bool enabled, uint64_t nowMs, uint64_t timeoutMs) {
    if (busy()) return false;
    const pid_t parent = getpid();
    const pid_t child = fork();
    if (child < 0) {
        ALOGW("token=%u fork failed: %s", token, strerror(errno));
        return false;
    }
    if (child == 0) {
        // No shell or user-controlled executable/arguments/environment.
        if (prctl(PR_SET_PDEATHSIG, SIGKILL) != 0 || getppid() != parent) _exit(125);
        const int nullFd = open("/dev/null", O_RDWR | O_CLOEXEC);
        if (nullFd < 0) _exit(125);
        for (int fd = 0; fd < 3; ++fd) if (dup2(nullFd, fd) < 0) _exit(125);
        if (nullFd > 2) close(nullFd);
        char executable[] = "/system/bin/ims_settings_probe";
        char command[] = "ensure-volte";
        char value[] = {enabled ? '1' : '0', '\0'};
        char libraryPath[] = "LD_LIBRARY_PATH=/system/lib/radio-su6-73:/system/lib:/system/vendor/lib";
        // execve intentionally drops inherited variables. SU6 dependencies
        // need Motorola's logging ABI, normally supplied by init's shim map.
        char logShim[] = "LD_PRELOAD=/system/lib/libshim_log.so";
        char* const argv[] = {executable, command, value, nullptr};
        char* const env[] = {libraryPath, logShim, nullptr};
        execve(executable, argv, env);
        _exit(127);
    }
    pid_ = child;
    token_ = token;
    // Normal jobs retain 48s (helper watchdog 45s). An early best-effort job
    // gets less time so it cannot exhaust a queued framework request's 60s.
    deadlineMs_ = nowMs + (timeoutMs < 48000 ? timeoutMs : 48000);
    discard_ = false;
    killed_ = false;
    return true;
}

bool VolteWorker::poll(uint64_t nowMs, VolteWorkerResult* result) {
    if (!busy() || result == nullptr) return false;
    if (!killed_ && nowMs >= deadlineMs_) {
        ALOGW("token=%u child deadline exceeded; outcome unknown", token_);
        if (kill(pid_, SIGKILL) != 0 && errno != ESRCH) {
            ALOGE("token=%u cannot terminate child: %s", token_, strerror(errno));
        }
        killed_ = true;
    }
    int status = 0;
    const pid_t reaped = waitpid(pid_, &status, WNOHANG);
    if (reaped == 0 || (reaped < 0 && errno == EINTR)) return false;
    const int waitError = reaped < 0 ? errno : 0;
    if (waitError != 0) ALOGE("token=%u waitpid failed: %s", token_, strerror(waitError));
    result->token = token_;
    result->exitCode = reaped > 0 && WIFEXITED(status) ? WEXITSTATUS(status) : 128;
    result->successful = !killed_ && waitError == 0 && result->exitCode == 0;
    // Never retry usage/library/ABI/preservation/exec errors automatically.
    result->retryable = waitError == 0 && (result->exitCode == 4 || result->exitCode == 5
            || result->exitCode == 6 || result->exitCode == 7 || result->exitCode == 124);
    if (!result->successful && !discard_) {
        ALOGW("dependency=ims_settings_probe token=%u exit=%d failure=%s killed=%d retryable=%d",
              token_, result->exitCode, exitReason(result->exitCode), killed_, result->retryable);
    }
    pid_ = -1;
    return !discard_;
}

void VolteWorker::cancel() {
    discard_ = true;
    if (busy() && !killed_) {
        if (kill(pid_, SIGKILL) != 0 && errno != ESRCH) {
            ALOGE("token=%u cannot cancel child: %s", token_, strerror(errno));
        }
        killed_ = true;
    }
}

}  // namespace imscompat
