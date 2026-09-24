/* Copyright (C) 2026 The XPerience Project */

#ifndef IMS_INITIAL_REGISTRATION_H
#define IMS_INITIAL_REGISTRATION_H

namespace imscompat {

typedef void* (*InitialRegistrationGetRcsObjectFunction)();

// Resolve the exact SU6-7.3 initial-registration ABI. This is observation-only
// until persist.ims.reg.trigger is exactly "enabled".
bool initializeInitialRegistration(
        void* rcsLibrary,
        InitialRegistrationGetRcsObjectFunction getRcsObject);

bool isInitialRegistrationEnabled();

// Invoke the stock trigger once, after validating the live object graph and
// its configured feature table. A failed attempt is not retried until the
// daemon is restarted, avoiding duplicate allocations in the proprietary ABI.
void reconcileInitialRegistration();

// Pair an adapter-owned registration with the stock deregistration path.
void shutdownInitialRegistration();

}  // namespace imscompat

#endif  // IMS_INITIAL_REGISTRATION_H
