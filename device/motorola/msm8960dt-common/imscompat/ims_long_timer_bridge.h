/* Copyright (C) 2026 The XPerience Project */

#ifndef IMS_LONG_TIMER_BRIDGE_H
#define IMS_LONG_TIMER_BRIDGE_H

namespace imscompat {

// Repair the two lib-imsdpl callback slots used for timers longer than one
// second. The stock library resolves them from a hard-coded, mismatched
// /vendor lib-imsrcs. rcsLibrary must be the already-selected coherent RCS
// library handle used by the bootstrap daemon.
bool initializeLongTimerBridge(void* rcsLibrary);

bool isLongTimerBridgeEnabled();

}  // namespace imscompat

#endif  // IMS_LONG_TIMER_BRIDGE_H
