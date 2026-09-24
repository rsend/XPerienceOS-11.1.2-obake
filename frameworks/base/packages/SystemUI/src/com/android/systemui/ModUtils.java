/*
 * Copyright (C) 2018 The XPerience Project
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
package com.android.systemui;

import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.content.pm.PackageManager.NameNotFoundException;
import android.content.pm.ResolveInfo;
import android.os.SystemProperties;
import android.text.TextUtils;
import android.util.Base64;
import android.util.Log;
import java.nio.ByteBuffer;
import java.util.List;

public class ModUtils {
    
    private static final Intent DYNAMIC_MOD_BATTERY_HISTORY_CHART_INTENT = new Intent("com.motorola.extensions.settings.MODS_BATTERY_HISTORY_CHART");
    private static String TAG = ModUtils.class.getSimpleName();
	
	private static final boolean DEBUG = Log.isLoggable(TAG, 3);

    public static int getModBatteryLevel(Intent batteryChangedIntent) {
        int level = batteryChangedIntent.getIntExtra("mod_level", -1);
        return (level * 100) / batteryChangedIntent.getIntExtra("scale", 100);
    }

    public static int getModBatteryType(Intent batteryChangedIntent) {
        return batteryChangedIntent.getIntExtra("mod_type", 0);
    }

    public static boolean isModActive(int device_level, int mod_level, int mod_type) {
        if (mod_level <= 0) {
            return false;
        }
        if (DEBUG) {
            Log.d(TAG, "isModActive ; Battery Type is " + mod_type);
        }
        if (mod_type != 2) {
            return false;
        }
        String mode = SystemProperties.get("sys.mod.batterymode");
        Log.d(TAG, "isModActive ; Battery Mode is " + mode);
        return "0".equals(mode) || device_level <= 80;
    }

    public static String getModDisplayName() {
        String name = SystemProperties.get("sys.mod.displayname");
        Log.d(TAG, "getModDisplayName; Name is " + name);
        return name;
    }

    public static boolean isModAttached(int modStatus, int modLevel) {
        return modStatus != 1 && modLevel >= 0;
    }

    public static boolean isModAttached() {
        String attached = SystemProperties.get("sys.mod.current");
        Log.d(TAG, "isModAttached: " + attached);
        if (attached != null) {
            Log.d(TAG, "isModAttached: " + attached.trim().length());
        }
        if (attached == null || attached.trim().length() == 0) {
            return false;
        }
        return true;
    }

    public static String getModChargingLabel(Context context, Intent batteryIntent) {
        int resId;
        int plugType = batteryIntent.getIntExtra("plugged_raw", batteryIntent.getIntExtra("plugged", 0));
        if (plugType == 1) {
            resId = R.string.power_charging_ac;
        } else if (plugType == 2) {
            resId = R.string.power_charging_usb;
        } else if (plugType == 4) {
            resId = R.string.power_charging_wireless;
        } else {
            resId = R.string.power_charging;
        }
        return context.getResources().getString(resId);
    }

    public static boolean showModQsTile(Context context) {
        PackageManager pm = context.getPackageManager();
        List<ResolveInfo> activities = context.getPackageManager().queryIntentActivities(DYNAMIC_MOD_BATTERY_HISTORY_CHART_INTENT, 128);
        if (activities == null) {
            return false;
        }
        for (ResolveInfo activity : activities) {
            try {
                if ((pm.getApplicationInfo(activity.activityInfo.packageName, 0).flags & 1) != 0) {
                    return true;
                }
            } catch (NameNotFoundException e) {
            }
        }
        return false;
    }

    public static boolean isAProjectorMod() {
        boolean isProjector = false;
        String current = SystemProperties.get("sys.mod.current");
        if (!TextUtils.isEmpty(current)) {
            ByteBuffer buf = ByteBuffer.wrap(Base64.decode(current.substring(0, 32), 0));
            if (buf != null) {
                int vid = buf.getInt();
                int pid = buf.getInt();
                if (vid == 296 && (983040 & pid) == 327680) {
                    isProjector = true;
                }
            }
        }
        Log.d(TAG, "isAProjectorMod isProjector = " + isProjector);
        return isProjector;
    }
}