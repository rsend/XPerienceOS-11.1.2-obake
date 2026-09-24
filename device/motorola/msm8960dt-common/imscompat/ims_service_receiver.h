/* Copyright (C) 2026 The XPerience Project */

#ifndef IMS_SERVICE_RECEIVER_H
#define IMS_SERVICE_RECEIVER_H

#include "ims_service_control.h"

namespace imscompat {

// Starts the observation-only Step 3 receiver. Failure is exposed through
// sys.ims.dpl.svc.status while the existing DPL baseline remains available as
// an explicit degraded mode.
bool startServiceStateReceiver();

// Returns an atomic copy of the most recently authenticated and decoded
// snapshot. The caller cannot observe a partially updated table.
bool getServiceStateSnapshot(ServiceStateSnapshot* snapshot);

}  // namespace imscompat

#endif  // IMS_SERVICE_RECEIVER_H
