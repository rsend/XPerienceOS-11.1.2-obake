/* Copyright (C) 2026 The XPerience Project */

#ifndef IMS_SERVICE_LIFECYCLE_H
#define IMS_SERVICE_LIFECYCLE_H

namespace imscompat {

typedef void* (*GetRcsObjectFunction)();

// Resolve the unmodified stock MMTEL service-monitor ABI. Initialization is
// observation-only; dispatch remains disabled unless the semantic kill switch
// persist.ims.service.lifecycle is exactly "enabled".
bool initializeServiceLifecycle(GetRcsObjectFunction getRcsObject);

// Reconcile the latest acknowledged framework service state with the stock
// modem-facing AddService/RemoveService lifecycle. A late subscriber first
// queries and restores the stock registration-manager PowerUp subscription.
// Sends then retry at a bounded rate until the DCM PDP callback confirms the
// IMS bearer.
void reconcileServiceLifecycle();

// Best-effort paired removal for orderly DPL shutdown/loss of readiness.
void shutdownServiceLifecycle();

}  // namespace imscompat

#endif  // IMS_SERVICE_LIFECYCLE_H
