/*
 * Copyright 2018 The Android Open Source Project
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

#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>

// TODO(b/129481165): remove the #pragma below and fix conversion issues
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wconversion"
#include <ui/GraphicTypes.h>
#pragma clang diagnostic pop

#include "EventThread.h"
#include "LayerHistory.h"
#include "OneShotTimer.h"
#include "RefreshRateConfigs.h"
#include "SchedulerUtils.h"

namespace android {

using namespace std::chrono_literals;
using scheduler::LayerHistory;

class FenceTime;
class InjectVSyncSource;
class PredictedVsyncTracer;

namespace scheduler {
class VsyncController;
class VSyncDispatch;
class VSyncTracker;
} // namespace scheduler

namespace frametimeline {
class TokenManager;
} // namespace frametimeline

struct ISchedulerCallback {
    virtual void setVsyncEnabled(bool) = 0;
    virtual void changeRefreshRate(const scheduler::RefreshRateConfigs::RefreshRate&,
                                   scheduler::RefreshRateConfigEvent) = 0;
    virtual void repaintEverythingForHWC() = 0;
    virtual void kernelTimerChanged(bool expired) = 0;

protected:
    ~ISchedulerCallback() = default;
};

class Scheduler {
public:
    using RefreshRate = scheduler::RefreshRateConfigs::RefreshRate;
    using ConfigEvent = scheduler::RefreshRateConfigEvent;

    Scheduler(const scheduler::RefreshRateConfigs&, ISchedulerCallback&);
    ~Scheduler();

    using ConnectionHandle = scheduler::ConnectionHandle;
    ConnectionHandle createConnection(const char* connectionName, frametimeline::TokenManager*,
                                      std::chrono::nanoseconds workDuration,
                                      std::chrono::nanoseconds readyDuration,
                                      impl::EventThread::InterceptVSyncsCallback);

    sp<IDisplayEventConnection> createDisplayEventConnection(ConnectionHandle,
                                                             ISurfaceComposer::ConfigChanged);

    sp<EventThreadConnection> getEventConnection(ConnectionHandle);

    void onHotplugReceived(ConnectionHandle, PhysicalDisplayId, bool connected);
    void onPrimaryDisplayConfigChanged(ConnectionHandle, PhysicalDisplayId,
                                       HwcConfigIndexType configId, nsecs_t vsyncPeriod)
            EXCLUDES(mFeatureStateLock);
    void onNonPrimaryDisplayConfigChanged(ConnectionHandle, PhysicalDisplayId,
                                          HwcConfigIndexType configId, nsecs_t vsyncPeriod);
    void onScreenAcquired(ConnectionHandle);
    void onScreenReleased(ConnectionHandle);

    // Modifies work duration in the event thread.
    void setDuration(ConnectionHandle, std::chrono::nanoseconds workDuration,
                     std::chrono::nanoseconds readyDuration);

    void getDisplayStatInfo(DisplayStatInfo* stats, nsecs_t now);

    // Returns injector handle if injection has toggled, or an invalid handle otherwise.
    ConnectionHandle enableVSyncInjection(bool enable);
    // Returns false if injection is disabled.
    bool injectVSync(nsecs_t when, nsecs_t expectedVSyncTime, nsecs_t deadlineTimestamp);
    void enableHardwareVsync();
    void disableHardwareVsync(bool makeUnavailable);

    // Resyncs the scheduler to hardware vsync.
    // If makeAvailable is true, then hardware vsync will be turned on.
    // Otherwise, if hardware vsync is not already enabled then this method will
    // no-op.
    // The period is the vsync period from the current display configuration.
    void resyncToHardwareVsync(bool makeAvailable, nsecs_t period, bool force_resync = false);
    void resync();

    // Passes a vsync sample to VsyncController. periodFlushed will be true if
    // VsyncController detected that the vsync period changed, and false otherwise.
    void addResyncSample(nsecs_t timestamp, std::optional<nsecs_t> hwcVsyncPeriod,
                         bool* periodFlushed);
    void addPresentFence(const std::shared_ptr<FenceTime>&);
    void setIgnorePresentFences(bool ignore);

    // Layers are registered on creation, and unregistered when the weak reference expires.
    void registerLayer(Layer*);
    void recordLayerHistory(Layer*, nsecs_t presentTime, LayerHistory::LayerUpdateType updateType);
    void setConfigChangePending(bool pending);

    // Detects content using layer history, and selects a matching refresh rate.
    void chooseRefreshRateForContent();

    bool isIdleTimerEnabled() const { return mIdleTimer.has_value(); }
    void resetIdleTimer();

    // Function that resets the touch timer.
    void notifyTouchEvent();

    void setDisplayPowerState(bool normal);

    scheduler::VSyncDispatch& getVsyncDispatch() { return *mVsyncSchedule.dispatch; }

    void dump(std::string&) const;
    void dump(ConnectionHandle, std::string&) const;
    void dumpVsync(std::string&) const;

    // Get the appropriate refresh for current conditions.
    std::optional<HwcConfigIndexType> getPreferredConfigId();

    // Notifies the scheduler about a refresh rate timeline change.
    void onNewVsyncPeriodChangeTimeline(const hal::VsyncPeriodChangeTimeline& timeline);

    // Notifies the scheduler when the display was refreshed
    void onDisplayRefreshed(nsecs_t timestamp);

    // Notifies the scheduler when the display size has changed. Called from SF's main thread
    void onPrimaryDisplayAreaChanged(uint32_t displayArea);

    size_t getEventThreadConnectionCount(ConnectionHandle handle);

    std::unique_ptr<VSyncSource> makePrimaryDispSyncSource(const char* name,
                                                           std::chrono::nanoseconds workDuration,
                                                           std::chrono::nanoseconds readyDuration,
                                                           bool traceVsync = true);

private:
    friend class TestableScheduler;

    // In order to make sure that the features don't override themselves, we need a state machine
    // to keep track which feature requested the config change.
    enum class ContentDetectionState { Off, On };
    enum class TimerState { Reset, Expired };
    enum class TouchState { Inactive, Active };

    struct Options {
        // Whether to use idle timer callbacks that support the kernel timer.
        bool supportKernelTimer;
        // Whether to use content detection at all.
        bool useContentDetection;
        // Whether to use improved content detection.
        bool useContentDetectionV2;
    };

    struct VsyncSchedule {
        std::unique_ptr<scheduler::VsyncController> controller;
        std::unique_ptr<scheduler::VSyncTracker> tracker;
        std::unique_ptr<scheduler::VSyncDispatch> dispatch;
    };

    // Unlike the testing constructor, this creates the VsyncSchedule, LayerHistory, and timers.
    Scheduler(const scheduler::RefreshRateConfigs&, ISchedulerCallback&, Options);

    // Used by tests to inject mocks.
    Scheduler(VsyncSchedule, const scheduler::RefreshRateConfigs&, ISchedulerCallback&,
              std::unique_ptr<LayerHistory>, Options);

    static VsyncSchedule createVsyncSchedule(bool supportKernelIdleTimer);
    static std::unique_ptr<LayerHistory> createLayerHistory(const scheduler::RefreshRateConfigs&,
                                                            bool useContentDetectionV2);

    // Create a connection on the given EventThread.
    ConnectionHandle createConnection(std::unique_ptr<EventThread>);
    sp<EventThreadConnection> createConnectionInternal(EventThread*,
                                                       ISurfaceComposer::ConfigChanged);

    // Update feature state machine to given state when corresponding timer resets or expires.
    void kernelIdleTimerCallback(TimerState);
    void idleTimerCallback(TimerState);
    void touchTimerCallback(TimerState);
    void displayPowerTimerCallback(TimerState);

    // handles various timer features to change the refresh rate.
    template <class T>
    bool handleTimerStateChanged(T* currentState, T newState);

    void setVsyncPeriod(nsecs_t period, bool force_resync = false);

    // This function checks whether individual features that are affecting the refresh rate
    // selection were initialized, prioritizes them, and calculates the HwcConfigIndexType
    // for the suggested refresh rate.
    HwcConfigIndexType calculateRefreshRateConfigIndexType(
            scheduler::RefreshRateConfigs::GlobalSignals* consideredSignals = nullptr)
            REQUIRES(mFeatureStateLock);

    void dispatchCachedReportedConfig() REQUIRES(mFeatureStateLock);

    // Stores EventThread associated with a given VSyncSource, and an initial EventThreadConnection.
    struct Connection {
        sp<EventThreadConnection> connection;
        std::unique_ptr<EventThread> thread;
    };

    ConnectionHandle::Id mNextConnectionHandleId = 0;
    mutable std::mutex mConnectionsLock;
    std::unordered_map<ConnectionHandle, Connection> mConnections GUARDED_BY(mConnectionsLock);

    bool mInjectVSyncs = false;
    InjectVSyncSource* mVSyncInjector = nullptr;
    ConnectionHandle mInjectorConnectionHandle;

    std::mutex mHWVsyncLock;
    bool mPrimaryHWVsyncEnabled GUARDED_BY(mHWVsyncLock) = false;
    bool mHWVsyncAvailable GUARDED_BY(mHWVsyncLock) = false;

    std::atomic<nsecs_t> mLastResyncTime = 0;

    const Options mOptions;
    VsyncSchedule mVsyncSchedule;

    // Used to choose refresh rate if content detection is enabled.
    const std::unique_ptr<LayerHistory> mLayerHistory;

    // Timer that records time between requests for next vsync.
    std::optional<scheduler::OneShotTimer> mIdleTimer;
    // Timer used to monitor touch events.
    std::optional<scheduler::OneShotTimer> mTouchTimer;
    // Timer used to monitor display power mode.
    std::optional<scheduler::OneShotTimer> mDisplayPowerTimer;

    ISchedulerCallback& mSchedulerCallback;

    // In order to make sure that the features don't override themselves, we need a state machine
    // to keep track which feature requested the config change.
    std::mutex mFeatureStateLock;

    struct {
        ContentDetectionState contentDetectionV1 = ContentDetectionState::Off;
        TimerState idleTimer = TimerState::Reset;
        TouchState touch = TouchState::Inactive;
        TimerState displayPowerTimer = TimerState::Expired;

        std::optional<HwcConfigIndexType> configId;
        LayerHistory::Summary contentRequirements;

        bool isDisplayPowerStateNormal = true;

        // Used to cache the last parameters of onPrimaryDisplayConfigChanged
        struct ConfigChangedParams {
            ConnectionHandle handle;
            PhysicalDisplayId displayId;
            HwcConfigIndexType configId;
            nsecs_t vsyncPeriod;
        };

        std::optional<ConfigChangedParams> cachedConfigChangedParams;
    } mFeatures GUARDED_BY(mFeatureStateLock);

    const scheduler::RefreshRateConfigs& mRefreshRateConfigs;

    std::mutex mVsyncTimelineLock;
    std::optional<hal::VsyncPeriodChangeTimeline> mLastVsyncPeriodChangeTimeline
            GUARDED_BY(mVsyncTimelineLock);
    static constexpr std::chrono::nanoseconds MAX_VSYNC_APPLIED_TIME = 200ms;

    const std::unique_ptr<PredictedVsyncTracer> mPredictedVsyncTracer;
};

} // namespace android
