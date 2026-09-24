LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)
LOCAL_MODULE := ims_lte_guard
LOCAL_MODULE_TAGS := optional
LOCAL_SRC_FILES := ims_lte_guard.cpp
LOCAL_SHARED_LIBRARIES := libcutils libdl liblog
LOCAL_CPPFLAGS := -std=gnu++11 -Wall -Wextra -Werror
include $(BUILD_EXECUTABLE)

include $(CLEAR_VARS)

LOCAL_MODULE := ims_compatd
LOCAL_MODULE_TAGS := optional
LOCAL_SRC_FILES := \
    ims_compatd.cpp \
    ims_frame_codec.cpp \
    ims_protocol_adapter.cpp \
    ims_call_list_bridge.cpp \
    ims_volte_bridge.cpp \
    ims_volte_worker.cpp \
    ims_service_state.cpp \
    ims_service_control.cpp \
    ims_service_publisher.cpp
LOCAL_SHARED_LIBRARIES := libcutils liblog
LOCAL_CPPFLAGS := -std=gnu++11 -Wall -Wextra -Werror

include $(BUILD_EXECUTABLE)

include $(CLEAR_VARS)

LOCAL_MODULE := ims_dpl_bootstrapd
LOCAL_MODULE_TAGS := optional
LOCAL_SRC_FILES := \
    ims_dpl_probe.cpp \
    ims_service_state.cpp \
    ims_service_control.cpp \
    ims_service_receiver.cpp \
    ims_service_lifecycle.cpp \
    ims_long_timer_bridge.cpp \
    ims_initial_registration.cpp
LOCAL_SHARED_LIBRARIES := libcutils libdl liblog
LOCAL_CPPFLAGS := -DIMS_DPL_SERVICE -std=gnu++11 -Wall -Wextra -Werror

include $(BUILD_EXECUTABLE)

include $(CLEAR_VARS)

LOCAL_MODULE := libims_dpl_trace
LOCAL_MODULE_TAGS := optional
LOCAL_SRC_FILES := ims_dpl_trace.cpp
LOCAL_SHARED_LIBRARIES := libdl
LOCAL_CPPFLAGS := -std=gnu++11 -Wall -Wextra -Werror

include $(BUILD_SHARED_LIBRARY)

include $(CLEAR_VARS)

LOCAL_MODULE := ims_settings_probe
LOCAL_MODULE_TAGS := optional
LOCAL_SRC_FILES := ims_settings_probe.cpp
LOCAL_SHARED_LIBRARIES := libdl liblog
LOCAL_CPPFLAGS := -std=gnu++11 -Wall -Wextra -Werror

include $(BUILD_EXECUTABLE)
