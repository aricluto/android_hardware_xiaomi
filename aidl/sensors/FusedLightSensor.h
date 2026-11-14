/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <android/hardware/sensors/2.1/types.h>

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace android {
namespace hardware {
namespace sensors {
namespace V2_1 {
namespace implementation {

/*
 * Some Xiaomi devices have a second ambient light sensor on the back, next to
 * the cameras. Stock combines both in its framework: the brighter of the two
 * readings drives the display brightness, so a shadow over the front sensor
 * doesn't dim the screen in a bright room.
 *
 * This exposes that combination as a virtual non-wake-up light sensor, which
 * the framework uses when config_displayLightSensorType selects it. Readings
 * of the back sensor are ignored while a flash LED is lit and for a while
 * after, because the flash shines straight into it.
 *
 * The front and back sensors stay available to other clients: requests for
 * them are tracked separately from the ones of the virtual sensor, and the
 * sensors are configured with the union of both.
 */
class FusedLightSensor {
  public:
    static constexpr const char* kStringType = "org.lineageos.sensor.fused_light";

    using ActivateFn = std::function<V1_0::Result(int32_t handle, bool enabled)>;
    using BatchFn =
            std::function<V1_0::Result(int32_t handle, int64_t periodNs, int64_t latencyNs)>;

    // Adds the virtual sensor to the list if both light sensors are in it. The
    // functions configure the sources in their sub-HAL.
    void addTo(std::map<int32_t, SensorInfo>& sensors, ActivateFn activate, BatchFn batch);

    // Whether requests for the handle have to go through this class.
    bool handles(int32_t handle) const {
        return mEnabled && (handle == kHandle || handle == mFront.handle || handle == mBack.handle);
    }
    bool isFused(int32_t handle) const { return mEnabled && handle == kHandle; }

    V1_0::Result activate(int32_t handle, bool enabled);
    V1_0::Result batch(int32_t handle, int64_t periodNs, int64_t latencyNs);
    Event flushCompleteEvent() const;

    // Consumes the events of the sources: drops the ones nobody asked for and
    // adds the virtual sensor's readings.
    std::vector<Event> process(const std::vector<Event>& events);

  private:
    // A sub-HAL index of 0xff, which no sub-HAL has
    static constexpr int32_t kHandle = static_cast<int32_t>(0xff000001);
    static constexpr int32_t kType = 0x10000 + 0x4c58;  // DEVICE_PRIVATE_BASE + "LX"
    static constexpr int64_t kFlashHoldOffNs = 1000000000LL;

    struct Source {
        int32_t handle = -1;
        bool requested = false;  // by a client of the source itself
        int64_t periodNs = INT64_MAX;
        int64_t latencyNs = 0;
        bool hasValue = false;
        float lux = 0;
    };

    V1_0::Result configure(const Source& source, bool requested);
    bool flashLit() const;
    std::optional<float> fusedLuxLocked(int64_t now);

    bool mEnabled = false;
    ActivateFn mActivateSource;
    BatchFn mBatchSource;
    std::vector<std::string> mFlashLeds;

    std::mutex mConfigLock;  // serializes requests
    bool mActive = false;
    int64_t mPeriodNs = INT64_MAX;

    std::mutex mLock;  // guards the state that process() uses
    Source mFront, mBack;
    bool mFusing = false;
    std::optional<float> mOutput;
    std::optional<int64_t> mFlashLitNs;  // last time a flash LED was seen lit
};

}  // namespace implementation
}  // namespace V2_1
}  // namespace sensors
}  // namespace hardware
}  // namespace android
