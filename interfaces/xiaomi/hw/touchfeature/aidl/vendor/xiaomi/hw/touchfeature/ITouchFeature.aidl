/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

package vendor.xiaomi.hw.touchfeature;

import vendor.xiaomi.hw.touchfeature.ITouchFeatureCallback;

/*
 * The touch modes of Xiaomi's touch HAL. Modes are numbered as in the
 * xiaomi_touch driver (common_data_mode); touchId selects the touchscreen.
 */
@VintfStability
interface ITouchFeature {
    int getModeCurValue(int touchId, int mode);
    int getModeDefaultValue(int touchId, int mode);
    int getModeMaxValue(int touchId, int mode);
    int getModeMinValue(int touchId, int mode);
    int[] getModeValue(int touchId, int mode);
    String getTouchEvent();
    int modeReset(int touchId, int mode);
    int setModeLongValue(int touchId, int mode, int length, in int[] value);
    int setModeValue(int touchId, int mode, int value);
    String getModeCurValueString(int touchId, int mode);
    String getModeWhiteList(int touchId, in int[] modes);
    int registerCallback(int touchId, ITouchFeatureCallback callback, String name);
    int unregisterCallback(int touchId, ITouchFeatureCallback callback, String name);
    String setModePackageName(int touchId, int mode, String packageName);
}
