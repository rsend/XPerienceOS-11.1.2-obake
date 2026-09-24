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
package com.android.systemui.qs.tiles;

import android.app.ActivityManager;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.graphics.drawable.Drawable;
import android.os.Handler;
import android.os.Looper;
import android.os.UserHandle;
import android.text.SpannableStringBuilder;
import android.text.style.RelativeSizeSpan;
import android.util.Log;
import android.view.LayoutInflater;
import android.view.View;
import android.view.View.OnAttachStateChangeListener;
import android.view.View.OnClickListener;
import android.view.ViewGroup;
import android.widget.RadioButton;
import android.widget.TextView;
import com.android.settingslib.Utils;
import com.android.systemui.ModUtils;
import com.android.systemui.ModsBatteryMeterDrawable;
import com.android.systemui.R;
import com.android.systemui.qs.QSTile;
import com.android.systemui.qs.QSTile.DetailAdapter;
import com.android.systemui.qs.QSTile.DrawableIcon;
import com.android.systemui.qs.QSTile.Host;
import com.android.systemui.qs.QSTile.Icon;
import com.android.systemui.qs.QSTile.State;
import com.android.systemui.statusbar.policy.BatteryController;
import com.android.systemui.statusbar.policy.BatteryController.BatteryStateChangeCallback;
import java.text.NumberFormat;
import java.util.ArrayList;
import java.util.HashMap;


public class ModBatteryTile extends QSTile<State> implements BatteryStateChangeCallback {
    private static final boolean DEBUG = Log.isLoggable("ModBatteryTile", 3);
    private final int MOD_METRICS_CATEGORY = 999999;
    private final int UNKNOWN_LEVEL = -1;
    private final BatteryController mBatteryController;
    private final BatteryDetail mBatteryDetail = new BatteryDetail();
    private boolean mCharging;
    private final Context mContext;
    private boolean mDetailShown;
    private int mLevel = -1;
    private final BroadcastReceiver mModReceiver = new C04331();
    private boolean mPluggedIn;
    private boolean mPowerSave;

    class C04331 extends BroadcastReceiver {
        C04331() {
        }

        public void onReceive(Context context, Intent intent) {
            if (ModBatteryTile.DEBUG) {
                Log.d("ModBatteryTile", "Received " + intent);
            }
            if (intent == null) {
                return;
            }
            if ("com.motorola.mod.action.MOD_ATTACH".equals(intent.getAction())) {
                ModBatteryTile.this.mBatteryController.addStateChangedCallback(ModBatteryTile.this);
            } else if ("com.motorola.mod.action.MOD_DETACH".equals(intent.getAction())) {
                ModBatteryTile.this.mBatteryController.removeStateChangedCallback(ModBatteryTile.this);
                ModBatteryTile.this.mLevel = -1;
                ModBatteryTile.this.refreshState(null);
            }
        }
    }

    class C04342 extends Icon {
        C04342() {
        }

        public Drawable getDrawable(Context context) {
            ModsBatteryMeterDrawable drawable = new ModsBatteryMeterDrawable(context, new Handler(Looper.getMainLooper()), context.getColor(R.color.batterymeter_frame_color));
            drawable.onBatteryLevelChanged(ModBatteryTile.this.mLevel, ModBatteryTile.this.mPluggedIn, ModBatteryTile.this.mCharging);
            return drawable;
        }

        public int getPadding() {
            return ModBatteryTile.this.mHost.getContext().getResources().getDimensionPixelSize(R.dimen.qs_battery_padding);
        }
    }

    private final class BatteryDetail implements DetailAdapter, OnAttachStateChangeListener {
        private View mCurrentView;
        private final ModsBatteryMeterDrawable mDrawable;
        private final BroadcastReceiver mReceiver;

        class C04351 extends BroadcastReceiver {
            C04351() {
            }

            public void onReceive(Context context, Intent intent) {
                BatteryDetail.this.postBindView();
            }
        }

        class C04362 implements Runnable {
            C04362() {
            }

            public void run() {
                BatteryDetail.this.bindView();
            }
        }

        class C04373 implements OnClickListener {
            C04373() {
            }

            public void onClick(View v) {
                if (ModBatteryTile.DEBUG) {
                    Log.d("ModBatteryTile", "onClick");
                }
                Intent newIntent = new Intent("com.motorola.modservice.ui.action.SETTINGS");
                newIntent.putExtra("display_picker", true);
                if (ModBatteryTile.DEBUG) {
                    Log.d("ModBatteryTile", "Sending intent " + newIntent.toUri(0));
                }
                ModBatteryTile.this.mHost.startActivityDismissingKeyguard(newIntent);
            }
        }

        private BatteryDetail() {
            this.mDrawable = new ModsBatteryMeterDrawable(ModBatteryTile.this.mHost.getContext(), new Handler(), ModBatteryTile.this.mHost.getContext().getColor(R.color.batterymeter_frame_color));
            this.mReceiver = new C04351();
        }

        public CharSequence getTitle() {
            return ModBatteryTile.this.mContext.getString(R.string.moto_mods, new Object[]{Integer.valueOf(ModBatteryTile.this.mLevel)});
        }

        public Boolean getToggleState() {
            return null;
        }

        public View createDetailView(Context context, View convertView, ViewGroup parent) {
            if (convertView == null) {
                convertView = LayoutInflater.from(ModBatteryTile.this.mContext).inflate(R.layout.mod_battery_detail, parent, false);
            }
            this.mCurrentView = convertView;
            this.mCurrentView.addOnAttachStateChangeListener(this);
            bindView();
            return convertView;
        }

        private void postBindView() {
            if (this.mCurrentView != null) {
                this.mCurrentView.post(new C04362());
            }
        }

        private void bindView() {
            if (this.mCurrentView != null) {
                updateBatteryInfo();
            }
        }

        private void updateBatteryInfo() {
            if (ModBatteryTile.DEBUG) {
                Log.d("ModBatteryTile", "updateBatteryInfo");
            }
            Intent batteryBroadcast = ModBatteryTile.this.mContext.registerReceiver(null, new IntentFilter("android.intent.action.BATTERY_CHANGED"));
            int modLevel = ModUtils.getModBatteryLevel(batteryBroadcast);
            if (modLevel < 0) {
                ModBatteryTile.this.showDetail(false);
                return;
            }
            String mod_label;
            TextView nameView = (TextView) this.mCurrentView.findViewById(R.id.mod_name);
            String mod_name = ModUtils.getModDisplayName();
            if (mod_name != null) {
                nameView.setVisibility(0);
                nameView.setText(mod_name);
            } else {
                nameView.setVisibility(8);
            }
            SpannableStringBuilder builder = new SpannableStringBuilder();
            builder.append(Utils.formatPercentage(modLevel), new RelativeSizeSpan(2.6f), 17);
            if (ModBatteryTile.this.mCharging) {
                mod_label = ModUtils.getModChargingLabel(ModBatteryTile.this.mContext, batteryBroadcast);
            } else {
                mod_label = ModBatteryTile.this.mContext.getResources().getString(R.string.mod_remaining_label);
            }
            if (mod_label != null) {
                if (ModBatteryTile.this.mContext.getResources().getBoolean(R.bool.quick_settings_wide)) {
                    builder.append(' ');
                } else {
                    builder.append('\n');
                }
                builder.append(mod_label);
            }
            ((TextView) this.mCurrentView.findViewById(R.id.charge_and_estimation)).setText(builder);
            ArrayList<HashMap<String, Object>> m_data = new ArrayList();
            int batteryType = ModUtils.getModBatteryType(batteryBroadcast);
            if (ModUtils.getModBatteryType(batteryBroadcast) != 2) {
                this.mCurrentView.findViewById(R.id.divider).setVisibility(8);
                this.mCurrentView.findViewById(R.id.list_view_title).setVisibility(8);
                return;
            }
            this.mCurrentView.findViewById(R.id.divider).setVisibility(0);
            TextView modeView = (TextView) this.mCurrentView.findViewById(R.id.list_view_title);
            modeView.setVisibility(0);
            modeView.setOnClickListener(new C04373());
        }

        public Intent getSettingsIntent() {
            return new Intent("com.motorola.modservice.ui.action.SETTINGS");
        }

        public void setToggleState(boolean state) {
        }

        public int getMetricsCategory() {
            return 999999;
        }

        public void onViewAttachedToWindow(View v) {
            if (!ModBatteryTile.this.mDetailShown) {
                ModBatteryTile.this.mDetailShown = true;
                v.getContext().registerReceiver(this.mReceiver, new IntentFilter("android.intent.action.TIME_TICK"));
            }
        }

        public void onViewDetachedFromWindow(View v) {
            if (ModBatteryTile.this.mDetailShown) {
                ModBatteryTile.this.mDetailShown = false;
                v.getContext().unregisterReceiver(this.mReceiver);
            }
        }
    }

    public ModBatteryTile(Host host) {
        super(host);
        this.mBatteryController = host.getBatteryController();
        this.mContext = host.getContext();
    }

    public State newTileState() {
        return new State();
    }

    public DetailAdapter getDetailAdapter() {
        return this.mBatteryDetail;
    }

    public int getMetricsCategory() {
        return 999999;
    }

    public void setListening(boolean listening) {
        if (DEBUG) {
            Log.d("ModBatteryTile", "setListening listening is " + listening);
        }
        if (listening) {
            if (ModUtils.isModAttached()) {
                if (DEBUG) {
                    Log.d("ModBatteryTile", "Mod is attached, register for battery");
                }
                this.mBatteryController.addStateChangedCallback(this);
            } else {
                this.mLevel = -1;
                this.mPluggedIn = false;
                this.mCharging = false;
            }
            if (DEBUG) {
                Log.d("ModBatteryTile", "Mod is attached, register for Mod attach/detach");
            }
            IntentFilter filter = new IntentFilter("com.motorola.mod.action.MOD_ATTACH");
            filter.addAction("com.motorola.mod.action.MOD_DETACH");
            this.mContext.registerReceiverAsUser(this.mModReceiver, new UserHandle(ActivityManager.getCurrentUser()), filter, "com.motorola.mod.permission.MOD_INTERNAL", null);
            return;
        }
        if (DEBUG) {
            Log.d("ModBatteryTile", "unregister listeners");
        }
        this.mBatteryController.removeStateChangedCallback(this);
        this.mContext.unregisterReceiver(this.mModReceiver);
    }

    public void setDetailListening(boolean listening) {
        super.setDetailListening(listening);
        if (!listening) {
            this.mBatteryDetail.mCurrentView = null;
        }
    }

    public Intent getLongClickIntent() {
        return new Intent("com.motorola.modservice.ui.action.SETTINGS");
    }

    protected void handleClick() {
        if (DEBUG) {
            Log.d("ModBatteryTile", "handleClick level is " + this.mLevel);
        }
        if (this.mLevel >= 0) {
            showDetail(true);
            return;
        }
        this.mHost.startActivityDismissingKeyguard(new Intent("com.motorola.modservice.ui.action.SETTINGS"));
    }

    public CharSequence getTileLabel() {
        return this.mContext.getString(R.string.moto_mods);
    }

    protected void handleUpdateState(State state, Object arg) {
        if (DEBUG) {
            Log.d("ModBatteryTile", "handleUpdateState mLevel is " + this.mLevel);
        }
        int level = arg != null ? ((Integer) arg).intValue() : this.mLevel;
        if (DEBUG) {
            Log.d("ModBatteryTile", "handleUpdateState level is " + level);
        }
        String string;
        if (level >= 0) {
            String percentage = NumberFormat.getPercentInstance().format(((double) level) / 100.0d);
            state.icon = new C04342();
            state.label = percentage;
            StringBuilder append = new StringBuilder().append(this.mContext.getString(R.string.accessibility_quick_settings_battery, new Object[]{percentage})).append(",");
            if (this.mPowerSave) {
                string = this.mContext.getString(R.string.battery_saver_notification_title);
            } else if (this.mCharging) {
                string = this.mContext.getString(R.string.expanded_header_battery_charging);
            } else {
                string = "";
            }
            state.contentDescription = append.append(string).append(",").append(this.mContext.getString(R.string.accessibility_battery_details)).toString();
            string = RadioButton.class.getName();
            state.expandedAccessibilityClassName = string;
            state.minimalAccessibilityClassName = string;
        } else if (ModUtils.isModAttached()) {
            state.icon = new DrawableIcon(this.mHost.getContext().getDrawable(R.drawable.ic_mod));
            state.label = this.mContext.getString(R.string.moto_mods);
            if (DEBUG) {
                Log.d("ModBatteryTile", "No mod battery label is " + state.label);
            }
            state.contentDescription = this.mContext.getString(R.string.moto_mods);
            string = RadioButton.class.getName();
            state.expandedAccessibilityClassName = string;
            state.minimalAccessibilityClassName = string;
        } else {
            Drawable icon = this.mHost.getContext().getDrawable(R.drawable.ic_mod).mutate();
            icon.setTint(this.mHost.getContext().getColor(R.color.qs_tile_tint_unavailable));
            state.icon = new DrawableIcon(icon);
            state.label = this.mContext.getString(R.string.moto_mods);
            if (DEBUG) {
                Log.d("ModBatteryTile", "No mod label is " + state.label);
            }
            state.contentDescription = this.mContext.getString(R.string.moto_mods);
            string = RadioButton.class.getName();
            state.expandedAccessibilityClassName = string;
            state.minimalAccessibilityClassName = string;
        }
    }

    public void onBatteryLevelChanged(int level, boolean pluggedIn, boolean charging) {
        boolean z = false;
        if (DEBUG) {
            Log.d("ModBatteryTile", "onBatteryLevelChanged " + level + ":" + pluggedIn + ":" + charging);
        }
        Intent batteryIntent = this.mContext.registerReceiver(null, new IntentFilter("android.intent.action.BATTERY_CHANGED"));
        if (batteryIntent != null) {
            boolean z2;
            if (DEBUG) {
                Log.d("ModBatteryTile", "onBatteryLevelChanged intent is " + batteryIntent.toUri(0));
            }
            this.mLevel = (int) ((((float) batteryIntent.getIntExtra("mod_level", -1)) * 100.0f) / ((float) batteryIntent.getIntExtra("scale", 100)));
            if (batteryIntent.getIntExtra("mod_status", 1) == 2) {
                z2 = true;
            } else {
                z2 = false;
            }
            this.mCharging = z2;
            int plugType = batteryIntent.getIntExtra("plugged_raw", batteryIntent.getIntExtra("plugged", 0));
            if (!(plugType == 0 || plugType == 8)) {
                z = true;
            }
            this.mPluggedIn = z;
        } else {
            this.mLevel = -1;
            this.mPluggedIn = false;
            this.mCharging = false;
        }
        if (DEBUG) {
            Log.d("ModBatteryTile", "onBatteryLevelChanged " + this.mLevel + ":" + this.mPluggedIn + ":" + this.mCharging);
        }
        refreshState(Integer.valueOf(this.mLevel));
        if (this.mDetailShown) {
            this.mBatteryDetail.postBindView();
        }
    }

    public void onPowerSaveChanged(boolean isPowerSave) {
        this.mPowerSave = isPowerSave;
        refreshState(null);
        if (this.mDetailShown) {
            this.mBatteryDetail.postBindView();
        }
    }
}