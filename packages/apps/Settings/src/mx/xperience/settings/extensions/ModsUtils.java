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
package mx.xperience.settings.extensions;

import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.content.pm.ResolveInfo;
import android.content.res.Resources;
import android.os.BatteryStats;
import android.support.v7.preference.PreferenceViewHolder;
import android.widget.ImageView;
import android.widget.TextView;
import com.android.settings.R;
import com.android.settingslib.Utils;
import mx.xperience.settings.extensions.internal.SystemUtils;
import java.util.List;

public final class ModsUtils {
    private static final Intent DYNAMIC_MOD_BATTERY_HISTORY_CHART_INTENT = new Intent("com.motorola.extensions.settings.MODS_BATTERY_HISTORY_CHART");

    public static int getModBatteryLevel(Intent batteryChangedIntent) {
        int level = batteryChangedIntent.getIntExtra("mod_level", -1);
        return (level * 100) / batteryChangedIntent.getIntExtra("scale", 100);
    }

    public static String getModBatteryPercentage(Intent batteryChangedIntent) {
        return Utils.formatPercentage(getModBatteryLevel(batteryChangedIntent));
    }

    public static String getModBatteryStatus(Resources res, Intent batteryChangedIntent) {
        Intent intent = batteryChangedIntent;
        int plugType = batteryChangedIntent.getIntExtra("plugged_raw", batteryChangedIntent.getIntExtra("plugged", 0));
        return getBatteryStatusString(res, plugType, getModStatus(batteryChangedIntent.getIntExtra("mod_status", 1), batteryChangedIntent.getIntExtra("mod_type", 0), plugType));
    }

    private static String getBatteryStatusString(Resources res, int plugType, int status) {
        if (status == 2) {
            int resId;
            if (plugType == 1) {
                resId = R.string.battery_info_status_charging_ac;
            } else if (plugType == 2) {
                resId = R.string.battery_info_status_charging_usb;
            } else if (plugType == 4) {
                resId = R.string.battery_info_status_charging_wireless;
            } else {
                resId = R.string.battery_info_status_charging;
            }
            return res.getString(resId);
        } else if (status == 3) {
            return res.getString(R.string.battery_info_status_discharging);
        } else {
            if (status == 4) {
                return res.getString(R.string.battery_info_status_not_charging);
            }
            if (status == 5) {
                return res.getString(R.string.battery_info_status_full);
            }
            return res.getString(R.string.battery_info_status_unknown);
        }
    }

    public static boolean isModAttached(Intent batteryChangedIntent) {
        if (batteryChangedIntent == null) {
            return false;
        }
        return getModBatteryLevel(batteryChangedIntent) >= 0 && batteryChangedIntent.getIntExtra("mod_status", 1) != 1;
    }

    private static int getModStatus(int modStatus, int batteryType, int plugType) {
        if (modStatus == 2 && batteryType == 2 && !externalChargerPlugged(plugType)) {
            return 4;
        }
        return modStatus;
    }

    private static boolean externalChargerPlugged(int plugType) {
        if (plugType == 0 || plugType == 8) {
            return false;
        }
        return true;
    }

    public static void setBatteryLabels(Context context, BatteryStats stats, PreferenceViewHolder view, Intent batteryChangedIntent) {
        int modLevel = getModBatteryLevel(batteryChangedIntent);
        int battLevel = Utils.getBatteryLevel(batteryChangedIntent);
        if (modLevel >= 0) {
            view.findViewById(R.id.mod_desc).setVisibility(0);
            view.findViewById(R.id.mod_icon).setVisibility(0);
            view.findViewById(R.id.mod_text).setVisibility(0);
            ((TextView) view.findViewById(R.id.core_desc)).setText(Utils.formatPercentage(battLevel));
            ((TextView) view.findViewById(R.id.mod_desc)).setText(Utils.formatPercentage(modLevel));
            ((TextView) view.findViewById(R.id.mod_text)).setText(context.getResources().getString(R.string.battery_manager_mod_sub_desc));
        } else {
            view.findViewById(R.id.mod_desc).setVisibility(8);
            view.findViewById(R.id.mod_icon).setVisibility(8);
            view.findViewById(R.id.mod_text).setVisibility(8);
            ((TextView) view.findViewById(R.id.core_desc)).setText(Utils.formatPercentage(battLevel));
        }
        int plugType = batteryChangedIntent.getIntExtra("plugged_raw", batteryChangedIntent.getIntExtra("plugged", 0));
        int modStatus = getModStatus(batteryChangedIntent.getIntExtra("mod_status", 1), batteryChangedIntent.getIntExtra("mod_type", 0), plugType);
        int status = batteryChangedIntent.getIntExtra("status", 1);
        if (modStatus == 2) {
            ((ImageView) view.findViewById(R.id.mod_icon)).setImageResource(R.drawable.ic_mod_battery_mgr_charging);
            ((TextView) view.findViewById(R.id.mod_text)).setText(context.getResources().getString(R.string.battery_manager_charging_sub_desc));
        } else {
            ((ImageView) view.findViewById(R.id.mod_icon)).setImageResource(R.drawable.ic_mod_battery_mgr);
            ((TextView) view.findViewById(R.id.mod_text)).setText(context.getResources().getString(R.string.battery_manager_mod_sub_desc));
        }
        if (status == 2) {
            ((ImageView) view.findViewById(R.id.core_icon)).setImageResource(R.drawable.ic_mod_battery_mgr_phone_charging);
            ((TextView) view.findViewById(R.id.core_text)).setText(context.getResources().getString(R.string.battery_manager_charging_sub_desc));
            return;
        }
        ((ImageView) view.findViewById(R.id.core_icon)).setImageResource(R.drawable.ic_mod_battery_mgr_phone);
        ((TextView) view.findViewById(R.id.core_text)).setText(context.getResources().getString(R.string.battery_manager_core_sub_desc));
    }

    public static boolean showModsHistoryChart(Context context) {
        /*PackageManager pm = context.getPackageManager();
        if (!SystemUtils.isSystemOrMotoApp(pm, context.getPackageName())) {
            return false;
        }*/
        List<ResolveInfo> activities = context.getPackageManager().queryIntentActivities(DYNAMIC_MOD_BATTERY_HISTORY_CHART_INTENT, 128);
        if (activities == null) {
            return false;
        }
        /*for (ResolveInfo activity : activities) {
            if (SystemUtils.isSystemOrMotoApp(pm, activity.activityInfo.packageName)) {
                return true;
            }
        }*/
        return false;
    }
}