/*
   Copyright (c) 2013, The Linux Foundation. All rights reserved.

   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions are
   met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above
      copyright notice, this list of conditions and the following
      disclaimer in the documentation and/or other materials provided
      with the distribution.
    * Neither the name of The Linux Foundation nor the names of its
      contributors may be used to endorse or promote products derived
      from this software without specific prior written permission.

   THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED
   WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
   MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT
   ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
   BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
   CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
   SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
   BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
   WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
   OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
   IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdlib.h>

#include "vendor_init.h"
#include "property_service.h"
#include "log.h"
#include "util.h"

#define ARRAY_SIZE(x) (sizeof(x)/sizeof(x[0]))

static void set_cmdline_properties()
{
    size_t i;

    struct {
        const char *src_prop;
        const char *dest_prop;
        const char *def_val;
    } prop_map[] = {
        { "ro.boot.device", "ro.hw.device", "obake", },
        { "ro.boot.hwrev", "ro.hw.hwrev", "0x8300", },
        { "ro.boot.radio", "ro.hw.radio", "0x2", },
    };

    for (i = 0; i < ARRAY_SIZE(prop_map); i++) {
        std::string temp_var = property_get(prop_map[i].src_prop);
        property_set(prop_map[i].dest_prop,
                     temp_var.empty() ? prop_map[i].def_val : temp_var.c_str());
    }
}

static void gsm_properties()
{
    property_set("ro.telephony.default_network", "9");
    property_set("telephony.lteOnGsmDevice", "1");
}

static void verizon_properties()
{
    property_set("ro.telephony.default_cdma_sub", "0");
    property_set("ro.cdma.home.operator.numeric", "311480");
    property_set("ro.cdma.home.operator.alpha", "Verizon");

    property_set("persist.radio.0x9e_not_callname", "1");
    property_set("persist.ril.max.crit.qmi.fails", "4");

    property_set("ril.subscription.types", "NV,RUIM");
    property_set("ro.cdma.subscribe_on_ruim_ready", "true");
    property_set("ro.mot.ignore_csim_appid", "true");
    property_set("ro.ril.svdo", "true");
    property_set("ro.telephony.default_network", "10");
    property_set("telephony.lteOnCdmaDevice", "1");
    property_set("telephony.rilV7NeedCDMALTEPhone", "true");
    
    property_set("ro.cdma.data_retry_config", "max_retries=infinite,0,0,10000,10000,100000,10000,10000,10000,10000,140000,540000,960000");
    property_set("ro.gsm.data_retry_config", "default_randomization=2000,max_retries=infinite,1000,1000,80000,125000,485000,905000");
    property_set("ro.gsm.2nd_data_retry_config", "max_retries=1,15000");
    property_set("ro.com.google.clientidbase.ms", "android-verizon");
    property_set("ro.com.google.clientidbase.am", "android-verizon");
    property_set("ro.com.google.clientidbase.yt", "android-verizon");
}

void vendor_load_properties()
{
    std::string platform;
    std::string radio;
    std::string device;
    std::string carrier;
    std::string bootdevice;
    std::string devicename;

    platform = property_get("ro.board.platform");

    if (platform != ANDROID_TARGET) {
        return;
    }

    set_cmdline_properties();

    radio = property_get("ro.boot.radio");
    carrier = property_get("ro.boot.carrier");
    bootdevice = property_get("ro.boot.device");

    if (bootdevice == "obakem") {
        /* xt1030 - obakem */
        property_set("ro.product.device", "obakem");
        property_set("ro.build.product", "obakem");
        property_set("ro.product.model", "DROID Mini");
        property_set("ro.audio.init", "obakem");
    } else if (bootdevice == "obake") {
        /* xt1080 - obake */
        property_set("ro.product.device", "obake");
        property_set("ro.build.product", "obake");
        property_set("ro.product.model", "DROID Ultra");
    } else if (bootdevice == "obake-maxx") {
        /* xt1080m - obake-maxx */
        property_set("ro.product.device", "obake-maxx");
        property_set("ro.build.product", "obake-maxx");
        property_set("ro.product.model", "DROID MAXX");
        property_set("ro.audio.init", "obake-maxx");
    }

    property_set("ro.build.description",
                 "obake_verizon-user 4.4.4 SU6-7.2 3 release-keys");

    property_set("ro.build.fingerprint",
                 "motorola/obake_verizon/obake:4.4.4/SU6-7.2/3:user/release-keys");

    if (carrier == "vzw") {
        verizon_properties();
    } else {
        gsm_properties();
    }

    devicename = property_get("ro.product.device");

    INFO("Found device: %s radio id: %s carrier: %s Setting build properties for %s device\n",
         bootdevice.c_str(),
         radio.c_str(),
         carrier.c_str(),
         devicename.c_str());

}
