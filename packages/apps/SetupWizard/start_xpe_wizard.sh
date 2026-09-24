#!/bin/bash

adb root
wait ${!}
adb shell pm disable com.google.android.setupwizard || true
wait ${!}
adb shell pm disable com.android.provision || true
wait ${!}
adb shell am start mx.xperience.setupwizard/mx.xperience.setupwizard.SetupWizardTestActivity
