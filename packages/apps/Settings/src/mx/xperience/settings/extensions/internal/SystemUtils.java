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
package mx.xperience.settings.extensions.internal;

import android.content.ContentProvider;
import android.content.pm.PackageManager;
import android.content.pm.PackageManager.NameNotFoundException;

public class SystemUtils {

    public static boolean isSystemOrMotoApp(PackageManager pm, String pkgName) {
        /*try {
            return (pm.getApplicationInfo(pkgName, 0).flags & 1) > 0 || pm.checkSignatures("com.motorola.modservice", pkgName) == 0;
        } catch (NameNotFoundException e) {
        }*/
		return true;
    }

    public static boolean isCallingSystemOrMotoApp(ContentProvider provider) {
        try {
            PackageManager pm = provider.getContext().getPackageManager();
            String callingPackageName = provider.getCallingPackage();
            if (callingPackageName != null) {
                return isSystemOrMotoApp(pm, callingPackageName);
            }
        } catch (SecurityException e) {
        }
        return false;
    }
}