/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "FusedLightSensor"

#include "FusedLightSensor.h"

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/strings.h>

#include <dirent.h>

#include <algorithm>
#include <memory>

namespace android {
namespace hardware {
namespace sensors {
namespace V2_1 {
namespace implementation {

using ::android::base::ReadFileToString;
using ::android::base::StartsWith;
using ::android::base::Trim;
using V1_0::MetaDataEventType;
using V1_0::Result;
using V1_0::SensorFlagBits;

namespace {

constexpr const char* kBackLightType = "xiaomi.sensor.back_lux";
constexpr const char* kLedsDir = "/sys/class/leds";

bool isWakeUp(const SensorInfo& sensor) {
    return sensor.flags & static_cast<uint32_t>(SensorFlagBits::WAKE_UP);
}

}  // namespace

void FusedLightSensor::addTo(std::map<int32_t, SensorInfo>& sensors, ActivateFn activate,
                             BatchFn batch) {
    const SensorInfo* front = nullptr;
    const SensorInfo* back = nullptr;
    for (const auto& [handle, sensor] : sensors) {
        if (isWakeUp(sensor)) continue;
        if (!front && sensor.type == SensorType::LIGHT) front = &sensor;
        if (!back && sensor.typeAsString == kBackLightType) back = &sensor;
    }
    if (!front || !back) return;

    SensorInfo fused = *front;
    fused.sensorHandle = kHandle;
    fused.name = "Fused Ambient Light Sensor";
    fused.vendor = "The LineageOS Project";
    fused.version = 1;
    fused.type = static_cast<SensorType>(kType);
    fused.typeAsString = kStringType;
    fused.maxRange = std::max(front->maxRange, back->maxRange);
    fused.power = front->power + back->power;
    fused.minDelay = std::max(front->minDelay, back->minDelay);
    fused.fifoReservedEventCount = 0;
    fused.fifoMaxEventCount = 0;
    fused.flags &= ~static_cast<uint32_t>(SensorFlagBits::MASK_DIRECT_REPORT |
                                          SensorFlagBits::MASK_DIRECT_CHANNEL);

    LOG(INFO) << "Fusing " << front->name << " and " << back->name;

    mFront.handle = front->sensorHandle;
    mBack.handle = back->sensorHandle;
    mActivateSource = std::move(activate);
    mBatchSource = std::move(batch);
    sensors[kHandle] = fused;

    if (std::unique_ptr<DIR, decltype(&closedir)> dir(opendir(kLedsDir), closedir); dir) {
        while (struct dirent* entry = readdir(dir.get())) {
            std::string name = entry->d_name;
            if (StartsWith(name, "led:flash_") || StartsWith(name, "led:torch_")) {
                mFlashLeds.push_back(std::string(kLedsDir) + "/" + name + "/brightness");
            }
        }
    }
    if (mFlashLeds.empty()) {
        LOG(WARNING) << "No flash LEDs found, the back sensor will see the flash";
    }

    mEnabled = true;
}

// Configures a source for the union of its own request and the virtual sensor's
Result FusedLightSensor::configure(const Source& source, bool requested) {
    if (!requested && !mActive) return mActivateSource(source.handle, false);

    int64_t period = std::min(requested ? source.periodNs : INT64_MAX,
                              mActive ? mPeriodNs : INT64_MAX);
    if (period != INT64_MAX) {
        Result result = mBatchSource(source.handle, period, mActive ? 0 : source.latencyNs);
        if (result != Result::OK) return result;
    }
    return mActivateSource(source.handle, true);
}

Result FusedLightSensor::activate(int32_t handle, bool enabled) {
    std::lock_guard<std::mutex> configLock(mConfigLock);

    if (handle == kHandle) {
        mActive = enabled;
        {
            std::lock_guard<std::mutex> lock(mLock);
            mFusing = enabled;
            mOutput.reset();
        }
        Result front = configure(mFront, mFront.requested);
        Result back = configure(mBack, mBack.requested);
        return front != Result::OK ? front : back;
    }

    Source& source = handle == mFront.handle ? mFront : mBack;
    {
        std::lock_guard<std::mutex> lock(mLock);
        source.requested = enabled;
    }
    return configure(source, enabled);
}

Result FusedLightSensor::batch(int32_t handle, int64_t periodNs, int64_t latencyNs) {
    std::lock_guard<std::mutex> configLock(mConfigLock);

    if (handle == kHandle) {
        mPeriodNs = periodNs;
        if (!mActive) return Result::OK;
        Result front = configure(mFront, mFront.requested);
        Result back = configure(mBack, mBack.requested);
        return front != Result::OK ? front : back;
    }

    // Clients batch before activating, so this applies whether or not the
    // source is requested yet
    Source& source = handle == mFront.handle ? mFront : mBack;
    source.periodNs = periodNs;
    source.latencyNs = latencyNs;
    int64_t period = mActive ? std::min(periodNs, mPeriodNs) : periodNs;
    return mBatchSource(handle, period, mActive ? 0 : latencyNs);
}

Event FusedLightSensor::flushCompleteEvent() const {
    Event event = {};
    event.sensorHandle = kHandle;
    event.sensorType = SensorType::META_DATA;
    event.u.meta.what = MetaDataEventType::META_DATA_FLUSH_COMPLETE;
    return event;
}

bool FusedLightSensor::flashLit() const {
    for (const auto& path : mFlashLeds) {
        std::string value;
        if (ReadFileToString(path, &value) && Trim(value) != "0") return true;
    }
    return false;
}

std::optional<float> FusedLightSensor::fusedLuxLocked(int64_t now) {
    if (flashLit()) mFlashLitNs = now;
    bool useBack = mBack.hasValue && (!mFlashLitNs || now - *mFlashLitNs > kFlashHoldOffNs);

    if (mFront.hasValue && useBack) return std::max(mFront.lux, mBack.lux);
    if (mFront.hasValue) return mFront.lux;
    if (useBack) return mBack.lux;
    return std::nullopt;
}

std::vector<Event> FusedLightSensor::process(const std::vector<Event>& events) {
    std::lock_guard<std::mutex> lock(mLock);
    std::vector<Event> out;
    out.reserve(events.size() + 1);
    int64_t timestamp = 0;

    for (const Event& event : events) {
        Source* source = event.sensorHandle == mFront.handle  ? &mFront
                         : event.sensorHandle == mBack.handle ? &mBack
                                                              : nullptr;
        if (!source || event.sensorType == SensorType::META_DATA) {
            out.push_back(event);
            continue;
        }

        // Both sensors report lux in the first value
        source->lux = event.u.data[0];
        source->hasValue = true;
        timestamp = event.timestamp;
        if (source->requested) out.push_back(event);
    }

    if (mFusing && timestamp) {
        std::optional<float> lux = fusedLuxLocked(timestamp);
        if (lux && lux != mOutput) {
            Event event = {};
            event.timestamp = timestamp;
            event.sensorHandle = kHandle;
            event.sensorType = static_cast<SensorType>(kType);
            event.u.data[0] = *lux;
            out.push_back(event);
            mOutput = lux;
        }
    }

    return out;
}

}  // namespace implementation
}  // namespace V2_1
}  // namespace sensors
}  // namespace hardware
}  // namespace android
