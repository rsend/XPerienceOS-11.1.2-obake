/*
**
** Copyright 2016, The CyanogenMod Project
**
** Licensed under the Apache License, Version 2.0 (the "License");
** you may not use this file except in compliance with the License.
** You may obtain a copy of the License at
**
**     http://www.apache.org/licenses/LICENSE-2.0
**
** Unless required by applicable law or agreed to in writing, software
** distributed under the License is distributed on an "AS IS" BASIS,
** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
** See the License for the specific language governing permissions and
** limitations under the License.
*/

//#define LOG_NDEBUG 0

#define LOG_TAG "CMAudioService-JNI"

#include <utils/Log.h>

#include <JNIHelp.h>
#include <jni.h>
#include "core_jni_helpers.h"
#include "android_media_AudioErrors.h"
#include "android_runtime/AndroidRuntime.h"


#include <media/AudioSystem.h>
#include <media/AudioSession.h>

#include <system/audio.h>
#include <utils/threads.h>

// ----------------------------------------------------------------------------

namespace android {

static jclass gArrayListClass;
static struct {
    jmethodID    add;
    jmethodID    toArray;
} gArrayListMethods;

static struct {
    jmethodID postAudioSessionEventFromNative;
} gAudioSessionEventHandlerMethods;

static jclass gAudioSessionInfoClass;
static jmethodID gAudioSessionInfoCstor;

static jobject gThiz;

static Mutex gCallbackLock;

// ----------------------------------------------------------------------------

static void
org_cyanogenmod_cmaudio_service_CMAudioService_session_info_callback(int event,
        sp<AudioSessionInfo>& info, bool added)
{
    AutoMutex _l(gCallbackLock);

    if (gThiz == NULL) {
        ALOGE("gThiz iz null");
       return;
    }

    JNIEnv *env = AndroidRuntime::getJNIEnv();

    jobject jSession = env->NewObject(gAudioSessionInfoClass, gAudioSessionInfoCstor,
            info->mSessionId, info->mStream, info->mFlags, info->mChannelMask, info->mUid);

    env->CallVoidMethod(gThiz,
            gAudioSessionEventHandlerMethods.postAudioSessionEventFromNative,
            event, jSession, added);

    env->DeleteLocalRef(jSession);
}

JNIEXPORT void JNICALL
org_cyanogenmod_cmaudio_service_registerAudioSessionCallback(JNIEnv *env, jobject thiz,
        jboolean enabled)
{
    ALOGV("registerAudioSessionCallback %d", enabled);

    if (enabled) {
        if (gThiz == NULL) {
            gThiz = env->NewGlobalRef(thiz);
        }

    } else {
        if (gThiz != NULL) {
            env->DeleteGlobalRef(gThiz);
            gThiz = NULL;
        }
    }

    AudioSystem::setAudioSessionCallback(enabled ?
            org_cyanogenmod_cmaudio_service_CMAudioService_session_info_callback : NULL);
}

JNIEXPORT jint JNICALL org_cyanogenmod_cmaudio_service_CMAudioService_listAudioSessions(JNIEnv *env,
        jobject thiz, jint streams, jobject jSessions)
{
    ALOGV("listAudioSessions");

    if (jSessions == NULL) {
        ALOGE("listAudioSessions NULL arraylist");
        return (jint)AUDIO_JAVA_BAD_VALUE;
    }
    if (!env->IsInstanceOf(jSessions, gArrayListClass)) {
        ALOGE("listAudioSessions not an arraylist");
        return (jint)AUDIO_JAVA_BAD_VALUE;
    }

    status_t status;
    Vector< sp<AudioSessionInfo>> sessions;

    status = AudioSystem::listAudioSessions((audio_stream_type_t)streams, sessions);
    if (status != NO_ERROR) {
        ALOGE("AudioSystem::listAudioSessions error %d", status);
    } else {
        ALOGV("AudioSystem::listAudioSessions count=%zu", sessions.size());
    }

    jint jStatus = nativeToJavaStatus(status);
    if (jStatus != AUDIO_JAVA_SUCCESS) {
        goto exit;
    }

    for (size_t i = 0; i < sessions.size(); i++) {
        const sp<AudioSessionInfo>& s = sessions.itemAt(i);

        jobject jSession = env->NewObject(gAudioSessionInfoClass, gAudioSessionInfoCstor,
                s->mSessionId, s->mStream, s->mFlags, s->mChannelMask, s->mUid);

        if (jSession == NULL) {
            jStatus = (jint)AUDIO_JAVA_ERROR;
            goto exit;
        }

        env->CallBooleanMethod(jSessions, gArrayListMethods.add, jSession);
        env->DeleteLocalRef(jSession);
    }

exit:
    return jStatus;
}

} /* namespace android */

// ----------------------------------------------------------------------------

static const char* const kClassPathName = "org/cyanogenmod/cmaudio/service/CMAudioService";

static JNINativeMethod gMethods[] = {
     {"native_listAudioSessions", "(ILjava/util/ArrayList;)I",
            (void *)android::org_cyanogenmod_cmaudio_service_CMAudioService_listAudioSessions},
     {"native_registerAudioSessionCallback", "(Z)V",
            (void *)android::org_cyanogenmod_cmaudio_service_registerAudioSessionCallback},
};

static int registerNativeMethods(JNIEnv* env, const char* className,
        JNINativeMethod* gMethods, int numMethods) {
    jclass clazz;
    clazz = env->FindClass(className);
    if (clazz == NULL) {
        ALOGE("Native registration unable to find class '%s'", className);
        return JNI_FALSE;
    }
    if (env->RegisterNatives(clazz, gMethods, numMethods) < 0) {
        ALOGE("RegisterNatives failed for '%s'", className);
        return JNI_FALSE;
    }
    return JNI_TRUE;
}


jint JNI_OnLoad(JavaVM* vm, void* reserved)
{
    ALOGV("OnLoad");
    JNIEnv* env = 0;

    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
        ALOGE("Error: GetEnv failed in JNI_OnLoad");
        return -1;
    }
    jclass serviceClass = env->FindClass(kClassPathName);
    if (!serviceClass) {
        ALOGE("Failed to get class reference");
        return -1;
    }

    if (!registerNativeMethods(env, kClassPathName, gMethods, NELEM(gMethods))) {
        ALOGE("Error: could not register native methods cmaudio service");
        return -1;
    }

    jclass arrayListClass = android::FindClassOrDie(env, "java/util/ArrayList");
    android::gArrayListClass = android::MakeGlobalRefOrDie(env, arrayListClass);
    android::gArrayListMethods.add = android::GetMethodIDOrDie(env, arrayListClass, "add",
            "(Ljava/lang/Object;)Z");
    android::gArrayListMethods.toArray = android::GetMethodIDOrDie(env, arrayListClass,
            "toArray", "()[Ljava/lang/Object;");

    jclass audioSessionInfoClass = android::FindClassOrDie(env,
            "cyanogenmod/media/AudioSessionInfo");
    android::gAudioSessionInfoClass = android::MakeGlobalRefOrDie(env, audioSessionInfoClass);
    android::gAudioSessionInfoCstor = android::GetMethodIDOrDie(env, audioSessionInfoClass,
            "<init>", "(IIIII)V");

    android::gAudioSessionEventHandlerMethods.postAudioSessionEventFromNative =
            android::GetMethodIDOrDie(env, env->FindClass(kClassPathName),
            "audioSessionCallbackFromNative", "(ILcyanogenmod/media/AudioSessionInfo;Z)V");

    ALOGV("loaded successfully!!!");

    return JNI_VERSION_1_6;
}

