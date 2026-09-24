LOG_TO_ANDROID_LOGCAT := true

LOCAL_PATH:= $(call my-dir)
include $(CLEAR_VARS)

LOCAL_SRC_FILES:= tinyxml2.cpp

LOCAL_MODULE:=libtinyxml2
LOCAL_MODULE_TAGS := optional

ifeq ($(LOG_TO_ANDROID_LOGCAT),true)
LOCAL_CFLAGS+= -DDEBUG -DANDROID_NDK
endif

include $(BUILD_STATIC_LIBRARY)

include $(CLEAR_VARS)

LOCAL_SRC_FILES:= tinyxml2.cpp

LOCAL_MODULE:=libtinyxml2
LOCAL_MODULE_TAGS := optional

include $(BUILD_HOST_STATIC_LIBRARY)

# shared library
include $(CLEAR_VARS)

LOCAL_MODULE:=libtinyxml2
LOCAL_MODULE_TAGS := optional

LOCAL_WHOLE_STATIC_LIBRARIES := libtinyxml2
ifeq ($(LOG_TO_ANDROID_LOGCAT),true)
LOCAL_SHARED_LIBRARIES+= libcutils
endif

include $(BUILD_SHARED_LIBRARY)
