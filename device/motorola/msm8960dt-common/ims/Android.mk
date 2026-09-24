# Copyright (C) 2017 The LineageOS Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

# Obake's legacy RIL sends IMS network-state notifications without a payload.
# The API-24 Clark client handles that legacy form by querying registration
# state, while the Shamu client incorrectly treats it as an error. Most of the
# startup protocol is compatible, but Clark also sends request IDs absent from
# stock obake (including generic IMS config requests), and its TTY request uses
# a different ID. ims_compatd will adapt those differences after pass-through
# validation. Until then, configs/init/ims.rc keeps the direct socket alias.
LOCAL_MODULE := ims
LOCAL_MODULE_OWNER := motorola
LOCAL_PREBUILT_MODULE_FILE := vendor/motorola/clark/proprietary/app/ims/ims.apk
LOCAL_CERTIFICATE := platform
LOCAL_MODULE_TAGS := optional
LOCAL_MODULE_CLASS := APPS
LOCAL_DEX_PREOPT := false
LOCAL_MODULE_SUFFIX := $(COMMON_ANDROID_PACKAGE_SUFFIX)

include $(BUILD_PREBUILT)
