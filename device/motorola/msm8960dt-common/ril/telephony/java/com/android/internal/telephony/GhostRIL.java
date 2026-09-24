/*
 * Copyright (C) 2016 The CyanogenMod Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

package com.android.internal.telephony;

import static com.android.internal.telephony.RILConstants.*;

import android.content.Context;
import android.os.Message;
import android.os.Parcel;
import android.os.SystemProperties;
import android.provider.Settings;
import android.telecom.TelecomManager;

/**
 * Custom Qualcomm RIL for Moto X
 *
 * {@hide}
 */
public class GhostRIL extends RIL implements CommandsInterface {
    public GhostRIL(Context context, int preferredNetworkType, int cdmaSubscription) {
        super(context, publishEarlyVolteIntent(context, preferredNetworkType),
                cdmaSubscription, null);
    }

    public GhostRIL(Context context, int preferredNetworkType,
            int cdmaSubscription, Integer instanceId) {
        super(context, publishEarlyVolteIntent(context, preferredNetworkType),
                cdmaSubscription, instanceId);
    }

    // Evaluated BEFORE RIL's constructor starts its socket threads. This is
    // fresh user intent, not cached SIM/carrier provisioning or voice readiness.
    // The normal IMS feature request remains authoritative once it arrives.
    private static int publishEarlyVolteIntent(Context context, int networkType) {
        if ("obake_legacy".equals(SystemProperties.get("persist.ims.compat.mode"))
                && SystemProperties.getBoolean("persist.ims.compat.startup", true)) {
            try {
                // Invalidate a previous phone-process hint before reading.
                SystemProperties.set("ril.ims.early_volte", "unknown");
                boolean enabled = Settings.Global.getInt(context.getContentResolver(),
                        Settings.Global.ENHANCED_4G_MODE_ENABLED, 1) == 1;
                boolean nonTty = Settings.Secure.getInt(context.getContentResolver(),
                        Settings.Secure.PREFERRED_TTY_MODE, TelecomManager.TTY_MODE_OFF)
                        == TelecomManager.TTY_MODE_OFF;
                SystemProperties.set("ril.ims.early_volte", enabled && nonTty ? "1" : "0");
            } catch (RuntimeException e) {
                // Unknown is not permission to turn voice on. Normal IMS can
                // still configure it later, including carrier-supported TTY.
                android.telephony.Rlog.w("GhostRIL", "Early VoLTE intent unavailable", e);
            }
        }
        return networkType;
    }

    @Override
    protected Object
    responseFailCause(Parcel p) {
        int numInts;
        int response[];

        numInts = p.readInt();
        response = new int[numInts];
        for (int i = 0 ; i < numInts ; i++) {
            response[i] = p.readInt();
        }
        LastCallFailCause failCause = new LastCallFailCause();
        failCause.causeCode = response[0];
        if (p.dataAvail() > 0) {
          failCause.vendorCause = p.readString();
        }
        return failCause;
    }
}
