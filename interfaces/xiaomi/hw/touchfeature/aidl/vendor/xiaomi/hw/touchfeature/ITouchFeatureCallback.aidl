/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

package vendor.xiaomi.hw.touchfeature;

@VintfStability
interface ITouchFeatureCallback {
    int callback(int touchId, int event, int value, in int[] values, in String[] strings);
}
