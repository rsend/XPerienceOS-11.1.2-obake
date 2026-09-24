/*
 * Copyright (C) 2026 The Android Open Source Project
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

import android.os.AsyncResult;
import android.os.Handler;
import android.os.Looper;
import android.os.Message;
import android.os.SystemProperties;
import android.telephony.Rlog;
import android.telephony.ServiceState;
import android.telephony.SubscriptionManager;
import android.telephony.TelephonyManager;

import com.android.internal.telephony.uicc.IccRecords;

import java.io.PrintWriter;
import java.util.ArrayList;

/** Opt-in home-network policy. All entry points run on the Phone looper. */
class HomeNetworkSelection extends Handler {
    static final String PROPERTY = "persist.radio.home_auto_sel";
    private static final String TAG = "HomeNetworkSelection";
    private static final int CHECK = 1;
    private static final int SET = 2;
    private static final int VERIFY = 3;
    static final int TIMEOUT = 4;
    // Legacy NAS automatic selection may take 60s, even when mode0 is already applied.
    private static final long DEADLINE_MS = 75000;
    private static final int MAX_WAITERS = 4;

    private final Phone mPhone;
    private final CommandsInterface mCi;
    private Request mPending;
    private IccRecords mRecords;
    private int mSubId = SubscriptionManager.INVALID_SUBSCRIPTION_ID;
    private int mSequence;
    private boolean mAttempted;
    private String mOutcome = "idle";

    private static final class Request {
        int sequence;
        int subId;
        IccRecords records;
        String homeOperator;
        boolean setReportedError;
        final ArrayList<Message> responses = new ArrayList<Message>();
    }

    HomeNetworkSelection(Phone phone, CommandsInterface ci) {
        this(phone, ci, phone.getLooper());
    }

    HomeNetworkSelection(Phone phone, CommandsInterface ci, Looper looper) {
        super(looper);
        mPhone = phone;
        mCi = ci;
    }

    static boolean validOperator(String operator) {
        if (operator == null || (operator.length() != 5 && operator.length() != 6)) {
            return false;
        }
        for (int i = 0; i < operator.length(); i++) {
            if (operator.charAt(i) < '0' || operator.charAt(i) > '9') return false;
        }
        return true;
    }

    static boolean registeredAtHome(ServiceState ss) {
        if (ss == null || ss.getRoaming()) return false;
        return (ss.getVoiceRegState() == ServiceState.STATE_IN_SERVICE
                && validOperator(ss.getVoiceOperatorNumeric()))
                || (ss.getDataRegState() == ServiceState.STATE_IN_SERVICE
                && ss.getRilDataRadioTechnology() != ServiceState.RIL_RADIO_TECHNOLOGY_IWLAN
                && validOperator(ss.getDataOperatorNumeric()));
    }

    static boolean shouldUseAutomatic(String simOperator, String selectedOperator,
            ServiceState ss) {
        if (!validOperator(simOperator) || (ss != null && ss.getRoaming())) return false;
        // An OOS roaming=false is not evidence of being home. The only boot-time
        // exception is a saved/requested PLMN that exactly matches this SIM.
        return registeredAtHome(ss)
                || (validOperator(selectedOperator) && simOperator.equals(selectedOperator));
    }

    boolean enabled() {
        return SystemProperties.getBoolean(PROPERTY, false)
                && mPhone.getPhoneType() == TelephonyManager.PHONE_TYPE_GSM;
    }

    private ServiceState state() {
        ServiceStateTracker tracker = mPhone.getServiceStateTracker();
        // Do not use an IMS-merged Phone ServiceState (e.g. Wi-Fi calling).
        return tracker == null ? null : tracker.mSS;
    }

    private String simOperator() {
        IccRecords records = mPhone.getIccRecords();
        return records == null ? null : records.getOperatorNumeric();
    }

    private boolean safeToSelect() {
        return mCi.getRadioState().isOn()
                && mPhone.getServiceStateTracker() != null && !mPhone.isShuttingDown()
                && mPhone.getState() == PhoneConstants.State.IDLE
                && !mPhone.isInEmergencyCall() && !mPhone.isInEcm();
    }

    /** Intercept both saved boot selections and new manual selections at home. */
    boolean selectIfHome(String selectedOperator, Message response) {
        updateIdentity();
        if (!enabled() || !shouldUseAutomatic(simOperator(), selectedOperator, state())) {
            cancel("manual_selection_superseded_policy");
            return false;
        }
        if (!safeToSelect() || !SubscriptionManager.isValidSubscriptionId(mPhone.getSubId())) {
            failResponse(response, "selection_not_safe");
            return true;
        }
        start(response);
        return true;
    }

    /** Called after a complete cellular service-state poll, not per partial RIL response. */
    void onServiceState(ServiceState ss) {
        updateIdentity();
        if (!enabled() || !mCi.getRadioState().isOn() || (ss != null && ss.getRoaming())) {
            cancel("policy_disabled_radio_off_or_roaming");
            mAttempted = false;
            return;
        }
        if (!safeToSelect()) {
            cancel("selection_deferred_until_safe");
            mAttempted = false;
            return;
        }
        if (!registeredAtHome(ss)) return;
        if (!ss.getIsManualSelection()) {
            mAttempted = false;
        } else if (!mAttempted && safeToSelect() && validOperator(simOperator())
                && SubscriptionManager.isValidSubscriptionId(mPhone.getSubId())) {
            start(null);
        }
    }

    private void updateIdentity() {
        IccRecords records = mPhone.getIccRecords();
        int subId = mPhone.getSubId();
        if (records != mRecords || subId != mSubId) {
            cancel("subscription_changed");
            mRecords = records;
            mSubId = subId;
            mAttempted = false;
        }
    }

    private void start(Message response) {
        if (mPending != null) {
            if (response != null) {
                if (mPending.responses.size() < MAX_WAITERS) mPending.responses.add(response);
                else failResponse(response, "too_many_pending_requests");
            }
            return;
        }
        Request request = new Request();
        request.sequence = ++mSequence;
        request.subId = mPhone.getSubId();
        request.records = mPhone.getIccRecords();
        request.homeOperator = simOperator();
        if (response != null) request.responses.add(response);
        mPending = request;
        mAttempted = true;
        mOutcome = "checking";
        sendMessageDelayed(obtainMessage(TIMEOUT, request), DEADLINE_MS);
        mCi.getNetworkSelectionMode(obtainMessage(CHECK, request));
    }

    private boolean stillValid(Request request) {
        return enabled() && safeToSelect() && request.subId == mPhone.getSubId()
                && request.records == mPhone.getIccRecords()
                && request.homeOperator.equals(simOperator())
                && shouldUseAutomatic(request.homeOperator, request.homeOperator, state());
    }

    @Override
    public void handleMessage(Message message) {
        if (message.what == TIMEOUT) {
            if (mPending != null && message.obj == mPending) finish("timeout_outcome_unknown", false);
            return;
        }
        if (!(message.obj instanceof AsyncResult)) {
            if (mPending != null) finish("malformed_response", false);
            return;
        }
        AsyncResult result = (AsyncResult) message.obj;
        if (result.userObj != mPending || mPending == null) {
            Rlog.w(TAG, "Ignoring late response stage=" + message.what);
            return;
        }
        Request request = mPending;
        if (!stillValid(request)) {
            finish("context_changed", false);
        } else if (message.what == SET) {
            // A legacy registration failure does not prove the selection mode failed.
            // Independently read mode even on SET error; never claim LTE registration.
            request.setReportedError = result.exception != null;
            if (request.setReportedError) {
                Rlog.w(TAG, "sequence=" + request.sequence + " subId=" + request.subId
                        + " selection_error=" + result.exception + "; verifying actual mode");
            }
            mOutcome = "verifying";
            mCi.getNetworkSelectionMode(obtainMessage(VERIFY, request));
        } else if (result.exception != null) {
            finish("radio_error_stage_" + message.what + ":" + result.exception, false);
        } else if (message.what == CHECK || message.what == VERIFY) {
            if (!(result.result instanceof int[]) || ((int[]) result.result).length == 0) {
                finish("invalid_mode_response", false);
                return;
            }
            int mode = ((int[]) result.result)[0];
            if (mode == 0) {
                // Only a confirmed modem mode0 permits removal of the saved manual choice.
                finish(mPhone.clearSavedNetworkSelection()
                        ? (request.setReportedError ? "automatic_verified_after_set_error"
                                : "automatic_verified")
                        : "preference_commit_failed", true);
            } else if (mode == 1 && message.what == CHECK) {
                mOutcome = "setting";
                mCi.setNetworkSelectionModeAutomatic(obtainMessage(SET, request));
            } else {
                finish("automatic_not_verified_mode_" + mode, false);
            }
        } else {
            finish("unexpected_stage_" + message.what, false);
        }
    }

    private void cancel(String reason) {
        if (mPending != null) finish(reason, false);
    }

    private void finish(String outcome, boolean modemAutomatic) {
        Request request = mPending;
        removeMessages(TIMEOUT, request);
        mPending = null;
        mOutcome = outcome;
        boolean success = modemAutomatic && ("automatic_verified".equals(outcome)
                || "automatic_verified_after_set_error".equals(outcome));
        if (!success) {
            Rlog.w(TAG, "sequence=" + request.sequence + " subId=" + request.subId
                    + " outcome=" + outcome + " modemAutomatic=" + modemAutomatic);
        }
        for (Message response : request.responses) {
            AsyncResult.forMessage(response, null, success ? null
                    : new CommandException(CommandException.Error.GENERIC_FAILURE));
            response.sendToTarget();
        }
    }

    private void failResponse(Message response, String reason) {
        Rlog.w(TAG, "subId=" + mPhone.getSubId() + " outcome=" + reason);
        if (response != null) {
            AsyncResult.forMessage(response, null,
                    new CommandException(CommandException.Error.GENERIC_FAILURE));
            response.sendToTarget();
        }
    }

    void dump(PrintWriter pw) {
        pw.println(" HomeNetworkSelection: enabled=" + enabled() + " subId=" + mSubId
                + " sequence=" + mSequence + " pending=" + (mPending != null)
                + " attempted=" + mAttempted + " outcome=" + mOutcome);
    }
}
