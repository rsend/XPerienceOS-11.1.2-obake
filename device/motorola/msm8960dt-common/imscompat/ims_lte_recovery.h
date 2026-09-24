/* Copyright (C) 2026 The XPerience Project. Licensed under Apache-2.0. */
#ifndef IMS_LTE_RECOVERY_H
#define IMS_LTE_RECOVERY_H
#include <stdint.h>

namespace imscompat {
enum class NasService { kUnknown, kSearching, kLte, kOther, kDenied };
enum class RecoveryAction { kNone, kArm, kRestore };

// No boot-relative expiry. Confirmed absence of service pauses the budget;
// unknown status does not. One bounded attempt per service/IMS acquisition.
class LteRecovery {
  public:
    static constexpr uint64_t kBudgetMs = 120000;
    static constexpr uint64_t kRetryMs = 30000;

    explicit LteRecovery(bool exhausted = false) : exhausted_(exhausted) {}

    RecoveryAction update(uint64_t now, NasService service, bool voiceKnown,
                          bool voiceReady, bool wanted) {
        if (protected_ && sampled_ && previous_ != NasService::kSearching) {
            const uint64_t delta = now >= lastMs_ ? now - lastMs_ : kBudgetMs;
            usedMs_ += delta < kBudgetMs - usedMs_ ? delta : kBudgetMs - usedMs_;
        }
        sampled_ = true;
        lastMs_ = now;
        previous_ = service;

        if (!wanted) {
            usedMs_ = 0;
            settled_ = false;
            exhausted_ = false;
            sawLoss_ = false;
            return restoreWhenAllowed(now);
        }
        if ((service == NasService::kLte && voiceKnown && voiceReady)
                || service == NasService::kOther) {
            // Actual usable service, not a timeout/query error, ends a cycle.
            usedMs_ = 0;
            settled_ = true;
            exhausted_ = false;
            sawLoss_ = false;
            return restoreWhenAllowed(now);
        }
        if (exhausted_) {
            if (service == NasService::kSearching) sawLoss_ = true;
            // Do not rearm on OOS caused by our own timeout/restoration, or
            // while the same LTE attachment continues failing IMS. Require
            // observed loss followed by a NEW LTE registration, plus cooldown.
            if (!protected_ && sawLoss_ && service == NasService::kLte
                    && now >= retryAt_ && now >= rearmAt_) {
                exhausted_ = false;
                sawLoss_ = false;
                settled_ = false;
                usedMs_ = 0;
            } else {
                return restoreWhenAllowed(now);
            }
        }
        if (protected_ && (usedMs_ >= kBudgetMs || service == NasService::kDenied)) {
            exhausted_ = true;
            sawLoss_ = false;
            // Restoration itself is immediate; the next ARM is rate-limited.
            rearmAt_ = now + kRetryMs;
            return restoreWhenAllowed(now);
        }
        if (protected_) return RecoveryAction::kNone;
        if (settled_) {
            if (service == NasService::kSearching
                    || (service == NasService::kLte && voiceKnown && !voiceReady)) {
                settled_ = false; // Real service loss, not an IMSA query failure.
                usedMs_ = 0;
            } else {
                return RecoveryAction::kNone;
            }
        }
        if (now < retryAt_ || now < rearmAt_) return RecoveryAction::kNone;
        if (service == NasService::kSearching || service == NasService::kLte)
            return RecoveryAction::kArm;
        return RecoveryAction::kNone;
    }

    void armed(bool success, uint64_t now) {
        protected_ = success;
        if (!success) retryAt_ = now + kRetryMs;
        // Do not charge time spent executing the preceding GET/SET/GET.
        lastMs_ = now;
    }
    void restored(bool success, uint64_t now) {
        if (success) protected_ = false;
        else retryAt_ = now + kRetryMs;
    }
    bool exhausted() const { return exhausted_; }
    bool protecting() const { return protected_; }
    uint64_t usedMs() const { return usedMs_; }

  private:
    RecoveryAction restoreWhenAllowed(uint64_t now) const {
        return protected_ && now >= retryAt_ ? RecoveryAction::kRestore : RecoveryAction::kNone;
    }
    bool protected_ = false;
    bool settled_ = false;
    bool exhausted_ = false;
    bool sawLoss_ = false;
    bool sampled_ = false;
    NasService previous_ = NasService::kUnknown;
    uint64_t usedMs_ = 0;
    uint64_t lastMs_ = 0;
    uint64_t retryAt_ = 0;
    uint64_t rearmAt_ = 0;
};

// Cheap deterministic transition tests, run with the codec checks before the
// first modem operation. Also callable from the standalone host test (not a
// build step performed by the agent).
inline bool lteRecoverySelfTest() {
    using S = NasService;
    using A = RecoveryAction;
    LteRecovery late;
    if (late.update(0, S::kSearching, false, false, true) != A::kArm) return false;
    late.armed(true, 0);
    if (late.update(600000, S::kSearching, false, false, true) != A::kNone
            || late.usedMs() != 0) return false; // ten minutes without RF
    if (late.update(600001, S::kLte, true, false, true) != A::kNone) return false;
    if (late.update(630001, S::kLte, true, false, true) != A::kNone
            || late.usedMs() != 30000) return false;
    late.update(630002, S::kSearching, false, false, true);
    if (late.update(1230002, S::kSearching, false, false, true) != A::kNone
            || late.usedMs() != 30001) return false; // pause, NOT a reset
    late.update(1230003, S::kLte, true, false, true);
    if (late.update(1260003, S::kLte, true, true, true) != A::kRestore) return false;
    late.restored(true, 1260003);
    if (late.update(1860003, S::kSearching, false, false, true) != A::kArm) return false;
    late.armed(true, 1860003); // a genuine loss after working voice starts a new cycle
    if (late.update(7200000, S::kSearching, false, false, true) != A::kNone
            || late.usedMs() != 0) return false;

    LteRecovery bounded;
    if (bounded.update(0, S::kLte, true, false, true) != A::kArm) return false;
    bounded.armed(true, 0);
    if (bounded.update(120000, S::kLte, true, false, true) != A::kRestore
            || !bounded.exhausted()) return false;
    bounded.restored(true, 120000);
    if (bounded.update(120001, S::kLte, true, false, true) != A::kNone) return false;
    if (bounded.update(120002, S::kSearching, false, false, true) != A::kNone) return false;
    if (bounded.update(120003, S::kLte, true, false, true) != A::kNone) return false;
    if (bounded.update(149999, S::kLte, true, false, true) != A::kNone) return false;
    if (bounded.update(150000, S::kLte, true, false, true) != A::kArm) return false;
    bounded.armed(true, 150000);
    if (bounded.update(150001, S::kDenied, false, false, true) != A::kRestore) return false;
    bounded.restored(true, 150001);
    if (bounded.update(9999999, S::kDenied, false, false, true) != A::kNone) return false;
    if (bounded.update(10000000, S::kUnknown, false, false, true) != A::kNone) return false;

    LteRecovery unknown;
    unknown.update(0, S::kSearching, false, false, true);
    unknown.armed(true, 0);
    unknown.update(1, S::kUnknown, false, false, true);
    if (unknown.update(120001, S::kUnknown, false, false, true) != A::kRestore) return false;
    unknown.restored(true, 120001);
    if (unknown.update(9999999, S::kUnknown, false, false, true) != A::kNone) return false;

    LteRecovery toggles;
    if (toggles.update(0, S::kSearching, false, false, false) != A::kNone) return false;
    if (toggles.update(3600000, S::kSearching, false, false, true) != A::kArm) return false;
    toggles.armed(true, 3600000);
    if (toggles.update(3600001, S::kSearching, false, false, false) != A::kRestore) return false;
    toggles.restored(true, 3600001);
    if (toggles.update(3600002, S::kSearching, false, false, true) != A::kArm) return false;
    toggles.armed(true, 3600002);
    if (toggles.update(3600003, S::kOther, false, false, true) != A::kRestore) return false;
    toggles.restored(true, 3600003);
    if (toggles.update(9999999, S::kOther, false, false, true) != A::kNone) return false;

    LteRecovery voice;
    voice.update(0, S::kLte, true, true, true);
    if (voice.update(1000, S::kLte, false, false, true) != A::kNone) return false;
    if (voice.update(2000, S::kLte, true, false, true) != A::kArm) return false;

    LteRecovery errors;
    errors.update(0, S::kLte, true, false, true);
    errors.armed(false, 0);
    if (errors.update(29999, S::kLte, true, false, true) != A::kNone) return false;
    if (errors.update(30000, S::kLte, true, false, true) != A::kArm) return false;
    errors.armed(true, 30000);
    if (errors.update(150000, S::kLte, true, false, true) != A::kRestore) return false;
    errors.restored(false, 150000);
    if (errors.update(179999, S::kLte, true, false, true) != A::kNone) return false;
    if (errors.update(180000, S::kLte, true, false, true) != A::kRestore) return false;
    errors.restored(true, 180000);
    if (errors.update(9999999, S::kLte, true, false, true) != A::kNone) return false;
    LteRecovery restarted(true);
    return restarted.update(0, S::kLte, false, false, true) == A::kNone;
}
}  // namespace imscompat
#endif
