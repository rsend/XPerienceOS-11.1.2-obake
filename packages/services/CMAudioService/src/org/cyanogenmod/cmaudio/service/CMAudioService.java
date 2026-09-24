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
package org.cyanogenmod.cmaudio.service;


import android.app.Service;
import android.content.Intent;
import android.os.Handler;
import android.os.IBinder;
import android.os.Message;
import android.os.RemoteException;
import android.os.UserHandle;
import android.util.Log;

import java.util.ArrayList;
import java.util.List;

import cyanogenmod.media.AudioSessionInfo;
import cyanogenmod.media.CMAudioManager;
import cyanogenmod.media.ICMAudioService;
import cyanogenmod.platform.Manifest;

public class CMAudioService extends Service {

    private static final String TAG = "XPeAudioService";
    private static final boolean DEBUG = Log.isLoggable(TAG, Log.DEBUG);

    static {
        System.loadLibrary("cmaudio_jni");
        if (DEBUG) Log.d(TAG, "loaded jni lib");
    }

    public static final int MSG_BROADCAST_SESSION = 1;

    private static final int AUDIO_STATUS_OK = 0;

    //keep in sync with include/media/AudioPolicy.h
    private final static int AUDIO_OUTPUT_SESSION_EFFECTS_UPDATE = 10;

    private final Handler mHandler = new Handler(new Handler.Callback() {
        @Override
        public boolean handleMessage(Message msg) {
            if (DEBUG) Log.d(TAG, "handleMessage() called with: " + "msg = [" + msg + "]");
            switch (msg.what) {
                case MSG_BROADCAST_SESSION:
                    broadcastSessionChanged(msg.arg1 == 1, (AudioSessionInfo) msg.obj);
                    break;
            }
            return true;
        }
    });

    @Override
    public void onCreate() {
        super.onCreate();
        native_registerAudioSessionCallback(true);
    }

    @Override
    public void onDestroy() {
        native_registerAudioSessionCallback(false);
        super.onDestroy();
    }

    private final IBinder mBinder = new ICMAudioService.Stub() {

        @Override
        public List<AudioSessionInfo> listAudioSessions(int streamType) throws RemoteException {
            final ArrayList<AudioSessionInfo> sessions = new ArrayList<AudioSessionInfo>();

            int status = native_listAudioSessions(streamType, sessions);
            if (status != AUDIO_STATUS_OK) {
                Log.e(TAG, "Error retrieving audio sessions! status=" + status);
            }

            return sessions;
        }

    };

    private void broadcastSessionChanged(boolean added, AudioSessionInfo sessionInfo) {
        Intent i = new Intent(CMAudioManager.ACTION_AUDIO_SESSIONS_CHANGED);
        i.putExtra(CMAudioManager.EXTRA_SESSION_INFO, sessionInfo);
        i.putExtra(CMAudioManager.EXTRA_SESSION_ADDED, added);

        sendBroadcastToAll(i, Manifest.permission.OBSERVE_AUDIO_SESSIONS);
    }

    private void sendBroadcastToAll(Intent intent, String receiverPermission) {
        intent.addFlags(Intent.FLAG_RECEIVER_FOREGROUND);

        if (DEBUG) Log.d(TAG, "Sending broadcast: " + intent);

        sendBroadcastAsUser(intent, UserHandle.ALL, receiverPermission);
    }

    @Override
    public IBinder onBind(Intent intent) {
        return mBinder;
    }

    /*
     * Handles events from JNI
     */
    private void audioSessionCallbackFromNative(int event, AudioSessionInfo sessionInfo,
            boolean added) {

        switch (event) {
            case AUDIO_OUTPUT_SESSION_EFFECTS_UPDATE:
                mHandler.obtainMessage(MSG_BROADCAST_SESSION, added ? 1 : 0, 0, sessionInfo)
                        .sendToTarget();
                break;
            default:
                Log.e(TAG, "Unknown event " + event);
        }
    }

    private native void native_registerAudioSessionCallback(boolean enabled);

    private native int native_listAudioSessions(int stream, ArrayList<AudioSessionInfo> sessions);

}
