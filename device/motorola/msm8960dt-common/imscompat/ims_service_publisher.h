/* Copyright (C) 2026 The XPerience Project */

#ifndef IMS_SERVICE_PUBLISHER_H
#define IMS_SERVICE_PUBLISHER_H

#include "ims_service_state.h"

#include <stdint.h>

namespace imscompat {

class ServiceStatePublisher {
  public:
    void markDirty();
    void tick(const ServiceStateTracker& services, uint64_t nowMs);
    bool dirty() const;

  private:
    bool sendSnapshot(const ServiceStateTracker& services);
    void scheduleRetry(uint64_t nowMs);

    uint32_t sequence_ = 0;
    uint32_t failures_ = 0;
    uint64_t nextAttemptMs_ = 0;
    uint64_t nextReceiverCheckMs_ = 0;
    bool dirty_ = false;
};

}  // namespace imscompat

#endif  // IMS_SERVICE_PUBLISHER_H
