/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// There are three kinds of samples done by the profiler.
//
// - A "periodic" sample is the most complex kind. It is done in response to a
//   timer while the profiler is active. It involves writing a stack trace plus
//   a variety of other values (memory measurements, responsiveness
//   measurements, markers, etc.) into the main ProfileBuffer. The sampling is
//   done from off-thread, and so SuspendAndSampleAndResumeThread() is used to
//   get the register values.
//
// - A "synchronous" sample is a simpler kind. It is done in response to an API
//   call (profiler_get_backtrace()). It involves writing a stack trace and
//   little else into a temporary ProfileBuffer, and wrapping that up in a
//   ProfilerBacktrace that can be subsequently used in a marker. The sampling
//   is done on-thread, and so Registers::SyncPopulate() is used to get the
//   register values.
//
// - A "backtrace" sample is the simplest kind. It is done in response to an
//   API call (profiler_suspend_and_sample_thread()). It involves getting a
//   stack trace via a ProfilerStackCollector; it does not write to a
//   ProfileBuffer. The sampling is done from off-thread, and so uses
//   SuspendAndSampleAndResumeThread() to get the register values.

#include "BaseProfiler.h"

#ifdef MOZ_BASE_PROFILER

#  include "platform.h"

#  include "PageInformation.h"
#  include "ProfiledThreadData.h"
#  include "ProfilerBacktrace.h"
#  include "ProfileBuffer.h"
#  include "BaseProfilerMarkerPayload.h"
#  include "RegisteredThread.h"
#  include "BaseProfilerSharedLibraries.h"
#  include "ThreadInfo.h"
#  include "VTuneProfiler.h"

// #include "memory_hooks.h"
#  include "mozilla/ArrayUtils.h"
#  include "mozilla/Atomics.h"
#  include "mozilla/AutoProfilerLabel.h"
#  include "mozilla/BaseProfilerDetail.h"
#  include "mozilla/Printf.h"
#  include "mozilla/Services.h"
#  include "mozilla/StackWalk.h"
#  include "mozilla/StaticPtr.h"
#  include "mozilla/ThreadLocal.h"
#  include "mozilla/TimeStamp.h"
#  include "mozilla/Tuple.h"
#  include "mozilla/UniquePtr.h"
#  include "mozilla/Vector.h"
#  include "prdtoa.h"
#  include "prtime.h"

#  include <algorithm>
#  include <errno.h>
#  include <fstream>
#  include <ostream>
#  include <sstream>

// Win32 builds always have frame pointers, so FramePointerStackWalk() always
// works.
#  if defined(GP_PLAT_x86_windows)
#    define HAVE_NATIVE_UNWIND
#    define USE_FRAME_POINTER_STACK_WALK
#  endif

// Win64 builds always omit frame pointers, so we use the slower
// MozStackWalk(), which works in that case.
#  if defined(GP_PLAT_amd64_windows)
#    define HAVE_NATIVE_UNWIND
#    define USE_MOZ_STACK_WALK
#  endif

// AArch64 Win64 doesn't seem to use frame pointers, so we use the slower
// MozStackWalk().
#  if defined(GP_PLAT_arm64_windows)
#    define HAVE_NATIVE_UNWIND
#    define USE_MOZ_STACK_WALK
#  endif

// Mac builds only have frame pointers when MOZ_PROFILING is specified, so
// FramePointerStackWalk() only works in that case. We don't use MozStackWalk()
// on Mac.
#  if defined(GP_OS_darwin) && defined(MOZ_PROFILING)
#    define HAVE_NATIVE_UNWIND
#    define USE_FRAME_POINTER_STACK_WALK
#  endif

// Android builds use the ARM Exception Handling ABI to unwind.
#  if defined(GP_PLAT_arm_linux) || defined(GP_PLAT_arm_android)
#    define HAVE_NATIVE_UNWIND
#    define USE_EHABI_STACKWALK
#    include "EHABIStackWalk.h"
#  endif

// Linux builds use LUL, which uses DWARF info to unwind stacks.
#  if defined(GP_PLAT_amd64_linux) || defined(GP_PLAT_x86_linux) ||     \
      defined(GP_PLAT_amd64_android) || defined(GP_PLAT_x86_android) || \
      defined(GP_PLAT_mips64_linux) || defined(GP_PLAT_arm64_linux) ||  \
      defined(GP_PLAT_arm64_android)
#    define HAVE_NATIVE_UNWIND
#    define USE_LUL_STACKWALK
#    include "lul/LulMain.h"
#    include "lul/platform-linux-lul.h"

// On linux we use LUL for periodic samples and synchronous samples, but we use
// FramePointerStackWalk for backtrace samples when MOZ_PROFILING is enabled.
// (See the comment at the top of the file for a definition of
// periodic/synchronous/backtrace.).
//
// FramePointerStackWalk can produce incomplete stacks when the current entry is
// in a shared library without framepointers, however LUL can take a long time
// to initialize, which is undesirable for consumers of
// profiler_suspend_and_sample_thread like the Background Hang Reporter.
#    if defined(MOZ_PROFILING)
#      define USE_FRAME_POINTER_STACK_WALK
#    endif
#  endif

// We can only stackwalk without expensive initialization on platforms which
// support FramePointerStackWalk or MozStackWalk. LUL Stackwalking requires
// initializing LUL, and EHABIStackWalk requires initializing EHABI, both of
// which can be expensive.
#  if defined(USE_FRAME_POINTER_STACK_WALK) || defined(USE_MOZ_STACK_WALK)
#    define HAVE_FASTINIT_NATIVE_UNWIND
#  endif

#  ifdef MOZ_VALGRIND
#    include <valgrind/memcheck.h>
#  else
#    define VALGRIND_MAKE_MEM_DEFINED(_addr, _len) ((void)0)
#  endif

#  if defined(GP_OS_linux) || defined(GP_OS_android)
#    include <ucontext.h>
#  endif

namespace mozilla {
namespace baseprofiler {

using detail::RacyFeatures;

bool BaseProfilerLogTest(int aLevelToTest) {
  static const int maxLevel =
      getenv("MOZ_BASE_PROFILER_VERBOSE_LOGGING")
          ? 5
          : getenv("MOZ_BASE_PROFILER_DEBUG_LOGGING")
                ? 4
                : getenv("MOZ_BASE_PROFILER_LOGGING") ? 3 : 0;
  return aLevelToTest <= maxLevel;
}

// Return all features that are available on this platform.
static uint32_t AvailableFeatures() {
  uint32_t features = 0;

#  define ADD_FEATURE(n_, str_, Name_, desc_) \
    ProfilerFeature::Set##Name_(features);

  // Add all the possible features.
  BASE_PROFILER_FOR_EACH_FEATURE(ADD_FEATURE)

#  undef ADD_FEATURE

  // Now remove features not supported on this platform/configuration.
  ProfilerFeature::ClearJava(features);
  ProfilerFeature::ClearJS(features);
  ProfilerFeature::ClearScreenshots(features);
#  if !defined(HAVE_NATIVE_UNWIND)
  ProfilerFeature::ClearStackWalk(features);
#  endif
  ProfilerFeature::ClearTaskTracer(features);
  ProfilerFeature::ClearTrackOptimizations(features);
  ProfilerFeature::ClearJSTracer(features);

  return features;
}

// Default features common to all contexts (even if not available).
static uint32_t DefaultFeatures() {
  return ProfilerFeature::Java | ProfilerFeature::JS | ProfilerFeature::Leaf |
         ProfilerFeature::StackWalk | ProfilerFeature::Threads;
}

// Extra default features when MOZ_BASE_PROFILER_STARTUP is set (even if not
// available).
static uint32_t StartupExtraDefaultFeatures() {
  // Enable mainthreadio by default for startup profiles as startup is heavy on
  // I/O operations, and main thread I/O is really important to see there.
  return ProfilerFeature::MainThreadIO;
}

// The auto-lock/unlock mutex that guards accesses to CorePS and ActivePS.
// Use `PSAutoLock lock;` to take the lock until the end of the enclosing block.
// External profilers may use this same lock for their own data, but as the lock
// is non-recursive, *only* `f(PSLockRef, ...)` functions below should be
// called, to avoid double-locking.
class MOZ_RAII PSAutoLock {
 public:
  PSAutoLock() { gPSMutex.Lock(); }

  ~PSAutoLock() { gPSMutex.Unlock(); }

  PSAutoLock(const PSAutoLock&) = delete;
  void operator=(const PSAutoLock&) = delete;

 private:
  static detail::BaseProfilerMutex gPSMutex;
};

detail::BaseProfilerMutex PSAutoLock::gPSMutex;

// Only functions that take a PSLockRef arg can access CorePS's and ActivePS's
// fields.
typedef const PSAutoLock& PSLockRef;

#  define PS_GET(type_, name_)      \
    static type_ name_(PSLockRef) { \
      MOZ_ASSERT(sInstance);        \
      return sInstance->m##name_;   \
    }

#  define PS_GET_LOCKLESS(type_, name_) \
    static type_ name_() {              \
      MOZ_ASSERT(sInstance);            \
      return sInstance->m##name_;       \
    }

#  define PS_GET_AND_SET(type_, name_)                  \
    PS_GET(type_, name_)                                \
    static void Set##name_(PSLockRef, type_ a##name_) { \
      MOZ_ASSERT(sInstance);                            \
      sInstance->m##name_ = a##name_;                   \
    }

// All functions in this file can run on multiple threads unless they have an
// NS_IsMainThread() assertion.

// This class contains the profiler's core global state, i.e. that which is
// valid even when the profiler is not active. Most profile operations can't do
// anything useful when this class is not instantiated, so we release-assert
// its non-nullness in all such operations.
//
// Accesses to CorePS are guarded by gPSMutex. Getters and setters take a
// PSAutoLock reference as an argument as proof that the gPSMutex is currently
// locked. This makes it clear when gPSMutex is locked and helps avoid
// accidental unlocked accesses to global state. There are ways to circumvent
// this mechanism, but please don't do so without *very* good reason and a
// detailed explanation.
//
// The exceptions to this rule:
//
// - mProcessStartTime, because it's immutable;
//
// - each thread's RacyRegisteredThread object is accessible without locking via
//   TLSRegisteredThread::RacyRegisteredThread().
class CorePS {
 private:
  CorePS()
      : mMainThreadId(profiler_current_thread_id()),
        mProcessStartTime(TimeStamp::ProcessCreation()),
        // This needs its own mutex, because it is used concurrently from
        // functions guarded by gPSMutex as well as others without safety (e.g.,
        // profiler_add_marker). It is *not* used inside the critical section of
        // the sampler, because mutexes cannot be used there.
        mCoreBlocksRingBuffer(BlocksRingBuffer::ThreadSafety::WithMutex)
#  ifdef USE_LUL_STACKWALK
        ,
        mLul(nullptr)
#  endif
  {
  }

  ~CorePS() {}

 public:
  static void Create(PSLockRef aLock) {
    MOZ_ASSERT(!sInstance);
    sInstance = new CorePS();
  }

  static void Destroy(PSLockRef aLock) {
    MOZ_ASSERT(sInstance);
    delete sInstance;
    sInstance = nullptr;
  }

  // Unlike ActivePS::Exists(), CorePS::Exists() can be called without gPSMutex
  // being locked. This is because CorePS is instantiated so early on the main
  // thread that we don't have to worry about it being racy.
  static bool Exists() { return !!sInstance; }

  static bool IsMainThread() {
    MOZ_ASSERT(sInstance);
    return profiler_current_thread_id() == sInstance->mMainThreadId;
  }

  static void AddSizeOf(PSLockRef, MallocSizeOf aMallocSizeOf,
                        size_t& aProfSize, size_t& aLulSize) {
    MOZ_ASSERT(sInstance);

    aProfSize += aMallocSizeOf(sInstance);

    for (auto& registeredThread : sInstance->mRegisteredThreads) {
      aProfSize += registeredThread->SizeOfIncludingThis(aMallocSizeOf);
    }

    for (auto& registeredPage : sInstance->mRegisteredPages) {
      aProfSize += registeredPage->SizeOfIncludingThis(aMallocSizeOf);
    }

    // Measurement of the following things may be added later if DMD finds it
    // is worthwhile:
    // - CorePS::mRegisteredThreads itself (its elements' children are
    // measured above)
    // - CorePS::mRegisteredPages itself (its elements' children are
    // measured above)
    // - CorePS::mInterposeObserver

#  if defined(USE_LUL_STACKWALK)
    if (sInstance->mLul) {
      aLulSize += sInstance->mLul->SizeOfIncludingThis(aMallocSizeOf);
    }
#  endif
  }

  // No PSLockRef is needed for this field because it's immutable.
  PS_GET_LOCKLESS(TimeStamp, ProcessStartTime)

  // No PSLockRef is needed for this field because it's thread-safe.
  PS_GET_LOCKLESS(BlocksRingBuffer&, CoreBlocksRingBuffer)

  PS_GET(const Vector<UniquePtr<RegisteredThread>>&, RegisteredThreads)

  static void AppendRegisteredThread(
      PSLockRef, UniquePtr<RegisteredThread>&& aRegisteredThread) {
    MOZ_ASSERT(sInstance);
    MOZ_RELEASE_ASSERT(
        sInstance->mRegisteredThreads.append(std::move(aRegisteredThread)));
  }

  static void RemoveRegisteredThread(PSLockRef,
                                     RegisteredThread* aRegisteredThread) {
    MOZ_ASSERT(sInstance);
    // Remove aRegisteredThread from mRegisteredThreads.
    for (UniquePtr<RegisteredThread>& rt : sInstance->mRegisteredThreads) {
      if (rt.get() == aRegisteredThread) {
        sInstance->mRegisteredThreads.erase(&rt);
        return;
      }
    }
  }

  PS_GET(Vector<RefPtr<PageInformation>>&, RegisteredPages)

  static void AppendRegisteredPage(PSLockRef,
                                   RefPtr<PageInformation>&& aRegisteredPage) {
    MOZ_ASSERT(sInstance);
    struct RegisteredPageComparator {
      PageInformation* aA;
      bool operator()(PageInformation* aB) const { return aA->Equals(aB); }
    };

    auto foundPageIter = std::find_if(
        sInstance->mRegisteredPages.begin(), sInstance->mRegisteredPages.end(),
        RegisteredPageComparator{aRegisteredPage.get()});

    if (foundPageIter != sInstance->mRegisteredPages.end()) {
      if ((*foundPageIter)->Url() == "about:blank") {
        // When a BrowsingContext is loaded, the first url loaded in it will be
        // about:blank, and if the principal matches, the first document loaded
        // in it will share an inner window. That's why we should delete the
        // intermittent about:blank if they share the inner window.
        sInstance->mRegisteredPages.erase(foundPageIter);
      } else {
        // Do not register the same page again.
        return;
      }
    }
    MOZ_RELEASE_ASSERT(
        sInstance->mRegisteredPages.append(std::move(aRegisteredPage)));
  }

  static void RemoveRegisteredPage(PSLockRef,
                                   uint64_t aRegisteredInnerWindowID) {
    MOZ_ASSERT(sInstance);
    // Remove RegisteredPage from mRegisteredPages by given inner window ID.
    sInstance->mRegisteredPages.eraseIf([&](const RefPtr<PageInformation>& rd) {
      return rd->InnerWindowID() == aRegisteredInnerWindowID;
    });
  }

  static void ClearRegisteredPages(PSLockRef) {
    MOZ_ASSERT(sInstance);
    sInstance->mRegisteredPages.clear();
  }

  PS_GET(const Vector<BaseProfilerCount*>&, Counters)

  static void AppendCounter(PSLockRef, BaseProfilerCount* aCounter) {
    MOZ_ASSERT(sInstance);
    // we don't own the counter; they may be stored in static objects
    MOZ_RELEASE_ASSERT(sInstance->mCounters.append(aCounter));
  }

  static void RemoveCounter(PSLockRef, BaseProfilerCount* aCounter) {
    // we may be called to remove a counter after the profiler is stopped or
    // late in shutdown.
    if (sInstance) {
      auto* counter = std::find(sInstance->mCounters.begin(),
                                sInstance->mCounters.end(), aCounter);
      MOZ_RELEASE_ASSERT(counter != sInstance->mCounters.end());
      sInstance->mCounters.erase(counter);
    }
  }

#  ifdef USE_LUL_STACKWALK
  static lul::LUL* Lul(PSLockRef) {
    MOZ_ASSERT(sInstance);
    return sInstance->mLul.get();
  }
  static void SetLul(PSLockRef, UniquePtr<lul::LUL> aLul) {
    MOZ_ASSERT(sInstance);
    sInstance->mLul = std::move(aLul);
  }
#  endif

  PS_GET_AND_SET(const std::string&, ProcessName)

 private:
  // The singleton instance
  static CorePS* sInstance;

  // ID of the main thread (assuming CorePS was started on the main thread).
  const int mMainThreadId;

  // The time that the process started.
  const TimeStamp mProcessStartTime;

  // The thread-safe blocks-oriented ring buffer into which all profiling data
  // is recorded.
  // ActivePS controls the lifetime of the underlying contents buffer: When
  // ActivePS does not exist, mCoreBlocksRingBuffer is empty and rejects all
  // reads&writes; see ActivePS for further details.
  // Note: This needs to live here outside of ActivePS, because some producers
  // are indirectly controlled (e.g., by atomic flags) and therefore may still
  // attempt to write some data shortly after ActivePS has shutdown and deleted
  // the underlying buffer in memory.
  BlocksRingBuffer mCoreBlocksRingBuffer;

  // Info on all the registered threads.
  // ThreadIds in mRegisteredThreads are unique.
  Vector<UniquePtr<RegisteredThread>> mRegisteredThreads;

  // Info on all the registered pages.
  // InnerWindowIDs in mRegisteredPages are unique.
  Vector<RefPtr<PageInformation>> mRegisteredPages;

  // Non-owning pointers to all active counters
  Vector<BaseProfilerCount*> mCounters;

#  ifdef USE_LUL_STACKWALK
  // LUL's state. Null prior to the first activation, non-null thereafter.
  UniquePtr<lul::LUL> mLul;
#  endif

  // Process name, provided by child process initialization code.
  std::string mProcessName;
};

CorePS* CorePS::sInstance = nullptr;

class SamplerThread;

static SamplerThread* NewSamplerThread(PSLockRef aLock, uint32_t aGeneration,
                                       double aInterval);

struct LiveProfiledThreadData {
  RegisteredThread* mRegisteredThread;
  UniquePtr<ProfiledThreadData> mProfiledThreadData;
};

// This class contains the profiler's global state that is valid only when the
// profiler is active. When not instantiated, the profiler is inactive.
//
// Accesses to ActivePS are guarded by gPSMutex, in much the same fashion as
// CorePS.
//
class ActivePS {
 private:
  static uint32_t AdjustFeatures(uint32_t aFeatures, uint32_t aFilterCount) {
    // Filter out any features unavailable in this platform/configuration.
    aFeatures &= AvailableFeatures();

    // Always enable ProfilerFeature::Threads if we have a filter, because
    // users sometimes ask to filter by a list of threads but forget to
    // explicitly specify ProfilerFeature::Threads.
    if (aFilterCount > 0) {
      aFeatures |= ProfilerFeature::Threads;
    }

    return aFeatures;
  }

  ActivePS(PSLockRef aLock, PowerOfTwo32 aCapacity, double aInterval,
           uint32_t aFeatures, const char** aFilters, uint32_t aFilterCount,
           const Maybe<double>& aDuration)
      : mGeneration(sNextGeneration++),
        mCapacity(aCapacity),
        mDuration(aDuration),
        mInterval(aInterval),
        mFeatures(AdjustFeatures(aFeatures, aFilterCount)),
        // 8 bytes per entry.
        mProfileBuffer(CorePS::CoreBlocksRingBuffer(),
                       PowerOfTwo32(aCapacity.Value() * 8)),
        // The new sampler thread doesn't start sampling immediately because the
        // main loop within Run() is blocked until this function's caller
        // unlocks gPSMutex.
        mSamplerThread(NewSamplerThread(aLock, mGeneration, aInterval))
#  undef HAS_FEATURE
        ,
        mIsPaused(false)
#  if defined(GP_OS_linux)
        ,
        mWasPaused(false)
#  endif
  {
    // Deep copy aFilters.
    MOZ_ALWAYS_TRUE(mFilters.resize(aFilterCount));
    for (uint32_t i = 0; i < aFilterCount; ++i) {
      mFilters[i] = aFilters[i];
    }
  }

  ~ActivePS() {}

  bool ThreadSelected(const char* aThreadName) {
    if (mFilters.empty()) {
      return true;
    }

    std::string name = aThreadName;
    std::transform(name.begin(), name.end(), name.begin(), ::tolower);

    for (uint32_t i = 0; i < mFilters.length(); ++i) {
      std::string filter = mFilters[i];

      if (filter == "*") {
        return true;
      }

      std::transform(filter.begin(), filter.end(), filter.begin(), ::tolower);

      // Crude, non UTF-8 compatible, case insensitive substring search
      if (name.find(filter) != std::string::npos) {
        return true;
      }

      // If the filter starts with pid:, check for a pid match
      if (filter.find("pid:") == 0) {
        std::string mypid = std::to_string(profiler_current_process_id());
        if (filter.compare(4, std::string::npos, mypid) == 0) {
          return true;
        }
      }
    }

    return false;
  }

 public:
  static void Create(PSLockRef aLock, PowerOfTwo32 aCapacity, double aInterval,
                     uint32_t aFeatures, const char** aFilters,
                     uint32_t aFilterCount, const Maybe<double>& aDuration) {
    MOZ_ASSERT(!sInstance);
    sInstance = new ActivePS(aLock, aCapacity, aInterval, aFeatures, aFilters,
                             aFilterCount, aDuration);
  }

  static MOZ_MUST_USE SamplerThread* Destroy(PSLockRef aLock) {
    MOZ_ASSERT(sInstance);
    auto samplerThread = sInstance->mSamplerThread;
    delete sInstance;
    sInstance = nullptr;

    return samplerThread;
  }

  static bool Exists(PSLockRef) { return !!sInstance; }

  static bool Equals(PSLockRef, PowerOfTwo32 aCapacity,
                     const Maybe<double>& aDuration, double aInterval,
                     uint32_t aFeatures, const char** aFilters,
                     uint32_t aFilterCount) {
    MOZ_ASSERT(sInstance);
    if (sInstance->mCapacity != aCapacity ||
        sInstance->mDuration != aDuration ||
        sInstance->mInterval != aInterval ||
        sInstance->mFeatures != aFeatures ||
        sInstance->mFilters.length() != aFilterCount) {
      return false;
    }

    for (uint32_t i = 0; i < sInstance->mFilters.length(); ++i) {
      if (strcmp(sInstance->mFilters[i].c_str(), aFilters[i]) != 0) {
        return false;
      }
    }
    return true;
  }

  static size_t SizeOf(PSLockRef, MallocSizeOf aMallocSizeOf) {
    MOZ_ASSERT(sInstance);

    size_t n = aMallocSizeOf(sInstance);

    n += sInstance->mProfileBuffer.SizeOfExcludingThis(aMallocSizeOf);

    // Measurement of the following members may be added later if DMD finds it
    // is worthwhile:
    // - mLiveProfiledThreads (both the array itself, and the contents)
    // - mDeadProfiledThreads (both the array itself, and the contents)
    //

    return n;
  }

  static bool ShouldProfileThread(PSLockRef aLock, ThreadInfo* aInfo) {
    MOZ_ASSERT(sInstance);
    return ((aInfo->IsMainThread() || FeatureThreads(aLock)) &&
            sInstance->ThreadSelected(aInfo->Name()));
  }

  PS_GET(uint32_t, Generation)

  PS_GET(PowerOfTwo32, Capacity)

  PS_GET(Maybe<double>, Duration)

  PS_GET(double, Interval)

  PS_GET(uint32_t, Features)

#  define PS_GET_FEATURE(n_, str_, Name_, desc_)                \
    static bool Feature##Name_(PSLockRef) {                     \
      MOZ_ASSERT(sInstance);                                    \
      return ProfilerFeature::Has##Name_(sInstance->mFeatures); \
    }

  BASE_PROFILER_FOR_EACH_FEATURE(PS_GET_FEATURE)

#  undef PS_GET_FEATURE

  PS_GET(const Vector<std::string>&, Filters)

  static ProfileBuffer& Buffer(PSLockRef) {
    MOZ_ASSERT(sInstance);
    return sInstance->mProfileBuffer;
  }

  static const Vector<LiveProfiledThreadData>& LiveProfiledThreads(PSLockRef) {
    MOZ_ASSERT(sInstance);
    return sInstance->mLiveProfiledThreads;
  }

  // Returns an array containing (RegisteredThread*, ProfiledThreadData*) pairs
  // for all threads that should be included in a profile, both for threads
  // that are still registered, and for threads that have been unregistered but
  // still have data in the buffer.
  // For threads that have already been unregistered, the RegisteredThread
  // pointer will be null.
  // The returned array is sorted by thread register time.
  // Do not hold on to the return value across thread registration or profiler
  // restarts.
  static Vector<Pair<RegisteredThread*, ProfiledThreadData*>> ProfiledThreads(
      PSLockRef) {
    MOZ_ASSERT(sInstance);
    Vector<Pair<RegisteredThread*, ProfiledThreadData*>> array;
    MOZ_RELEASE_ASSERT(
        array.initCapacity(sInstance->mLiveProfiledThreads.length() +
                           sInstance->mDeadProfiledThreads.length()));
    for (auto& t : sInstance->mLiveProfiledThreads) {
      MOZ_RELEASE_ASSERT(array.append(
          MakePair(t.mRegisteredThread, t.mProfiledThreadData.get())));
    }
    for (auto& t : sInstance->mDeadProfiledThreads) {
      MOZ_RELEASE_ASSERT(
          array.append(MakePair((RegisteredThread*)nullptr, t.get())));
    }

    std::sort(array.begin(), array.end(),
              [](const Pair<RegisteredThread*, ProfiledThreadData*>& a,
                 const Pair<RegisteredThread*, ProfiledThreadData*>& b) {
                return a.second()->Info()->RegisterTime() <
                       b.second()->Info()->RegisterTime();
              });
    return array;
  }

  static Vector<RefPtr<PageInformation>> ProfiledPages(PSLockRef aLock) {
    MOZ_ASSERT(sInstance);
    Vector<RefPtr<PageInformation>> array;
    for (auto& d : CorePS::RegisteredPages(aLock)) {
      MOZ_RELEASE_ASSERT(array.append(d));
    }
    for (auto& d : sInstance->mDeadProfiledPages) {
      MOZ_RELEASE_ASSERT(array.append(d));
    }
    // We don't need to sort the pages like threads since we won't show them
    // as a list.
    return array;
  }

  // Do a linear search through mLiveProfiledThreads to find the
  // ProfiledThreadData object for a RegisteredThread.
  static ProfiledThreadData* GetProfiledThreadData(
      PSLockRef, RegisteredThread* aRegisteredThread) {
    MOZ_ASSERT(sInstance);
    for (const LiveProfiledThreadData& thread :
         sInstance->mLiveProfiledThreads) {
      if (thread.mRegisteredThread == aRegisteredThread) {
        return thread.mProfiledThreadData.get();
      }
    }
    return nullptr;
  }

  static ProfiledThreadData* AddLiveProfiledThread(
      PSLockRef, RegisteredThread* aRegisteredThread,
      UniquePtr<ProfiledThreadData>&& aProfiledThreadData) {
    MOZ_ASSERT(sInstance);
    MOZ_RELEASE_ASSERT(
        sInstance->mLiveProfiledThreads.append(LiveProfiledThreadData{
            aRegisteredThread, std::move(aProfiledThreadData)}));

    // Return a weak pointer to the ProfiledThreadData object.
    return sInstance->mLiveProfiledThreads.back().mProfiledThreadData.get();
  }

  static void UnregisterThread(PSLockRef aLockRef,
                               RegisteredThread* aRegisteredThread) {
    MOZ_ASSERT(sInstance);

    DiscardExpiredDeadProfiledThreads(aLockRef);

    // Find the right entry in the mLiveProfiledThreads array and remove the
    // element, moving the ProfiledThreadData object for the thread into the
    // mDeadProfiledThreads array.
    // The thread's RegisteredThread object gets destroyed here.
    for (size_t i = 0; i < sInstance->mLiveProfiledThreads.length(); i++) {
      LiveProfiledThreadData& thread = sInstance->mLiveProfiledThreads[i];
      if (thread.mRegisteredThread == aRegisteredThread) {
        thread.mProfiledThreadData->NotifyUnregistered(
            sInstance->mProfileBuffer.BufferRangeEnd());
        MOZ_RELEASE_ASSERT(sInstance->mDeadProfiledThreads.append(
            std::move(thread.mProfiledThreadData)));
        sInstance->mLiveProfiledThreads.erase(
            &sInstance->mLiveProfiledThreads[i]);
        return;
      }
    }
  }

  PS_GET_AND_SET(bool, IsPaused)

#  if defined(GP_OS_linux)
  PS_GET_AND_SET(bool, WasPaused)
#  endif

  static void DiscardExpiredDeadProfiledThreads(PSLockRef) {
    MOZ_ASSERT(sInstance);
    uint64_t bufferRangeStart = sInstance->mProfileBuffer.BufferRangeStart();
    // Discard any dead threads that were unregistered before bufferRangeStart.
    sInstance->mDeadProfiledThreads.eraseIf(
        [bufferRangeStart](
            const UniquePtr<ProfiledThreadData>& aProfiledThreadData) {
          Maybe<uint64_t> bufferPosition =
              aProfiledThreadData->BufferPositionWhenUnregistered();
          MOZ_RELEASE_ASSERT(bufferPosition,
                             "should have unregistered this thread");
          return *bufferPosition < bufferRangeStart;
        });
  }

  static void UnregisterPage(PSLockRef aLock,
                             uint64_t aRegisteredInnerWindowID) {
    MOZ_ASSERT(sInstance);
    auto& registeredPages = CorePS::RegisteredPages(aLock);
    for (size_t i = 0; i < registeredPages.length(); i++) {
      RefPtr<PageInformation>& page = registeredPages[i];
      if (page->InnerWindowID() == aRegisteredInnerWindowID) {
        page->NotifyUnregistered(sInstance->mProfileBuffer.BufferRangeEnd());
        MOZ_RELEASE_ASSERT(
            sInstance->mDeadProfiledPages.append(std::move(page)));
        registeredPages.erase(&registeredPages[i--]);
      }
    }
  }

  static void DiscardExpiredPages(PSLockRef) {
    MOZ_ASSERT(sInstance);
    uint64_t bufferRangeStart = sInstance->mProfileBuffer.BufferRangeStart();
    // Discard any dead pages that were unregistered before
    // bufferRangeStart.
    sInstance->mDeadProfiledPages.eraseIf(
        [bufferRangeStart](const RefPtr<PageInformation>& aProfiledPage) {
          Maybe<uint64_t> bufferPosition =
              aProfiledPage->BufferPositionWhenUnregistered();
          MOZ_RELEASE_ASSERT(bufferPosition,
                             "should have unregistered this page");
          return *bufferPosition < bufferRangeStart;
        });
  }

  static void ClearUnregisteredPages(PSLockRef) {
    MOZ_ASSERT(sInstance);
    sInstance->mDeadProfiledPages.clear();
  }

  static void ClearExpiredExitProfiles(PSLockRef) {
    MOZ_ASSERT(sInstance);
    uint64_t bufferRangeStart = sInstance->mProfileBuffer.BufferRangeStart();
    // Discard exit profiles that were gathered before our buffer RangeStart.
    sInstance->mExitProfiles.eraseIf(
        [bufferRangeStart](const ExitProfile& aExitProfile) {
          return aExitProfile.mBufferPositionAtGatherTime < bufferRangeStart;
        });
  }

  static void AddExitProfile(PSLockRef aLock, const std::string& aExitProfile) {
    MOZ_ASSERT(sInstance);

    ClearExpiredExitProfiles(aLock);

    MOZ_RELEASE_ASSERT(sInstance->mExitProfiles.append(
        ExitProfile{aExitProfile, sInstance->mProfileBuffer.BufferRangeEnd()}));
  }

  static Vector<std::string> MoveExitProfiles(PSLockRef aLock) {
    MOZ_ASSERT(sInstance);

    ClearExpiredExitProfiles(aLock);

    Vector<std::string> profiles;
    MOZ_RELEASE_ASSERT(
        profiles.initCapacity(sInstance->mExitProfiles.length()));
    for (auto& profile : sInstance->mExitProfiles) {
      MOZ_RELEASE_ASSERT(profiles.append(std::move(profile.mJSON)));
    }
    sInstance->mExitProfiles.clear();
    return profiles;
  }

 private:
  // The singleton instance.
  static ActivePS* sInstance;

  // We need to track activity generations. If we didn't we could have the
  // following scenario.
  //
  // - profiler_stop() locks gPSMutex, de-instantiates ActivePS, unlocks
  //   gPSMutex, deletes the SamplerThread (which does a join).
  //
  // - profiler_start() runs on a different thread, locks gPSMutex,
  //   re-instantiates ActivePS, unlocks gPSMutex -- all before the join
  //   completes.
  //
  // - SamplerThread::Run() locks gPSMutex, sees that ActivePS is instantiated,
  //   and continues as if the start/stop pair didn't occur. Also
  //   profiler_stop() is stuck, unable to finish.
  //
  // By checking ActivePS *and* the generation, we can avoid this scenario.
  // sNextGeneration is used to track the next generation number; it is static
  // because it must persist across different ActivePS instantiations.
  const uint32_t mGeneration;
  static uint32_t sNextGeneration;

  // The maximum number of 8-byte entries in mProfileBuffer.
  const PowerOfTwo32 mCapacity;

  // The maximum duration of entries in mProfileBuffer, in seconds.
  const Maybe<double> mDuration;

  // The interval between samples, measured in milliseconds.
  const double mInterval;

  // The profile features that are enabled.
  const uint32_t mFeatures;

  // Substrings of names of threads we want to profile.
  Vector<std::string> mFilters;

  // The buffer into which all samples are recorded.
  ProfileBuffer mProfileBuffer;

  // ProfiledThreadData objects for any threads that were profiled at any point
  // during this run of the profiler:
  //  - mLiveProfiledThreads contains all threads that are still registered, and
  //  - mDeadProfiledThreads contains all threads that have already been
  //    unregistered but for which there is still data in the profile buffer.
  Vector<LiveProfiledThreadData> mLiveProfiledThreads;
  Vector<UniquePtr<ProfiledThreadData>> mDeadProfiledThreads;

  // Info on all the dead pages.
  // Registered pages are being moved to this array after unregistration.
  // We are keeping them in case we need them in the profile data.
  // We are removing them when we ensure that we won't need them anymore.
  Vector<RefPtr<PageInformation>> mDeadProfiledPages;

  // The current sampler thread. This class is not responsible for destroying
  // the SamplerThread object; the Destroy() method returns it so the caller
  // can destroy it.
  SamplerThread* const mSamplerThread;

  // Is the profiler paused?
  bool mIsPaused;

#  if defined(GP_OS_linux)
  // Used to record whether the profiler was paused just before forking. False
  // at all times except just before/after forking.
  bool mWasPaused;
#  endif

  struct ExitProfile {
    std::string mJSON;
    uint64_t mBufferPositionAtGatherTime;
  };
  Vector<ExitProfile> mExitProfiles;
};

ActivePS* ActivePS::sInstance = nullptr;
uint32_t ActivePS::sNextGeneration = 0;

#  undef PS_GET
#  undef PS_GET_LOCKLESS
#  undef PS_GET_AND_SET

Atomic<uint32_t, MemoryOrdering::Relaxed> RacyFeatures::sActiveAndFeatures(0);

/* static */
void RacyFeatures::SetActive(uint32_t aFeatures) {
  sActiveAndFeatures = Active | aFeatures;
}

/* static */
void RacyFeatures::SetInactive() { sActiveAndFeatures = 0; }

/* static */
bool RacyFeatures::IsActive() { return uint32_t(sActiveAndFeatures) & Active; }

/* static */
void RacyFeatures::SetPaused() { sActiveAndFeatures |= Paused; }

/* static */
void RacyFeatures::SetUnpaused() { sActiveAndFeatures &= ~Paused; }

/* static */
bool RacyFeatures::IsActiveWithFeature(uint32_t aFeature) {
  uint32_t af = sActiveAndFeatures;  // copy it first
  return (af & Active) && (af & aFeature);
}

/* static */
bool RacyFeatures::IsActiveWithoutPrivacy() {
  uint32_t af = sActiveAndFeatures;  // copy it first
  return (af & Active) && !(af & ProfilerFeature::Privacy);
}

/* static */
bool RacyFeatures::IsActiveAndUnpausedWithoutPrivacy() {
  uint32_t af = sActiveAndFeatures;  // copy it first
  return (af & Active) && !(af & (Paused | ProfilerFeature::Privacy));
}

// Each live thread has a RegisteredThread, and we store a reference to it in
// TLS. This class encapsulates that TLS.
class TLSRegisteredThread {
 public:
  static bool Init(PSLockRef) {
    bool ok1 = sRegisteredThread.init();
    bool ok2 = AutoProfilerLabel::sProfilingStack.init();
    return ok1 && ok2;
  }

  // Get the entire RegisteredThread. Accesses are guarded by gPSMutex.
  static class RegisteredThread* RegisteredThread(PSLockRef) {
    return sRegisteredThread.get();
  }

  // Get only the RacyRegisteredThread. Accesses are not guarded by gPSMutex.
  static class RacyRegisteredThread* RacyRegisteredThread() {
    class RegisteredThread* registeredThread = sRegisteredThread.get();
    return registeredThread ? &registeredThread->RacyRegisteredThread()
                            : nullptr;
  }

  // Get only the ProfilingStack. Accesses are not guarded by gPSMutex.
  // RacyRegisteredThread() can also be used to get the ProfilingStack, but that
  // is marginally slower because it requires an extra pointer indirection.
  static ProfilingStack* Stack() {
    return AutoProfilerLabel::sProfilingStack.get();
  }

  static void SetRegisteredThread(PSLockRef,
                                  class RegisteredThread* aRegisteredThread) {
    sRegisteredThread.set(aRegisteredThread);
    AutoProfilerLabel::sProfilingStack.set(
        aRegisteredThread
            ? &aRegisteredThread->RacyRegisteredThread().ProfilingStack()
            : nullptr);
  }

 private:
  // This is a non-owning reference to the RegisteredThread;
  // CorePS::mRegisteredThreads is the owning reference. On thread
  // deregistration, this reference is cleared and the RegisteredThread is
  // destroyed.
  static MOZ_THREAD_LOCAL(class RegisteredThread*) sRegisteredThread;
};

MOZ_THREAD_LOCAL(RegisteredThread*) TLSRegisteredThread::sRegisteredThread;

/* static */
ProfilingStack* AutoProfilerLabel::GetProfilingStack() {
  return sProfilingStack.get();
}

// Although you can access a thread's ProfilingStack via
// TLSRegisteredThread::sRegisteredThread, we also have a second TLS pointer
// directly to the ProfilingStack. Here's why.
//
// - We need to be able to push to and pop from the ProfilingStack in
//   AutoProfilerLabel.
//
// - The class functions are hot and must be defined in BaseProfiler.h so they
//   can be inlined.
//
// - We don't want to expose TLSRegisteredThread (and RegisteredThread) in
//   BaseProfiler.h.
//
// This second pointer isn't ideal, but does provide a way to satisfy those
// constraints. TLSRegisteredThread is responsible for updating it.
MOZ_THREAD_LOCAL(ProfilingStack*) AutoProfilerLabel::sProfilingStack;

// The name of the main thread.
static const char* const kMainThreadName = "GeckoMain";

////////////////////////////////////////////////////////////////////////
// BEGIN sampling/unwinding code

// The registers used for stack unwinding and a few other sampling purposes.
// The ctor does nothing; users are responsible for filling in the fields.
class Registers {
 public:
  Registers() : mPC{nullptr}, mSP{nullptr}, mFP{nullptr}, mLR{nullptr} {}

#  if defined(HAVE_NATIVE_UNWIND)
  // Fills in mPC, mSP, mFP, mLR, and mContext for a synchronous sample.
  void SyncPopulate();
#  endif

  void Clear() { memset(this, 0, sizeof(*this)); }

  // These fields are filled in by
  // Sampler::SuspendAndSampleAndResumeThread() for periodic and backtrace
  // samples, and by SyncPopulate() for synchronous samples.
  Address mPC;  // Instruction pointer.
  Address mSP;  // Stack pointer.
  Address mFP;  // Frame pointer.
  Address mLR;  // ARM link register.
#  if defined(GP_OS_linux) || defined(GP_OS_android)
  // This contains all the registers, which means it duplicates the four fields
  // above. This is ok.
  ucontext_t* mContext;  // The context from the signal handler.
#  endif
};

// Setting MAX_NATIVE_FRAMES too high risks the unwinder wasting a lot of time
// looping on corrupted stacks.
static const size_t MAX_NATIVE_FRAMES = 1024;

struct NativeStack {
  void* mPCs[MAX_NATIVE_FRAMES];
  void* mSPs[MAX_NATIVE_FRAMES];
  size_t mCount;  // Number of frames filled.

  NativeStack() : mPCs(), mSPs(), mCount(0) {}
};

// Merges the profiling stack and native stack, outputting the details to
// aCollector.
static void MergeStacks(uint32_t aFeatures, bool aIsSynchronous,
                        const RegisteredThread& aRegisteredThread,
                        const Registers& aRegs, const NativeStack& aNativeStack,
                        ProfilerStackCollector& aCollector) {
  // WARNING: this function runs within the profiler's "critical section".
  // WARNING: this function might be called while the profiler is inactive, and
  //          cannot rely on ActivePS.

  const ProfilingStack& profilingStack =
      aRegisteredThread.RacyRegisteredThread().ProfilingStack();
  const ProfilingStackFrame* profilingStackFrames = profilingStack.frames;
  uint32_t profilingStackFrameCount = profilingStack.stackSize();

  Maybe<uint64_t> samplePosInBuffer;
  if (!aIsSynchronous) {
    // aCollector.SamplePositionInBuffer() will return Nothing() when
    // profiler_suspend_and_sample_thread is called from the background hang
    // reporter.
    samplePosInBuffer = aCollector.SamplePositionInBuffer();
  }
  // While the profiling stack array is ordered oldest-to-youngest, the JS and
  // native arrays are ordered youngest-to-oldest. We must add frames to aInfo
  // oldest-to-youngest. Thus, iterate over the profiling stack forwards and JS
  // and native arrays backwards. Note: this means the terminating condition
  // jsIndex and nativeIndex is being < 0.
  uint32_t profilingStackIndex = 0;
  int32_t nativeIndex = aNativeStack.mCount - 1;

  uint8_t* lastLabelFrameStackAddr = nullptr;

  // Iterate as long as there is at least one frame remaining.
  while (profilingStackIndex != profilingStackFrameCount || nativeIndex >= 0) {
    // There are 1 to 3 frames available. Find and add the oldest.
    uint8_t* profilingStackAddr = nullptr;
    uint8_t* nativeStackAddr = nullptr;

    if (profilingStackIndex != profilingStackFrameCount) {
      const ProfilingStackFrame& profilingStackFrame =
          profilingStackFrames[profilingStackIndex];

      if (profilingStackFrame.isLabelFrame() ||
          profilingStackFrame.isSpMarkerFrame()) {
        lastLabelFrameStackAddr = (uint8_t*)profilingStackFrame.stackAddress();
      }

      // Skip any JS_OSR frames. Such frames are used when the JS interpreter
      // enters a jit frame on a loop edge (via on-stack-replacement, or OSR).
      // To avoid both the profiling stack frame and jit frame being recorded
      // (and showing up twice), the interpreter marks the interpreter
      // profiling stack frame as JS_OSR to ensure that it doesn't get counted.
      if (profilingStackFrame.isOSRFrame()) {
        profilingStackIndex++;
        continue;
      }

      MOZ_ASSERT(lastLabelFrameStackAddr);
      profilingStackAddr = lastLabelFrameStackAddr;
    }

    if (nativeIndex >= 0) {
      nativeStackAddr = (uint8_t*)aNativeStack.mSPs[nativeIndex];
    }

    // If there's a native stack frame which has the same SP as a profiling
    // stack frame, pretend we didn't see the native stack frame.  Ditto for a
    // native stack frame which has the same SP as a JS stack frame.  In effect
    // this means profiling stack frames or JS frames trump conflicting native
    // frames.
    if (nativeStackAddr && (profilingStackAddr == nativeStackAddr)) {
      nativeStackAddr = nullptr;
      nativeIndex--;
      MOZ_ASSERT(profilingStackAddr);
    }

    // Sanity checks.
    MOZ_ASSERT_IF(profilingStackAddr, profilingStackAddr != nativeStackAddr);
    MOZ_ASSERT_IF(nativeStackAddr, nativeStackAddr != profilingStackAddr);

    // Check to see if profiling stack frame is top-most.
    if (profilingStackAddr > nativeStackAddr) {
      MOZ_ASSERT(profilingStackIndex < profilingStackFrameCount);
      const ProfilingStackFrame& profilingStackFrame =
          profilingStackFrames[profilingStackIndex];

      // Sp marker frames are just annotations and should not be recorded in
      // the profile.
      if (!profilingStackFrame.isSpMarkerFrame()) {
        aCollector.CollectProfilingStackFrame(profilingStackFrame);
      }
      profilingStackIndex++;
      continue;
    }

    // If we reach here, there must be a native stack frame and it must be the
    // greatest frame.
    if (nativeStackAddr) {
      MOZ_ASSERT(nativeIndex >= 0);
      void* addr = (void*)aNativeStack.mPCs[nativeIndex];
      aCollector.CollectNativeLeafAddr(addr);
    }
    if (nativeIndex >= 0) {
      nativeIndex--;
    }
  }
}

#  if defined(GP_OS_windows) && defined(USE_MOZ_STACK_WALK)
static HANDLE GetThreadHandle(PlatformData* aData);
#  endif

#  if defined(USE_FRAME_POINTER_STACK_WALK) || defined(USE_MOZ_STACK_WALK)
static void StackWalkCallback(uint32_t aFrameNumber, void* aPC, void* aSP,
                              void* aClosure) {
  NativeStack* nativeStack = static_cast<NativeStack*>(aClosure);
  MOZ_ASSERT(nativeStack->mCount < MAX_NATIVE_FRAMES);
  nativeStack->mSPs[nativeStack->mCount] = aSP;
  nativeStack->mPCs[nativeStack->mCount] = aPC;
  nativeStack->mCount++;
}
#  endif

#  if defined(USE_FRAME_POINTER_STACK_WALK)
static void DoFramePointerBacktrace(PSLockRef aLock,
                                    const RegisteredThread& aRegisteredThread,
                                    const Registers& aRegs,
                                    NativeStack& aNativeStack) {
  // WARNING: this function runs within the profiler's "critical section".
  // WARNING: this function might be called while the profiler is inactive, and
  //          cannot rely on ActivePS.

  // Start with the current function. We use 0 as the frame number here because
  // the FramePointerStackWalk() call below will use 1..N. This is a bit weird
  // but it doesn't matter because StackWalkCallback() doesn't use the frame
  // number argument.
  StackWalkCallback(/* frameNum */ 0, aRegs.mPC, aRegs.mSP, &aNativeStack);

  uint32_t maxFrames = uint32_t(MAX_NATIVE_FRAMES - aNativeStack.mCount);

  const void* stackEnd = aRegisteredThread.StackTop();
  if (aRegs.mFP >= aRegs.mSP && aRegs.mFP <= stackEnd) {
    FramePointerStackWalk(StackWalkCallback, /* skipFrames */ 0, maxFrames,
                          &aNativeStack, reinterpret_cast<void**>(aRegs.mFP),
                          const_cast<void*>(stackEnd));
  }
}
#  endif

#  if defined(USE_MOZ_STACK_WALK)
static void DoMozStackWalkBacktrace(PSLockRef aLock,
                                    const RegisteredThread& aRegisteredThread,
                                    const Registers& aRegs,
                                    NativeStack& aNativeStack) {
  // WARNING: this function runs within the profiler's "critical section".
  // WARNING: this function might be called while the profiler is inactive, and
  //          cannot rely on ActivePS.

  // Start with the current function. We use 0 as the frame number here because
  // the MozStackWalkThread() call below will use 1..N. This is a bit weird but
  // it doesn't matter because StackWalkCallback() doesn't use the frame number
  // argument.
  StackWalkCallback(/* frameNum */ 0, aRegs.mPC, aRegs.mSP, &aNativeStack);

  uint32_t maxFrames = uint32_t(MAX_NATIVE_FRAMES - aNativeStack.mCount);

  HANDLE thread = GetThreadHandle(aRegisteredThread.GetPlatformData());
  MOZ_ASSERT(thread);
  MozStackWalkThread(StackWalkCallback, /* skipFrames */ 0, maxFrames,
                     &aNativeStack, thread, /* context */ nullptr);
}
#  endif

#  ifdef USE_EHABI_STACKWALK
static void DoEHABIBacktrace(PSLockRef aLock,
                             const RegisteredThread& aRegisteredThread,
                             const Registers& aRegs,
                             NativeStack& aNativeStack) {
  // WARNING: this function runs within the profiler's "critical section".
  // WARNING: this function might be called while the profiler is inactive, and
  //          cannot rely on ActivePS.

  const mcontext_t* mcontext = &aRegs.mContext->uc_mcontext;
  mcontext_t savedContext;
  const ProfilingStack& profilingStack =
      aRegisteredThread.RacyRegisteredThread().ProfilingStack();

  // Now unwind whatever's left (starting from the original registers).
  aNativeStack.mCount +=
      EHABIStackWalk(*mcontext, const_cast<void*>(aRegisteredThread.StackTop()),
                     aNativeStack.mSPs + aNativeStack.mCount,
                     aNativeStack.mPCs + aNativeStack.mCount,
                     MAX_NATIVE_FRAMES - aNativeStack.mCount);
}
#  endif

#  ifdef USE_LUL_STACKWALK

// See the comment at the callsite for why this function is necessary.
#    if defined(MOZ_HAVE_ASAN_BLACKLIST)
MOZ_ASAN_BLACKLIST static void ASAN_memcpy(void* aDst, const void* aSrc,
                                           size_t aLen) {
  // The obvious thing to do here is call memcpy(). However, although
  // ASAN_memcpy() is not instrumented by ASAN, memcpy() still is, and the
  // false positive still manifests! So we must implement memcpy() ourselves
  // within this function.
  char* dst = static_cast<char*>(aDst);
  const char* src = static_cast<const char*>(aSrc);

  for (size_t i = 0; i < aLen; i++) {
    dst[i] = src[i];
  }
}
#    endif

static void DoLULBacktrace(PSLockRef aLock,
                           const RegisteredThread& aRegisteredThread,
                           const Registers& aRegs, NativeStack& aNativeStack) {
  // WARNING: this function runs within the profiler's "critical section".
  // WARNING: this function might be called while the profiler is inactive, and
  //          cannot rely on ActivePS.

  const mcontext_t* mc = &aRegs.mContext->uc_mcontext;

  lul::UnwindRegs startRegs;
  memset(&startRegs, 0, sizeof(startRegs));

#    if defined(GP_PLAT_amd64_linux) || defined(GP_PLAT_amd64_android)
  startRegs.xip = lul::TaggedUWord(mc->gregs[REG_RIP]);
  startRegs.xsp = lul::TaggedUWord(mc->gregs[REG_RSP]);
  startRegs.xbp = lul::TaggedUWord(mc->gregs[REG_RBP]);
#    elif defined(GP_PLAT_arm_linux) || defined(GP_PLAT_arm_android)
  startRegs.r15 = lul::TaggedUWord(mc->arm_pc);
  startRegs.r14 = lul::TaggedUWord(mc->arm_lr);
  startRegs.r13 = lul::TaggedUWord(mc->arm_sp);
  startRegs.r12 = lul::TaggedUWord(mc->arm_ip);
  startRegs.r11 = lul::TaggedUWord(mc->arm_fp);
  startRegs.r7 = lul::TaggedUWord(mc->arm_r7);
#    elif defined(GP_PLAT_arm64_linux) || defined(GP_PLAT_arm64_android)
  startRegs.pc = lul::TaggedUWord(mc->pc);
  startRegs.x29 = lul::TaggedUWord(mc->regs[29]);
  startRegs.x30 = lul::TaggedUWord(mc->regs[30]);
  startRegs.sp = lul::TaggedUWord(mc->sp);
#    elif defined(GP_PLAT_x86_linux) || defined(GP_PLAT_x86_android)
  startRegs.xip = lul::TaggedUWord(mc->gregs[REG_EIP]);
  startRegs.xsp = lul::TaggedUWord(mc->gregs[REG_ESP]);
  startRegs.xbp = lul::TaggedUWord(mc->gregs[REG_EBP]);
#    elif defined(GP_PLAT_mips64_linux)
  startRegs.pc = lul::TaggedUWord(mc->pc);
  startRegs.sp = lul::TaggedUWord(mc->gregs[29]);
  startRegs.fp = lul::TaggedUWord(mc->gregs[30]);
#    else
#      error "Unknown plat"
#    endif

  // Copy up to N_STACK_BYTES from rsp-REDZONE upwards, but not going past the
  // stack's registered top point.  Do some basic sanity checks too.  This
  // assumes that the TaggedUWord holding the stack pointer value is valid, but
  // it should be, since it was constructed that way in the code just above.

  // We could construct |stackImg| so that LUL reads directly from the stack in
  // question, rather than from a copy of it.  That would reduce overhead and
  // space use a bit.  However, it gives a problem with dynamic analysis tools
  // (ASan, TSan, Valgrind) which is that such tools will report invalid or
  // racing memory accesses, and such accesses will be reported deep inside LUL.
  // By taking a copy here, we can either sanitise the copy (for Valgrind) or
  // copy it using an unchecked memcpy (for ASan, TSan).  That way we don't have
  // to try and suppress errors inside LUL.
  //
  // N_STACK_BYTES is set to 160KB.  This is big enough to hold all stacks
  // observed in some minutes of testing, whilst keeping the size of this
  // function (DoNativeBacktrace)'s frame reasonable.  Most stacks observed in
  // practice are small, 4KB or less, and so the copy costs are insignificant
  // compared to other profiler overhead.
  //
  // |stackImg| is allocated on this (the sampling thread's) stack.  That
  // implies that the frame for this function is at least N_STACK_BYTES large.
  // In general it would be considered unacceptable to have such a large frame
  // on a stack, but it only exists for the unwinder thread, and so is not
  // expected to be a problem.  Allocating it on the heap is troublesome because
  // this function runs whilst the sampled thread is suspended, so any heap
  // allocation risks deadlock.  Allocating it as a global variable is not
  // thread safe, which would be a problem if we ever allow multiple sampler
  // threads.  Hence allocating it on the stack seems to be the least-worst
  // option.

  lul::StackImage stackImg;

  {
#    if defined(GP_PLAT_amd64_linux) || defined(GP_PLAT_amd64_android)
    uintptr_t rEDZONE_SIZE = 128;
    uintptr_t start = startRegs.xsp.Value() - rEDZONE_SIZE;
#    elif defined(GP_PLAT_arm_linux) || defined(GP_PLAT_arm_android)
    uintptr_t rEDZONE_SIZE = 0;
    uintptr_t start = startRegs.r13.Value() - rEDZONE_SIZE;
#    elif defined(GP_PLAT_arm64_linux) || defined(GP_PLAT_arm64_android)
    uintptr_t rEDZONE_SIZE = 0;
    uintptr_t start = startRegs.sp.Value() - rEDZONE_SIZE;
#    elif defined(GP_PLAT_x86_linux) || defined(GP_PLAT_x86_android)
    uintptr_t rEDZONE_SIZE = 0;
    uintptr_t start = startRegs.xsp.Value() - rEDZONE_SIZE;
#    elif defined(GP_PLAT_mips64_linux)
    uintptr_t rEDZONE_SIZE = 0;
    uintptr_t start = startRegs.sp.Value() - rEDZONE_SIZE;
#    else
#      error "Unknown plat"
#    endif
    uintptr_t end = reinterpret_cast<uintptr_t>(aRegisteredThread.StackTop());
    uintptr_t ws = sizeof(void*);
    start &= ~(ws - 1);
    end &= ~(ws - 1);
    uintptr_t nToCopy = 0;
    if (start < end) {
      nToCopy = end - start;
      if (nToCopy > lul::N_STACK_BYTES) nToCopy = lul::N_STACK_BYTES;
    }
    MOZ_ASSERT(nToCopy <= lul::N_STACK_BYTES);
    stackImg.mLen = nToCopy;
    stackImg.mStartAvma = start;
    if (nToCopy > 0) {
      // If this is a vanilla memcpy(), ASAN makes the following complaint:
      //
      //   ERROR: AddressSanitizer: stack-buffer-underflow ...
      //   ...
      //   HINT: this may be a false positive if your program uses some custom
      //   stack unwind mechanism or swapcontext
      //
      // This code is very much a custom stack unwind mechanism! So we use an
      // alternative memcpy() implementation that is ignored by ASAN.
#    if defined(MOZ_HAVE_ASAN_BLACKLIST)
      ASAN_memcpy(&stackImg.mContents[0], (void*)start, nToCopy);
#    else
      memcpy(&stackImg.mContents[0], (void*)start, nToCopy);
#    endif
      (void)VALGRIND_MAKE_MEM_DEFINED(&stackImg.mContents[0], nToCopy);
    }
  }

  size_t framePointerFramesAcquired = 0;
  lul::LUL* lul = CorePS::Lul(aLock);
  lul->Unwind(reinterpret_cast<uintptr_t*>(aNativeStack.mPCs),
              reinterpret_cast<uintptr_t*>(aNativeStack.mSPs),
              &aNativeStack.mCount, &framePointerFramesAcquired,
              MAX_NATIVE_FRAMES, &startRegs, &stackImg);

  // Update stats in the LUL stats object.  Unfortunately this requires
  // three global memory operations.
  lul->mStats.mContext += 1;
  lul->mStats.mCFI += aNativeStack.mCount - 1 - framePointerFramesAcquired;
  lul->mStats.mFP += framePointerFramesAcquired;
}

#  endif

#  ifdef HAVE_NATIVE_UNWIND
static void DoNativeBacktrace(PSLockRef aLock,
                              const RegisteredThread& aRegisteredThread,
                              const Registers& aRegs,
                              NativeStack& aNativeStack) {
  // This method determines which stackwalker is used for periodic and
  // synchronous samples. (Backtrace samples are treated differently, see
  // profiler_suspend_and_sample_thread() for details). The only part of the
  // ordering that matters is that LUL must precede FRAME_POINTER, because on
  // Linux they can both be present.
#    if defined(USE_LUL_STACKWALK)
  DoLULBacktrace(aLock, aRegisteredThread, aRegs, aNativeStack);
#    elif defined(USE_EHABI_STACKWALK)
  DoEHABIBacktrace(aLock, aRegisteredThread, aRegs, aNativeStack);
#    elif defined(USE_FRAME_POINTER_STACK_WALK)
  DoFramePointerBacktrace(aLock, aRegisteredThread, aRegs, aNativeStack);
#    elif defined(USE_MOZ_STACK_WALK)
  DoMozStackWalkBacktrace(aLock, aRegisteredThread, aRegs, aNativeStack);
#    else
#      error "Invalid configuration"
#    endif
}
#  endif

// Writes some components shared by periodic and synchronous profiles to
// ActivePS's ProfileBuffer. (This should only be called from DoSyncSample()
// and DoPeriodicSample().)
//
// The grammar for entry sequences is in a comment above
// ProfileBuffer::StreamSamplesToJSON.
static inline void DoSharedSample(PSLockRef aLock, bool aIsSynchronous,
                                  RegisteredThread& aRegisteredThread,
                                  const Registers& aRegs, uint64_t aSamplePos,
                                  ProfileBuffer& aBuffer) {
  // WARNING: this function runs within the profiler's "critical section".

  MOZ_ASSERT(!aBuffer.IsThreadSafe(),
             "Mutexes cannot be used inside this critical section");

  MOZ_RELEASE_ASSERT(ActivePS::Exists(aLock));

  ProfileBufferCollector collector(aBuffer, ActivePS::Features(aLock),
                                   aSamplePos);
  NativeStack nativeStack;
#  if defined(HAVE_NATIVE_UNWIND)
  if (ActivePS::FeatureStackWalk(aLock)) {
    DoNativeBacktrace(aLock, aRegisteredThread, aRegs, nativeStack);

    MergeStacks(ActivePS::Features(aLock), aIsSynchronous, aRegisteredThread,
                aRegs, nativeStack, collector);
  } else
#  endif
  {
    MergeStacks(ActivePS::Features(aLock), aIsSynchronous, aRegisteredThread,
                aRegs, nativeStack, collector);

    // We can't walk the whole native stack, but we can record the top frame.
    if (ActivePS::FeatureLeaf(aLock)) {
      aBuffer.AddEntry(ProfileBufferEntry::NativeLeafAddr((void*)aRegs.mPC));
    }
  }
}

// Writes the components of a synchronous sample to the given ProfileBuffer.
static void DoSyncSample(PSLockRef aLock, RegisteredThread& aRegisteredThread,
                         const TimeStamp& aNow, const Registers& aRegs,
                         ProfileBuffer& aBuffer) {
  // WARNING: this function runs within the profiler's "critical section".

  uint64_t samplePos =
      aBuffer.AddThreadIdEntry(aRegisteredThread.Info()->ThreadId());

  TimeDuration delta = aNow - CorePS::ProcessStartTime();
  aBuffer.AddEntry(ProfileBufferEntry::Time(delta.ToMilliseconds()));

  DoSharedSample(aLock, /* aIsSynchronous = */ true, aRegisteredThread, aRegs,
                 samplePos, aBuffer);
}

// Writes the components of a periodic sample to ActivePS's ProfileBuffer.
// The ThreadId entry is already written in the main ProfileBuffer, its location
// is `aSamplePos`, we can write the rest to `aBuffer` (which may be different).
static void DoPeriodicSample(PSLockRef aLock,
                             RegisteredThread& aRegisteredThread,
                             ProfiledThreadData& aProfiledThreadData,
                             const Registers& aRegs, uint64_t aSamplePos,
                             ProfileBuffer& aBuffer) {
  // WARNING: this function runs within the profiler's "critical section".

  DoSharedSample(aLock, /* aIsSynchronous = */ false, aRegisteredThread, aRegs,
                 aSamplePos, aBuffer);
}

// END sampling/unwinding code
////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////
// BEGIN saving/streaming code

const static uint64_t kJS_MAX_SAFE_UINTEGER = +9007199254740991ULL;

static int64_t SafeJSInteger(uint64_t aValue) {
  return aValue <= kJS_MAX_SAFE_UINTEGER ? int64_t(aValue) : -1;
}

static void AddSharedLibraryInfoToStream(JSONWriter& aWriter,
                                         const SharedLibrary& aLib) {
  aWriter.StartObjectElement();
  aWriter.IntProperty("start", SafeJSInteger(aLib.GetStart()));
  aWriter.IntProperty("end", SafeJSInteger(aLib.GetEnd()));
  aWriter.IntProperty("offset", SafeJSInteger(aLib.GetOffset()));
  aWriter.StringProperty("name", aLib.GetModuleName().c_str());
  aWriter.StringProperty("path", aLib.GetModulePath().c_str());
  aWriter.StringProperty("debugName", aLib.GetDebugName().c_str());
  aWriter.StringProperty("debugPath", aLib.GetDebugPath().c_str());
  aWriter.StringProperty("breakpadId", aLib.GetBreakpadId().c_str());
  aWriter.StringProperty("arch", aLib.GetArch().c_str());
  aWriter.EndObject();
}

void AppendSharedLibraries(JSONWriter& aWriter) {
  SharedLibraryInfo info = SharedLibraryInfo::GetInfoForSelf();
  info.SortByAddress();
  for (size_t i = 0; i < info.GetSize(); i++) {
    AddSharedLibraryInfoToStream(aWriter, info.GetEntry(i));
  }
}

static void StreamCategories(SpliceableJSONWriter& aWriter) {
  // Same order as ProfilingCategory. Format:
  // [
  //   {
  //     name: "Idle",
  //     color: "transparent",
  //     subcategories: ["Other"],
  //   },
  //   {
  //     name: "Other",
  //     color: "grey",
  //     subcategories: [
  //       "JSM loading",
  //       "Subprocess launching",
  //       "DLL loading"
  //     ]
  //   },
  //   ...
  // ]

#  define CATEGORY_JSON_BEGIN_CATEGORY(name, labelAsString, color) \
    aWriter.Start();                                               \
    aWriter.StringProperty("name", labelAsString);                 \
    aWriter.StringProperty("color", color);                        \
    aWriter.StartArrayProperty("subcategories");
#  define CATEGORY_JSON_SUBCATEGORY(supercategory, name, labelAsString) \
    aWriter.StringElement(labelAsString);
#  define CATEGORY_JSON_END_CATEGORY \
    aWriter.EndArray();              \
    aWriter.EndObject();

  BASE_PROFILING_CATEGORY_LIST(CATEGORY_JSON_BEGIN_CATEGORY,
                               CATEGORY_JSON_SUBCATEGORY,
                               CATEGORY_JSON_END_CATEGORY)

#  undef CATEGORY_JSON_BEGIN_CATEGORY
#  undef CATEGORY_JSON_SUBCATEGORY
#  undef CATEGORY_JSON_END_CATEGORY
}

static int64_t MicrosecondsSince1970();

static void StreamMetaJSCustomObject(PSLockRef aLock,
                                     SpliceableJSONWriter& aWriter,
                                     bool aIsShuttingDown) {
  MOZ_RELEASE_ASSERT(CorePS::Exists() && ActivePS::Exists(aLock));

  aWriter.IntProperty("version", 19);

  // The "startTime" field holds the number of milliseconds since midnight
  // January 1, 1970 GMT. This grotty code computes (Now - (Now -
  // ProcessStartTime)) to convert CorePS::ProcessStartTime() into that form.
  TimeDuration delta = TimeStamp::NowUnfuzzed() - CorePS::ProcessStartTime();
  aWriter.DoubleProperty(
      "startTime", MicrosecondsSince1970() / 1000.0 - delta.ToMilliseconds());

  // Write the shutdownTime field. Unlike startTime, shutdownTime is not an
  // absolute time stamp: It's relative to startTime. This is consistent with
  // all other (non-"startTime") times anywhere in the profile JSON.
  if (aIsShuttingDown) {
    aWriter.DoubleProperty("shutdownTime", profiler_time());
  } else {
    aWriter.NullProperty("shutdownTime");
  }

  aWriter.StartArrayProperty("categories");
  StreamCategories(aWriter);
  aWriter.EndArray();

  if (!CorePS::IsMainThread()) {
    // Leave the rest of the properties out if we're not on the main thread.
    // At the moment, the only case in which this function is called on a
    // background thread is if we're in a content process and are going to
    // send this profile to the parent process. In that case, the parent
    // process profile's "meta" object already has the rest of the properties,
    // and the parent process profile is dumped on that process's main thread.
    return;
  }

  aWriter.DoubleProperty("interval", ActivePS::Interval(aLock));
  aWriter.IntProperty("stackwalk", ActivePS::FeatureStackWalk(aLock));

#  ifdef DEBUG
  aWriter.IntProperty("debug", 1);
#  else
  aWriter.IntProperty("debug", 0);
#  endif

  aWriter.IntProperty("gcpoison", 0);

  aWriter.IntProperty("asyncstack", 0);

  aWriter.IntProperty("processType", 0);
}

static void StreamPages(PSLockRef aLock, SpliceableJSONWriter& aWriter) {
  MOZ_RELEASE_ASSERT(CorePS::Exists());
  ActivePS::DiscardExpiredPages(aLock);
  for (const auto& page : ActivePS::ProfiledPages(aLock)) {
    page->StreamJSON(aWriter);
  }
}

static void locked_profiler_stream_json_for_this_process(
    PSLockRef aLock, SpliceableJSONWriter& aWriter, double aSinceTime,
    bool aIsShuttingDown, bool aOnlyThreads = false) {
  LOG("locked_profiler_stream_json_for_this_process");

  MOZ_RELEASE_ASSERT(CorePS::Exists() && ActivePS::Exists(aLock));

  AUTO_PROFILER_STATS(base_locked_profiler_stream_json_for_this_process);

  const double collectionStartMs = profiler_time();

  ProfileBuffer& buffer = ActivePS::Buffer(aLock);

  // If there is a set "Window length", discard older data.
  Maybe<double> durationS = ActivePS::Duration(aLock);
  if (durationS.isSome()) {
    const double durationStartMs = collectionStartMs - *durationS * 1000;
    buffer.DiscardSamplesBeforeTime(durationStartMs);
  }

  if (!aOnlyThreads) {
    // Put shared library info
    aWriter.StartArrayProperty("libs");
    AppendSharedLibraries(aWriter);
    aWriter.EndArray();

    // Put meta data
    aWriter.StartObjectProperty("meta");
    { StreamMetaJSCustomObject(aLock, aWriter, aIsShuttingDown); }
    aWriter.EndObject();

    // Put page data
    aWriter.StartArrayProperty("pages");
    { StreamPages(aLock, aWriter); }
    aWriter.EndArray();

    buffer.StreamProfilerOverheadToJSON(aWriter, CorePS::ProcessStartTime(),
                                        aSinceTime);
    buffer.StreamCountersToJSON(aWriter, CorePS::ProcessStartTime(),
                                aSinceTime);

    // Lists the samples for each thread profile
    aWriter.StartArrayProperty("threads");
  }

  // if aOnlyThreads is true, the only output will be the threads array items.
  {
    ActivePS::DiscardExpiredDeadProfiledThreads(aLock);
    Vector<Pair<RegisteredThread*, ProfiledThreadData*>> threads =
        ActivePS::ProfiledThreads(aLock);
    for (auto& thread : threads) {
      ProfiledThreadData* profiledThreadData = thread.second();
      profiledThreadData->StreamJSON(buffer, aWriter,
                                     CorePS::ProcessName(aLock),
                                     CorePS::ProcessStartTime(), aSinceTime);
    }
  }

  if (!aOnlyThreads) {
    aWriter.EndArray();

    aWriter.StartArrayProperty("pausedRanges");
    { buffer.StreamPausedRangesToJSON(aWriter, aSinceTime); }
    aWriter.EndArray();
  }

  const double collectionEndMs = profiler_time();

  // Record timestamps for the collection into the buffer, so that consumers
  // know why we didn't collect any samples for its duration.
  // We put these entries into the buffer after we've collected the profile,
  // so they'll be visible for the *next* profile collection (if they haven't
  // been overwritten due to buffer wraparound by then).
  buffer.AddEntry(ProfileBufferEntry::CollectionStart(collectionStartMs));
  buffer.AddEntry(ProfileBufferEntry::CollectionEnd(collectionEndMs));
}

bool profiler_stream_json_for_this_process(SpliceableJSONWriter& aWriter,
                                           double aSinceTime,
                                           bool aIsShuttingDown,
                                           bool aOnlyThreads) {
  LOG("profiler_stream_json_for_this_process");

  MOZ_RELEASE_ASSERT(CorePS::Exists());

  PSAutoLock lock;

  if (!ActivePS::Exists(lock)) {
    return false;
  }

  locked_profiler_stream_json_for_this_process(lock, aWriter, aSinceTime,
                                               aIsShuttingDown, aOnlyThreads);
  return true;
}

// END saving/streaming code
////////////////////////////////////////////////////////////////////////

static char FeatureCategory(uint32_t aFeature) {
  if (aFeature & DefaultFeatures()) {
    if (aFeature & AvailableFeatures()) {
      return 'D';
    }
    return 'd';
  }

  if (aFeature & StartupExtraDefaultFeatures()) {
    if (aFeature & AvailableFeatures()) {
      return 'S';
    }
    return 's';
  }

  if (aFeature & AvailableFeatures()) {
    return '-';
  }
  return 'x';
}

static void PrintUsageThenExit(int aExitCode) {
  printf(
      "\n"
      "Profiler environment variable usage:\n"
      "\n"
      "  MOZ_BASE_PROFILER_HELP\n"
      "  If set to any value, prints this message.\n"
      "  Use MOZ_PROFILER_HELP for Gecko Profiler help.\n"
      "\n"
      "  MOZ_BASE_PROFILER_{,DEBUG_,VERBOSE}LOGGING\n"
      "  Enables logging to stdout. The levels of logging available are\n"
      "  'MOZ_BASE_PROFILER_LOGGING' (least verbose), '..._DEBUG_LOGGING',\n"
      "  '..._VERBOSE_LOGGING' (most verbose)\n"
      "\n"
      "  MOZ_BASE_PROFILER_STARTUP\n"
      "  If set to any value other than '' or '0'/'N'/'n', starts the\n"
      "  profiler immediately on start-up.\n"
      "  Useful if you want profile code that runs very early.\n"
      "\n"
      "  MOZ_BASE_PROFILER_STARTUP_ENTRIES=<1..>\n"
      "  If MOZ_BASE_PROFILER_STARTUP is set, specifies the number of entries\n"
      "  per process in the profiler's circular buffer when the profiler is\n"
      "  first started.\n"
      "  If unset, the platform default is used:\n"
      "  %u entries per process, or %u when MOZ_BASE_PROFILER_STARTUP is set.\n"
      "  (8 bytes per entry -> %u or %u total bytes per process)\n"
      "\n"
      "  MOZ_BASE_PROFILER_STARTUP_DURATION=<1..>\n"
      "  If MOZ_BASE_PROFILER_STARTUP is set, specifies the maximum life time\n"
      "  of entries in the the profiler's circular buffer when the profiler\n"
      "  is first started, in seconds.\n"
      "  If unset, the life time of the entries will only be restricted by\n"
      "  MOZ_BASE_PROFILER_STARTUP_ENTRIES (or its default value), and no\n"
      "  additional time duration restriction will be applied.\n"
      "\n"
      "  MOZ_BASE_PROFILER_STARTUP_INTERVAL=<1..1000>\n"
      "  If MOZ_BASE_PROFILER_STARTUP is set, specifies the sample interval,\n"
      "  measured in milliseconds, when the profiler is first started.\n"
      "  If unset, the platform default is used.\n"
      "\n"
      "  MOZ_BASE_PROFILER_STARTUP_FEATURES_BITFIELD=<Number>\n"
      "  If MOZ_BASE_PROFILER_STARTUP is set, specifies the profiling\n"
      "  features, as the integer value of the features bitfield.\n"
      "  If unset, the value from MOZ_BASE_PROFILER_STARTUP_FEATURES is used.\n"
      "\n"
      "  MOZ_BASE_PROFILER_STARTUP_FEATURES=<Features>\n"
      "  If MOZ_BASE_PROFILER_STARTUP is set, specifies the profiling\n"
      "  features, as a comma-separated list of strings.\n"
      "  Ignored if MOZ_BASE_PROFILER_STARTUP_FEATURES_BITFIELD is set.\n"
      "  If unset, the platform default is used.\n"
      "\n"
      "    Features: (x=unavailable, D/d=default/unavailable,\n"
      "               S/s=MOZ_BASE_PROFILER_STARTUP extra "
      "default/unavailable)\n",
      unsigned(BASE_PROFILER_DEFAULT_ENTRIES.Value()),
      unsigned(BASE_PROFILER_DEFAULT_STARTUP_ENTRIES.Value()),
      unsigned(BASE_PROFILER_DEFAULT_ENTRIES.Value() * 8),
      unsigned(BASE_PROFILER_DEFAULT_STARTUP_ENTRIES.Value() * 8));

#  define PRINT_FEATURE(n_, str_, Name_, desc_)                             \
    printf("    %c %5u: \"%s\" (%s)\n",                                     \
           FeatureCategory(ProfilerFeature::Name_), ProfilerFeature::Name_, \
           str_, desc_);

  BASE_PROFILER_FOR_EACH_FEATURE(PRINT_FEATURE)

#  undef PRINT_FEATURE

  printf(
      "    -        \"default\" (All above D+S defaults)\n"
      "\n"
      "  MOZ_BASE_PROFILER_STARTUP_FILTERS=<Filters>\n"
      "  If MOZ_BASE_PROFILER_STARTUP is set, specifies the thread filters, as "
      "a\n"
      "  comma-separated list of strings. A given thread will be sampled if\n"
      "  any of the filters is a case-insensitive substring of the thread\n"
      "  name. If unset, a default is used.\n"
      "\n"
      "  MOZ_BASE_PROFILER_SHUTDOWN\n"
      "  If set, the profiler saves a profile to the named file on shutdown.\n"
      "\n"
      "  MOZ_BASE_PROFILER_SYMBOLICATE\n"
      "  If set, the profiler will pre-symbolicate profiles.\n"
      "  *Note* This will add a significant pause when gathering data, and\n"
      "  is intended mainly for local development.\n"
      "\n"
      "  MOZ_BASE_PROFILER_LUL_TEST\n"
      "  If set to any value, runs LUL unit tests at startup.\n"
      "\n"
      "  This platform %s native unwinding.\n"
      "\n",
#  if defined(HAVE_NATIVE_UNWIND)
      "supports"
#  else
      "does not support"
#  endif
  );

  exit(aExitCode);
}

////////////////////////////////////////////////////////////////////////
// BEGIN Sampler

#  if defined(GP_OS_linux) || defined(GP_OS_android)
struct SigHandlerCoordinator;
#  endif

// Sampler performs setup and teardown of the state required to sample with the
// profiler. Sampler may exist when ActivePS is not present.
//
// SuspendAndSampleAndResumeThread must only be called from a single thread,
// and must not sample the thread it is being called from. A separate Sampler
// instance must be used for each thread which wants to capture samples.

// WARNING WARNING WARNING WARNING WARNING WARNING WARNING WARNING
//
// With the exception of SamplerThread, all Sampler objects must be Disable-d
// before releasing the lock which was used to create them. This avoids races
// on linux with the SIGPROF signal handler.

class Sampler {
 public:
  // Sets up the profiler such that it can begin sampling.
  explicit Sampler(PSLockRef aLock);

  // Disable the sampler, restoring it to its previous state. This must be
  // called once, and only once, before the Sampler is destroyed.
  void Disable(PSLockRef aLock);

  // This method suspends and resumes the samplee thread. It calls the passed-in
  // function-like object aProcessRegs (passing it a populated |const
  // Registers&| arg) while the samplee thread is suspended.
  //
  // Func must be a function-like object of type `void()`.
  template <typename Func>
  void SuspendAndSampleAndResumeThread(
      PSLockRef aLock, const RegisteredThread& aRegisteredThread,
      const TimeStamp& aNow, const Func& aProcessRegs);

 private:
#  if defined(GP_OS_linux) || defined(GP_OS_android)
  // Used to restore the SIGPROF handler when ours is removed.
  struct sigaction mOldSigprofHandler;

  // This process' ID. Needed as an argument for tgkill in
  // SuspendAndSampleAndResumeThread.
  int mMyPid;

  // The sampler thread's ID.  Used to assert that it is not sampling itself,
  // which would lead to deadlock.
  int mSamplerTid;

 public:
  // This is the one-and-only variable used to communicate between the sampler
  // thread and the samplee thread's signal handler. It's static because the
  // samplee thread's signal handler is static.
  static struct SigHandlerCoordinator* sSigHandlerCoordinator;
#  endif
};

// END Sampler
////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////
// BEGIN SamplerThread

// The sampler thread controls sampling and runs whenever the profiler is
// active. It periodically runs through all registered threads, finds those
// that should be sampled, then pauses and samples them.

class SamplerThread {
 public:
  // Creates a sampler thread, but doesn't start it.
  SamplerThread(PSLockRef aLock, uint32_t aActivityGeneration,
                double aIntervalMilliseconds);
  ~SamplerThread();

  // This runs on (is!) the sampler thread.
  void Run();

  // This runs on the main thread.
  void Stop(PSLockRef aLock);

 private:
  // This suspends the calling thread for the given number of microseconds.
  // Best effort timing.
  void SleepMicro(uint32_t aMicroseconds);

  // The sampler used to suspend and sample threads.
  Sampler mSampler;

  // The activity generation, for detecting when the sampler thread must stop.
  const uint32_t mActivityGeneration;

  // The interval between samples, measured in microseconds.
  const int mIntervalMicroseconds;

  // The OS-specific handle for the sampler thread.
#  if defined(GP_OS_windows)
  HANDLE mThread;
#  elif defined(GP_OS_darwin) || defined(GP_OS_linux) || defined(GP_OS_android)
  pthread_t mThread;
#  endif

  SamplerThread(const SamplerThread&) = delete;
  void operator=(const SamplerThread&) = delete;
};

// This function is required because we need to create a SamplerThread within
// ActivePS's constructor, but SamplerThread is defined after ActivePS. It
// could probably be removed by moving some code around.
static SamplerThread* NewSamplerThread(PSLockRef aLock, uint32_t aGeneration,
                                       double aInterval) {
  return new SamplerThread(aLock, aGeneration, aInterval);
}

// This function is the sampler thread.  This implementation is used for all
// targets.
void SamplerThread::Run() {
  // TODO: If possible, name this thread later on, after NSPR becomes available.
  // PR_SetCurrentThreadName("SamplerThread");

  // Features won't change during this SamplerThread's lifetime, so we can
  // determine now whether stack sampling is required.
  const bool noStackSampling = []() {
    PSAutoLock lock;
    if (!ActivePS::Exists(lock)) {
      // If there is no active profiler, it doesn't matter what we return,
      // because this thread will exit before any stack sampling is attempted.
      return false;
    }
    return ActivePS::FeatureNoStackSampling(lock);
  }();

  // Use local BlocksRingBuffer&ProfileBuffer to capture the stack.
  // (This is to avoid touching the CorePS::BlocksRingBuffer lock while
  // a thread is suspended, because that thread could be working with
  // the CorePS::BlocksRingBuffer as well.)
  BlocksRingBuffer localBlocksRingBuffer(
      BlocksRingBuffer::ThreadSafety::WithoutMutex);
  ProfileBuffer localProfileBuffer(localBlocksRingBuffer,
                                   MakePowerOfTwo32<65536>());

  // Will be kept between collections, to know what each collection does.
  auto previousState = localBlocksRingBuffer.GetState();

  // This will be positive if we are running behind schedule (sampling less
  // frequently than desired) and negative if we are ahead of schedule.
  TimeDuration lastSleepOvershoot = 0;
  TimeStamp sampleStart = TimeStamp::NowUnfuzzed();

  while (true) {
    // This scope is for |lock|. It ends before we sleep below.
    {
      PSAutoLock lock;
      TimeStamp lockAcquired = TimeStamp::NowUnfuzzed();

      if (!ActivePS::Exists(lock)) {
        return;
      }

      // At this point profiler_stop() might have been called, and
      // profiler_start() might have been called on another thread. If this
      // happens the generation won't match.
      if (ActivePS::Generation(lock) != mActivityGeneration) {
        return;
      }

      ActivePS::ClearExpiredExitProfiles(lock);

      TimeStamp expiredMarkersCleaned = TimeStamp::NowUnfuzzed();

      if (!ActivePS::IsPaused(lock)) {
        TimeDuration delta = sampleStart - CorePS::ProcessStartTime();
        ProfileBuffer& buffer = ActivePS::Buffer(lock);

        // handle per-process generic counters
        const Vector<BaseProfilerCount*>& counters = CorePS::Counters(lock);
        for (auto& counter : counters) {
          // create Buffer entries for each counter
          buffer.AddEntry(ProfileBufferEntry::CounterId(counter));
          buffer.AddEntry(ProfileBufferEntry::Time(delta.ToMilliseconds()));
          // XXX support keyed maps of counts
          // In the future, we'll support keyed counters - for example, counters
          // with a key which is a thread ID. For "simple" counters we'll just
          // use a key of 0.
          int64_t count;
          uint64_t number;
          counter->Sample(count, number);
          buffer.AddEntry(ProfileBufferEntry::CounterKey(0));
          buffer.AddEntry(ProfileBufferEntry::Count(count));
          if (number) {
            buffer.AddEntry(ProfileBufferEntry::Number(number));
          }
        }
        TimeStamp countersSampled = TimeStamp::NowUnfuzzed();

        if (!noStackSampling) {
          const Vector<LiveProfiledThreadData>& liveThreads =
              ActivePS::LiveProfiledThreads(lock);

          for (auto& thread : liveThreads) {
            RegisteredThread* registeredThread = thread.mRegisteredThread;
            ProfiledThreadData* profiledThreadData =
                thread.mProfiledThreadData.get();
            RefPtr<ThreadInfo> info = registeredThread->Info();

            // If the thread is asleep and has been sampled before in the same
            // sleep episode, find and copy the previous sample, as that's
            // cheaper than taking a new sample.
            if (registeredThread->RacyRegisteredThread()
                    .CanDuplicateLastSampleDueToSleep()) {
              bool dup_ok = ActivePS::Buffer(lock).DuplicateLastSample(
                  info->ThreadId(), CorePS::ProcessStartTime(),
                  profiledThreadData->LastSample());
              if (dup_ok) {
                continue;
              }
            }

            AUTO_PROFILER_STATS(base_SamplerThread_Run_DoPeriodicSample);

            TimeStamp now = TimeStamp::NowUnfuzzed();

            // Add the thread ID now, so we know its position in the main
            // buffer, which is used by some JS data. (DoPeriodicSample only
            // knows about the temporary local buffer.)
            uint64_t samplePos =
                buffer.AddThreadIdEntry(registeredThread->Info()->ThreadId());
            profiledThreadData->LastSample() = Some(samplePos);

            // Also add the time, so it's always there after the thread ID, as
            // expected by the parser. (Other stack data is optional.)
            TimeDuration delta = now - CorePS::ProcessStartTime();
            buffer.AddEntry(ProfileBufferEntry::Time(delta.ToMilliseconds()));

            mSampler.SuspendAndSampleAndResumeThread(
                lock, *registeredThread, now,
                [&](const Registers& aRegs, const TimeStamp& aNow) {
                  DoPeriodicSample(lock, *registeredThread, *profiledThreadData,
                                   aRegs, samplePos, localProfileBuffer);
                });

            // If data is complete, copy it into the global buffer.
            auto state = localBlocksRingBuffer.GetState();
            if (state.mClearedBlockCount != previousState.mClearedBlockCount) {
              LOG("Stack sample too big for local storage, needed %u bytes",
                  unsigned(
                      state.mRangeEnd.ConvertToProfileBufferIndex() -
                      previousState.mRangeEnd.ConvertToProfileBufferIndex()));
            } else if (state.mRangeEnd.ConvertToProfileBufferIndex() -
                           previousState.mRangeEnd
                               .ConvertToProfileBufferIndex() >=
                       CorePS::CoreBlocksRingBuffer().BufferLength()->Value()) {
              LOG("Stack sample too big for profiler storage, needed %u bytes",
                  unsigned(
                      state.mRangeEnd.ConvertToProfileBufferIndex() -
                      previousState.mRangeEnd.ConvertToProfileBufferIndex()));
            } else {
              CorePS::CoreBlocksRingBuffer().AppendContents(
                  localBlocksRingBuffer);
            }

            // Clean up for the next run.
            localBlocksRingBuffer.Clear();
            previousState = localBlocksRingBuffer.GetState();
          }
        }

#  if defined(USE_LUL_STACKWALK)
        // The LUL unwind object accumulates frame statistics. Periodically we
        // should poke it to give it a chance to print those statistics.  This
        // involves doing I/O (fprintf, __android_log_print, etc.) and so
        // can't safely be done from the critical section inside
        // SuspendAndSampleAndResumeThread, which is why it is done here.
        CorePS::Lul(lock)->MaybeShowStats();
#  endif
        TimeStamp threadsSampled = TimeStamp::NowUnfuzzed();

        buffer.CollectOverheadStats(delta, lockAcquired - sampleStart,
                                    expiredMarkersCleaned - lockAcquired,
                                    countersSampled - expiredMarkersCleaned,
                                    threadsSampled - countersSampled);
      }
    }
    // gPSMutex is not held after this point.

    // Calculate how long a sleep to request.  After the sleep, measure how
    // long we actually slept and take the difference into account when
    // calculating the sleep interval for the next iteration.  This is an
    // attempt to keep "to schedule" in the presence of inaccuracy of the
    // actual sleep intervals.
    TimeStamp targetSleepEndTime =
        sampleStart + TimeDuration::FromMicroseconds(mIntervalMicroseconds);
    TimeStamp beforeSleep = TimeStamp::NowUnfuzzed();
    TimeDuration targetSleepDuration = targetSleepEndTime - beforeSleep;
    double sleepTime = std::max(
        0.0, (targetSleepDuration - lastSleepOvershoot).ToMicroseconds());
    SleepMicro(static_cast<uint32_t>(sleepTime));
    sampleStart = TimeStamp::NowUnfuzzed();
    lastSleepOvershoot =
        sampleStart - (beforeSleep + TimeDuration::FromMicroseconds(sleepTime));
  }
}

// Temporary closing namespaces from enclosing platform.cpp.
}  // namespace baseprofiler
}  // namespace mozilla

// We #include these files directly because it means those files can use
// declarations from this file trivially.  These provide target-specific
// implementations of all SamplerThread methods except Run().
#  if defined(GP_OS_windows)
#    include "platform-win32.cpp"
#  elif defined(GP_OS_darwin)
#    include "platform-macos.cpp"
#  elif defined(GP_OS_linux) || defined(GP_OS_android)
#    include "platform-linux-android.cpp"
#  else
#    error "bad platform"
#  endif

namespace mozilla {
namespace baseprofiler {

UniquePlatformData AllocPlatformData(int aThreadId) {
  return UniquePlatformData(new PlatformData(aThreadId));
}

void PlatformDataDestructor::operator()(PlatformData* aData) { delete aData; }

// END SamplerThread
////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////
// BEGIN externally visible functions

static uint32_t ParseFeature(const char* aFeature, bool aIsStartup) {
  if (strcmp(aFeature, "default") == 0) {
    return (aIsStartup ? (DefaultFeatures() | StartupExtraDefaultFeatures())
                       : DefaultFeatures()) &
           AvailableFeatures();
  }

#  define PARSE_FEATURE_BIT(n_, str_, Name_, desc_) \
    if (strcmp(aFeature, str_) == 0) {              \
      return ProfilerFeature::Name_;                \
    }

  BASE_PROFILER_FOR_EACH_FEATURE(PARSE_FEATURE_BIT)

#  undef PARSE_FEATURE_BIT

  printf("\nUnrecognized feature \"%s\".\n\n", aFeature);
  PrintUsageThenExit(1);
  return 0;
}

uint32_t ParseFeaturesFromStringArray(const char** aFeatures,
                                      uint32_t aFeatureCount,
                                      bool aIsStartup /* = false */) {
  uint32_t features = 0;
  for (size_t i = 0; i < aFeatureCount; i++) {
    features |= ParseFeature(aFeatures[i], aIsStartup);
  }
  return features;
}

// Find the RegisteredThread for the current thread. This should only be called
// in places where TLSRegisteredThread can't be used.
static RegisteredThread* FindCurrentThreadRegisteredThread(PSLockRef aLock) {
  int id = profiler_current_thread_id();
  const Vector<UniquePtr<RegisteredThread>>& registeredThreads =
      CorePS::RegisteredThreads(aLock);
  for (auto& registeredThread : registeredThreads) {
    if (registeredThread->Info()->ThreadId() == id) {
      return registeredThread.get();
    }
  }

  return nullptr;
}

static ProfilingStack* locked_register_thread(PSLockRef aLock,
                                              const char* aName,
                                              void* aStackTop) {
  MOZ_RELEASE_ASSERT(CorePS::Exists());

  MOZ_RELEASE_ASSERT(!FindCurrentThreadRegisteredThread(aLock));

  VTUNE_REGISTER_THREAD(aName);

  if (!TLSRegisteredThread::Init(aLock)) {
    return nullptr;
  }

  RefPtr<ThreadInfo> info = new ThreadInfo(aName, profiler_current_thread_id(),
                                           CorePS::IsMainThread());
  UniquePtr<RegisteredThread> registeredThread =
      MakeUnique<RegisteredThread>(info, aStackTop);

  TLSRegisteredThread::SetRegisteredThread(aLock, registeredThread.get());

  if (ActivePS::Exists(aLock) && ActivePS::ShouldProfileThread(aLock, info)) {
    registeredThread->RacyRegisteredThread().SetIsBeingProfiled(true);
    ActivePS::AddLiveProfiledThread(aLock, registeredThread.get(),
                                    MakeUnique<ProfiledThreadData>(info));
  }

  ProfilingStack* profilingStack =
      &registeredThread->RacyRegisteredThread().ProfilingStack();

  CorePS::AppendRegisteredThread(aLock, std::move(registeredThread));

  return profilingStack;
}

static void locked_profiler_start(PSLockRef aLock, PowerOfTwo32 aCapacity,
                                  double aInterval, uint32_t aFeatures,
                                  const char** aFilters, uint32_t aFilterCount,
                                  const Maybe<double>& aDuration);

static Vector<const char*> SplitAtCommas(const char* aString,
                                         UniquePtr<char[]>& aStorage) {
  size_t len = strlen(aString);
  aStorage = MakeUnique<char[]>(len + 1);
  PodCopy(aStorage.get(), aString, len + 1);

  // Iterate over all characters in aStorage and split at commas, by
  // overwriting commas with the null char.
  Vector<const char*> array;
  size_t currentElementStart = 0;
  for (size_t i = 0; i <= len; i++) {
    if (aStorage[i] == ',') {
      aStorage[i] = '\0';
    }
    if (aStorage[i] == '\0') {
      MOZ_RELEASE_ASSERT(array.append(&aStorage[currentElementStart]));
      currentElementStart = i + 1;
    }
  }
  return array;
}

void profiler_init(void* aStackTop) {
  LOG("profiler_init");

  VTUNE_INIT();

  MOZ_RELEASE_ASSERT(!CorePS::Exists());

  if (getenv("MOZ_BASE_PROFILER_HELP")) {
    PrintUsageThenExit(0);  // terminates execution
  }

  SharedLibraryInfo::Initialize();

  uint32_t features = DefaultFeatures() & AvailableFeatures();

  UniquePtr<char[]> filterStorage;

  Vector<const char*> filters;
  MOZ_RELEASE_ASSERT(filters.append(kMainThreadName));

  PowerOfTwo32 capacity = BASE_PROFILER_DEFAULT_ENTRIES;
  Maybe<double> duration = Nothing();
  double interval = BASE_PROFILER_DEFAULT_INTERVAL;

  {
    PSAutoLock lock;

    // We've passed the possible failure point. Instantiate CorePS, which
    // indicates that the profiler has initialized successfully.
    CorePS::Create(lock);

    locked_register_thread(lock, kMainThreadName, aStackTop);

    // Platform-specific initialization.
    PlatformInit(lock);

    // (Linux-only) We could create CorePS::mLul and read unwind info into it
    // at this point. That would match the lifetime implied by destruction of
    // it in profiler_shutdown() just below. However, that gives a big delay on
    // startup, even if no profiling is actually to be done. So, instead, it is
    // created on demand at the first call to PlatformStart().

    const char* startupEnv = getenv("MOZ_BASE_PROFILER_STARTUP");
    if (!startupEnv || startupEnv[0] == '\0' ||
        ((startupEnv[0] == '0' || startupEnv[0] == 'N' ||
          startupEnv[0] == 'n') &&
         startupEnv[1] == '\0')) {
      return;
    }

    LOG("- MOZ_BASE_PROFILER_STARTUP is set");

    // Startup default capacity may be different.
    capacity = BASE_PROFILER_DEFAULT_STARTUP_ENTRIES;

    const char* startupCapacity = getenv("MOZ_BASE_PROFILER_STARTUP_ENTRIES");
    if (startupCapacity && startupCapacity[0] != '\0') {
      errno = 0;
      long capacityLong = strtol(startupCapacity, nullptr, 10);
      // `long` could be 32 or 64 bits, so we force a 64-bit comparison with
      // the maximum 32-bit signed number (as more than that is clamped down to
      // 2^31 anyway).
      if (errno == 0 && capacityLong > 0 &&
          static_cast<uint64_t>(capacityLong) <=
              static_cast<uint64_t>(INT32_MAX)) {
        capacity = PowerOfTwo32(static_cast<uint32_t>(capacityLong));
        LOG("- MOZ_BASE_PROFILER_STARTUP_ENTRIES = %u",
            unsigned(capacity.Value()));
      } else {
        LOG("- MOZ_BASE_PROFILER_STARTUP_ENTRIES not a valid integer: %s",
            startupCapacity);
        PrintUsageThenExit(1);
      }
    }

    const char* startupDuration = getenv("MOZ_BASE_PROFILER_STARTUP_DURATION");
    if (startupDuration && startupDuration[0] != '\0') {
      // TODO implement if needed
      MOZ_CRASH("MOZ_BASE_PROFILER_STARTUP_DURATION unsupported");
      // errno = 0;
      // double durationVal = PR_strtod(startupDuration, nullptr);
      // if (errno == 0 && durationVal >= 0.0) {
      //   if (durationVal > 0.0) {
      //     duration = Some(durationVal);
      //   }
      //   LOG("- MOZ_BASE_PROFILER_STARTUP_DURATION = %f", durationVal);
      // } else {
      //   LOG("- MOZ_BASE_PROFILER_STARTUP_DURATION not a valid float: %s",
      //       startupDuration);
      //   PrintUsageThenExit(1);
      // }
    }

    const char* startupInterval = getenv("MOZ_BASE_PROFILER_STARTUP_INTERVAL");
    if (startupInterval && startupInterval[0] != '\0') {
      // TODO implement if needed
      MOZ_CRASH("MOZ_BASE_PROFILER_STARTUP_INTERVAL unsupported");
      // errno = 0;
      // interval = PR_strtod(startupInterval, nullptr);
      // if (errno == 0 && interval > 0.0 && interval <= 1000.0) {
      //   LOG("- MOZ_BASE_PROFILER_STARTUP_INTERVAL = %f", interval);
      // } else {
      //   LOG("- MOZ_BASE_PROFILER_STARTUP_INTERVAL not a valid float: %s",
      //       startupInterval);
      //   PrintUsageThenExit(1);
      // }
    }

    features |= StartupExtraDefaultFeatures() & AvailableFeatures();

    const char* startupFeaturesBitfield =
        getenv("MOZ_BASE_PROFILER_STARTUP_FEATURES_BITFIELD");
    if (startupFeaturesBitfield && startupFeaturesBitfield[0] != '\0') {
      errno = 0;
      features = strtol(startupFeaturesBitfield, nullptr, 10);
      if (errno == 0 && features != 0) {
        LOG("- MOZ_BASE_PROFILER_STARTUP_FEATURES_BITFIELD = %d", features);
      } else {
        LOG("- MOZ_BASE_PROFILER_STARTUP_FEATURES_BITFIELD not a valid "
            "integer: "
            "%s",
            startupFeaturesBitfield);
        PrintUsageThenExit(1);
      }
    } else {
      const char* startupFeatures =
          getenv("MOZ_BASE_PROFILER_STARTUP_FEATURES");
      if (startupFeatures && startupFeatures[0] != '\0') {
        // Interpret startupFeatures as a list of feature strings, separated by
        // commas.
        UniquePtr<char[]> featureStringStorage;
        Vector<const char*> featureStringArray =
            SplitAtCommas(startupFeatures, featureStringStorage);
        features = ParseFeaturesFromStringArray(featureStringArray.begin(),
                                                featureStringArray.length(),
                                                /* aIsStartup */ true);
        LOG("- MOZ_BASE_PROFILER_STARTUP_FEATURES = %d", features);
      }
    }

    const char* startupFilters = getenv("MOZ_BASE_PROFILER_STARTUP_FILTERS");
    if (startupFilters && startupFilters[0] != '\0') {
      filters = SplitAtCommas(startupFilters, filterStorage);
      LOG("- MOZ_BASE_PROFILER_STARTUP_FILTERS = %s", startupFilters);
    }

    locked_profiler_start(lock, capacity, interval, features, filters.begin(),
                          filters.length(), duration);
  }

  // TODO: Install memory counter if it is possible from mozglue.
  // #if defined(MOZ_REPLACE_MALLOC) && defined(MOZ_PROFILER_MEMORY)
  //   // start counting memory allocations (outside of lock because this may
  //   call
  //   // profiler_add_sampled_counter which would attempt to take the lock.)
  //   mozilla::profiler::install_memory_counter(true);
  // #endif
}

static void locked_profiler_save_profile_to_file(PSLockRef aLock,
                                                 const char* aFilename,
                                                 bool aIsShuttingDown);

static SamplerThread* locked_profiler_stop(PSLockRef aLock);

void profiler_shutdown() {
  LOG("profiler_shutdown");

  VTUNE_SHUTDOWN();

  MOZ_RELEASE_ASSERT(CorePS::IsMainThread());
  MOZ_RELEASE_ASSERT(CorePS::Exists());

  // If the profiler is active we must get a handle to the SamplerThread before
  // ActivePS is destroyed, in order to delete it.
  SamplerThread* samplerThread = nullptr;
  {
    PSAutoLock lock;

    // Save the profile on shutdown if requested.
    if (ActivePS::Exists(lock)) {
      const char* filename = getenv("MOZ_BASE_PROFILER_SHUTDOWN");
      if (filename) {
        locked_profiler_save_profile_to_file(lock, filename,
                                             /* aIsShuttingDown */ true);
      }

      samplerThread = locked_profiler_stop(lock);
    }

    CorePS::Destroy(lock);

    // We just destroyed CorePS and the ThreadInfos it contains, so we can
    // clear this thread's TLSRegisteredThread.
    TLSRegisteredThread::SetRegisteredThread(lock, nullptr);
  }

  // We do these operations with gPSMutex unlocked. The comments in
  // profiler_stop() explain why.
  if (samplerThread) {
    delete samplerThread;
  }
}

static bool WriteProfileToJSONWriter(SpliceableChunkedJSONWriter& aWriter,
                                     double aSinceTime, bool aIsShuttingDown,
                                     bool aOnlyThreads = false) {
  LOG("WriteProfileToJSONWriter");

  MOZ_RELEASE_ASSERT(CorePS::Exists());

  if (!aOnlyThreads) {
    aWriter.Start();
    {
      if (!profiler_stream_json_for_this_process(
              aWriter, aSinceTime, aIsShuttingDown, aOnlyThreads)) {
        return false;
      }

      // Don't include profiles from other processes because this is a
      // synchronous function.
      aWriter.StartArrayProperty("processes");
      aWriter.EndArray();
    }
    aWriter.End();
  } else {
    aWriter.StartBareList();
    if (!profiler_stream_json_for_this_process(aWriter, aSinceTime,
                                               aIsShuttingDown, aOnlyThreads)) {
      return false;
    }
    aWriter.EndBareList();
  }
  return true;
}

void profiler_set_process_name(const std::string& aProcessName) {
  LOG("profiler_set_process_name(\"%s\")", aProcessName.c_str());
  PSAutoLock lock;
  CorePS::SetProcessName(lock, aProcessName);
}

UniquePtr<char[]> profiler_get_profile(double aSinceTime, bool aIsShuttingDown,
                                       bool aOnlyThreads) {
  LOG("profiler_get_profile");

  SpliceableChunkedJSONWriter b;
  if (!WriteProfileToJSONWriter(b, aSinceTime, aIsShuttingDown, aOnlyThreads)) {
    return nullptr;
  }
  return b.WriteFunc()->CopyData();
}

void profiler_get_profile_json_into_lazily_allocated_buffer(
    const std::function<char*(size_t)>& aAllocator, double aSinceTime,
    bool aIsShuttingDown) {
  LOG("profiler_get_profile_json_into_lazily_allocated_buffer");

  SpliceableChunkedJSONWriter b;
  if (!WriteProfileToJSONWriter(b, aSinceTime, aIsShuttingDown)) {
    return;
  }

  b.WriteFunc()->CopyDataIntoLazilyAllocatedBuffer(aAllocator);
}

void profiler_get_start_params(int* aCapacity, Maybe<double>* aDuration,
                               double* aInterval, uint32_t* aFeatures,
                               Vector<const char*>* aFilters) {
  MOZ_RELEASE_ASSERT(CorePS::Exists());

  if (!aCapacity || !aDuration || !aInterval || !aFeatures || !aFilters) {
    return;
  }

  PSAutoLock lock;

  if (!ActivePS::Exists(lock)) {
    *aCapacity = 0;
    *aDuration = Nothing();
    *aInterval = 0;
    *aFeatures = 0;
    aFilters->clear();
    return;
  }

  *aCapacity = ActivePS::Capacity(lock).Value();
  *aDuration = ActivePS::Duration(lock);
  *aInterval = ActivePS::Interval(lock);
  *aFeatures = ActivePS::Features(lock);

  const Vector<std::string>& filters = ActivePS::Filters(lock);
  MOZ_ALWAYS_TRUE(aFilters->resize(filters.length()));
  for (uint32_t i = 0; i < filters.length(); ++i) {
    (*aFilters)[i] = filters[i].c_str();
  }
}

void GetProfilerEnvVarsForChildProcess(
    std::function<void(const char* key, const char* value)>&& aSetEnv) {
  MOZ_RELEASE_ASSERT(CorePS::Exists());

  PSAutoLock lock;

  if (!ActivePS::Exists(lock)) {
    aSetEnv("MOZ_BASE_PROFILER_STARTUP", "");
    return;
  }

  aSetEnv("MOZ_BASE_PROFILER_STARTUP", "1");
  auto capacityString =
      Smprintf("%u", unsigned(ActivePS::Capacity(lock).Value()));
  aSetEnv("MOZ_BASE_PROFILER_STARTUP_ENTRIES", capacityString.get());

  // Use AppendFloat instead of Smprintf with %f because the decimal
  // separator used by %f is locale-dependent. But the string we produce needs
  // to be parseable by strtod, which only accepts the period character as a
  // decimal separator. AppendFloat always uses the period character.
  std::string intervalString = std::to_string(ActivePS::Interval(lock));
  aSetEnv("MOZ_BASE_PROFILER_STARTUP_INTERVAL", intervalString.c_str());

  auto featuresString = Smprintf("%d", ActivePS::Features(lock));
  aSetEnv("MOZ_BASE_PROFILER_STARTUP_FEATURES_BITFIELD", featuresString.get());

  std::string filtersString;
  const Vector<std::string>& filters = ActivePS::Filters(lock);
  for (uint32_t i = 0; i < filters.length(); ++i) {
    filtersString += filters[i];
    if (i != filters.length() - 1) {
      filtersString += ",";
    }
  }
  aSetEnv("MOZ_BASE_PROFILER_STARTUP_FILTERS", filtersString.c_str());
}

void profiler_received_exit_profile(const std::string& aExitProfile) {
  MOZ_RELEASE_ASSERT(CorePS::Exists());
  PSAutoLock lock;
  if (!ActivePS::Exists(lock)) {
    return;
  }
  ActivePS::AddExitProfile(lock, aExitProfile);
}

Vector<std::string> profiler_move_exit_profiles() {
  MOZ_RELEASE_ASSERT(CorePS::Exists());
  PSAutoLock lock;
  Vector<std::string> profiles;
  if (ActivePS::Exists(lock)) {
    profiles = ActivePS::MoveExitProfiles(lock);
  }
  return profiles;
}

static void locked_profiler_save_profile_to_file(PSLockRef aLock,
                                                 const char* aFilename,
                                                 bool aIsShuttingDown = false) {
  LOG("locked_profiler_save_profile_to_file(%s)", aFilename);

  MOZ_RELEASE_ASSERT(CorePS::Exists() && ActivePS::Exists(aLock));

  std::ofstream stream;
  stream.open(aFilename);
  if (stream.is_open()) {
    SpliceableJSONWriter w(MakeUnique<OStreamJSONWriteFunc>(stream));
    w.Start();
    {
      locked_profiler_stream_json_for_this_process(aLock, w, /* sinceTime */ 0,
                                                   aIsShuttingDown);

      w.StartArrayProperty("processes");
      Vector<std::string> exitProfiles = ActivePS::MoveExitProfiles(aLock);
      for (auto& exitProfile : exitProfiles) {
        if (!exitProfile.empty()) {
          w.Splice(exitProfile.c_str());
        }
      }
      w.EndArray();
    }
    w.End();

    stream.close();
  }
}

void profiler_save_profile_to_file(const char* aFilename) {
  LOG("profiler_save_profile_to_file(%s)", aFilename);

  MOZ_RELEASE_ASSERT(CorePS::Exists());

  PSAutoLock lock;

  if (!ActivePS::Exists(lock)) {
    return;
  }

  locked_profiler_save_profile_to_file(lock, aFilename);
}

uint32_t profiler_get_available_features() {
  MOZ_RELEASE_ASSERT(CorePS::Exists());
  return AvailableFeatures();
}

Maybe<ProfilerBufferInfo> profiler_get_buffer_info() {
  MOZ_RELEASE_ASSERT(CorePS::Exists());

  PSAutoLock lock;

  if (!ActivePS::Exists(lock)) {
    return Nothing();
  }

  return Some(ActivePS::Buffer(lock).GetProfilerBufferInfo());
}

// This basically duplicates AutoProfilerLabel's constructor.
static void* MozGlueBaseLabelEnter(const char* aLabel,
                                   const char* aDynamicString, void* aSp) {
  ProfilingStack* profilingStack = AutoProfilerLabel::sProfilingStack.get();
  if (profilingStack) {
    profilingStack->pushLabelFrame(aLabel, aDynamicString, aSp,
                                   ProfilingCategoryPair::OTHER);
  }
  return profilingStack;
}

// This basically duplicates AutoProfilerLabel's destructor.
static void MozGlueBaseLabelExit(void* sProfilingStack) {
  if (sProfilingStack) {
    reinterpret_cast<ProfilingStack*>(sProfilingStack)->pop();
  }
}

static void locked_profiler_start(PSLockRef aLock, PowerOfTwo32 aCapacity,
                                  double aInterval, uint32_t aFeatures,
                                  const char** aFilters, uint32_t aFilterCount,
                                  const Maybe<double>& aDuration) {
  if (LOG_TEST) {
    LOG("locked_profiler_start");
    LOG("- capacity  = %d", int(aCapacity.Value()));
    LOG("- duration  = %.2f", aDuration ? *aDuration : -1);
    LOG("- interval = %.2f", aInterval);

#  define LOG_FEATURE(n_, str_, Name_, desc_)     \
    if (ProfilerFeature::Has##Name_(aFeatures)) { \
      LOG("- feature  = %s", str_);               \
    }

    BASE_PROFILER_FOR_EACH_FEATURE(LOG_FEATURE)

#  undef LOG_FEATURE

    for (uint32_t i = 0; i < aFilterCount; i++) {
      LOG("- threads  = %s", aFilters[i]);
    }
  }

  MOZ_RELEASE_ASSERT(CorePS::Exists() && !ActivePS::Exists(aLock));

#  if defined(GP_PLAT_amd64_windows)
  InitializeWin64ProfilerHooks();
#  endif

  // Fall back to the default values if the passed-in values are unreasonable.
  // Less than 8192 entries (65536 bytes) may not be enough for the most complex
  // stack, so we should be able to store at least one full stack.
  // TODO: Review magic numbers.
  PowerOfTwo32 capacity =
      (aCapacity.Value() >= 8192u) ? aCapacity : BASE_PROFILER_DEFAULT_ENTRIES;
  Maybe<double> duration = aDuration;

  if (aDuration && *aDuration <= 0) {
    duration = Nothing();
  }
  double interval = aInterval > 0 ? aInterval : BASE_PROFILER_DEFAULT_INTERVAL;

  ActivePS::Create(aLock, capacity, interval, aFeatures, aFilters, aFilterCount,
                   duration);

  // Set up profiling for each registered thread, if appropriate.
  const Vector<UniquePtr<RegisteredThread>>& registeredThreads =
      CorePS::RegisteredThreads(aLock);
  for (auto& registeredThread : registeredThreads) {
    RefPtr<ThreadInfo> info = registeredThread->Info();

    if (ActivePS::ShouldProfileThread(aLock, info)) {
      registeredThread->RacyRegisteredThread().SetIsBeingProfiled(true);
      ActivePS::AddLiveProfiledThread(aLock, registeredThread.get(),
                                      MakeUnique<ProfiledThreadData>(info));
      registeredThread->RacyRegisteredThread().ReinitializeOnResume();
    }
  }

  // Setup support for pushing/popping labels in mozglue.
  RegisterProfilerLabelEnterExit(MozGlueBaseLabelEnter, MozGlueBaseLabelExit);

  // At the very end, set up RacyFeatures.
  RacyFeatures::SetActive(ActivePS::Features(aLock));
}

void profiler_start(PowerOfTwo32 aCapacity, double aInterval,
                    uint32_t aFeatures, const char** aFilters,
                    uint32_t aFilterCount, const Maybe<double>& aDuration) {
  LOG("profiler_start");

  SamplerThread* samplerThread = nullptr;
  {
    PSAutoLock lock;

    // Initialize if necessary.
    if (!CorePS::Exists()) {
      profiler_init(nullptr);
    }

    // Reset the current state if the profiler is running.
    if (ActivePS::Exists(lock)) {
      samplerThread = locked_profiler_stop(lock);
    }

    locked_profiler_start(lock, aCapacity, aInterval, aFeatures, aFilters,
                          aFilterCount, aDuration);
  }

  // TODO: Install memory counter if it is possible from mozglue.
  // #if defined(MOZ_REPLACE_MALLOC) && defined(MOZ_PROFILER_MEMORY)
  //   // start counting memory allocations (outside of lock because this may
  //   call
  //   // profiler_add_sampled_counter which would attempt to take the lock.)
  //   mozilla::profiler::install_memory_counter(true);
  // #endif

  // We do these operations with gPSMutex unlocked. The comments in
  // profiler_stop() explain why.
  if (samplerThread) {
    delete samplerThread;
  }
}

void profiler_ensure_started(PowerOfTwo32 aCapacity, double aInterval,
                             uint32_t aFeatures, const char** aFilters,
                             uint32_t aFilterCount,
                             const Maybe<double>& aDuration) {
  LOG("profiler_ensure_started");

  // bool startedProfiler = false; (See TODO below)
  SamplerThread* samplerThread = nullptr;
  {
    PSAutoLock lock;

    // Initialize if necessary.
    if (!CorePS::Exists()) {
      profiler_init(nullptr);
    }

    if (ActivePS::Exists(lock)) {
      // The profiler is active.
      if (!ActivePS::Equals(lock, aCapacity, aDuration, aInterval, aFeatures,
                            aFilters, aFilterCount)) {
        // Stop and restart with different settings.
        samplerThread = locked_profiler_stop(lock);
        locked_profiler_start(lock, aCapacity, aInterval, aFeatures, aFilters,
                              aFilterCount, aDuration);
        // startedProfiler = true; (See TODO below)
      }
    } else {
      // The profiler is stopped.
      locked_profiler_start(lock, aCapacity, aInterval, aFeatures, aFilters,
                            aFilterCount, aDuration);
      // startedProfiler = true; (See TODO below)
    }
  }

  // TODO: Install memory counter if it is possible from mozglue.
  // #if defined(MOZ_REPLACE_MALLOC) && defined(MOZ_PROFILER_MEMORY)
  //   // start counting memory allocations (outside of lock because this may
  //   // call profiler_add_sampled_counter which would attempt to take the
  //   // lock.)
  //   mozilla::profiler::install_memory_counter(true);
  // #endif

  // We do these operations with gPSMutex unlocked. The comments in
  // profiler_stop() explain why.
  if (samplerThread) {
    delete samplerThread;
  }
}

static MOZ_MUST_USE SamplerThread* locked_profiler_stop(PSLockRef aLock) {
  LOG("locked_profiler_stop");

  MOZ_RELEASE_ASSERT(CorePS::Exists() && ActivePS::Exists(aLock));

  // At the very start, clear RacyFeatures.
  RacyFeatures::SetInactive();

  // TODO: Uninstall memory counter if it is possible from mozglue.
  // #if defined(MOZ_REPLACE_MALLOC) && defined(MOZ_PROFILER_MEMORY)
  //   mozilla::profiler::install_memory_counter(false);
  // #endif

  // Remove support for pushing/popping labels in mozglue.
  RegisterProfilerLabelEnterExit(nullptr, nullptr);

  // Stop sampling live threads.
  const Vector<LiveProfiledThreadData>& liveProfiledThreads =
      ActivePS::LiveProfiledThreads(aLock);
  for (auto& thread : liveProfiledThreads) {
    RegisteredThread* registeredThread = thread.mRegisteredThread;
    registeredThread->RacyRegisteredThread().SetIsBeingProfiled(false);
  }

  // The Stop() call doesn't actually stop Run(); that happens in this
  // function's caller when the sampler thread is destroyed. Stop() just gives
  // the SamplerThread a chance to do some cleanup with gPSMutex locked.
  SamplerThread* samplerThread = ActivePS::Destroy(aLock);
  samplerThread->Stop(aLock);

  return samplerThread;
}

void profiler_stop() {
  LOG("profiler_stop");

  MOZ_RELEASE_ASSERT(CorePS::Exists());

  SamplerThread* samplerThread;
  {
    PSAutoLock lock;

    if (!ActivePS::Exists(lock)) {
      return;
    }

    samplerThread = locked_profiler_stop(lock);
  }

  // We delete with gPSMutex unlocked. Otherwise we would get a deadlock: we
  // would be waiting here with gPSMutex locked for SamplerThread::Run() to
  // return so the join operation within the destructor can complete, but Run()
  // needs to lock gPSMutex to return.
  //
  // Because this call occurs with gPSMutex unlocked, it -- including the final
  // iteration of Run()'s loop -- must be able detect deactivation and return
  // in a way that's safe with respect to other gPSMutex-locking operations
  // that may have occurred in the meantime.
  delete samplerThread;
}

bool profiler_is_paused() {
  MOZ_RELEASE_ASSERT(CorePS::Exists());

  PSAutoLock lock;

  if (!ActivePS::Exists(lock)) {
    return false;
  }

  return ActivePS::IsPaused(lock);
}

void profiler_pause() {
  LOG("profiler_pause");

  MOZ_RELEASE_ASSERT(CorePS::Exists());

  {
    PSAutoLock lock;

    if (!ActivePS::Exists(lock)) {
      return;
    }

    RacyFeatures::SetPaused();
    ActivePS::SetIsPaused(lock, true);
    ActivePS::Buffer(lock).AddEntry(ProfileBufferEntry::Pause(profiler_time()));
  }
}

void profiler_resume() {
  LOG("profiler_resume");

  MOZ_RELEASE_ASSERT(CorePS::Exists());

  {
    PSAutoLock lock;

    if (!ActivePS::Exists(lock)) {
      return;
    }

    ActivePS::Buffer(lock).AddEntry(
        ProfileBufferEntry::Resume(profiler_time()));
    ActivePS::SetIsPaused(lock, false);
    RacyFeatures::SetUnpaused();
  }
}

bool profiler_feature_active(uint32_t aFeature) {
  // This function runs both on and off the main thread.

  MOZ_RELEASE_ASSERT(CorePS::Exists());

  // This function is hot enough that we use RacyFeatures, not ActivePS.
  return RacyFeatures::IsActiveWithFeature(aFeature);
}

void profiler_add_sampled_counter(BaseProfilerCount* aCounter) {
  DEBUG_LOG("profiler_add_sampled_counter(%s)", aCounter->mLabel);
  PSAutoLock lock;
  CorePS::AppendCounter(lock, aCounter);
}

void profiler_remove_sampled_counter(BaseProfilerCount* aCounter) {
  DEBUG_LOG("profiler_remove_sampled_counter(%s)", aCounter->mLabel);
  PSAutoLock lock;
  // Note: we don't enforce a final sample, though we could do so if the
  // profiler was active
  CorePS::RemoveCounter(lock, aCounter);
}

ProfilingStack* profiler_register_thread(const char* aName,
                                         void* aGuessStackTop) {
  DEBUG_LOG("profiler_register_thread(%s)", aName);

  MOZ_RELEASE_ASSERT(CorePS::Exists());

  PSAutoLock lock;

  void* stackTop = GetStackTop(aGuessStackTop);
  return locked_register_thread(lock, aName, stackTop);
}

void profiler_unregister_thread() {
  if (!CorePS::Exists()) {
    // This function can be called after the main thread has already shut down.
    return;
  }

  PSAutoLock lock;

  RegisteredThread* registeredThread = FindCurrentThreadRegisteredThread(lock);
  MOZ_RELEASE_ASSERT(registeredThread ==
                     TLSRegisteredThread::RegisteredThread(lock));
  if (registeredThread) {
    RefPtr<ThreadInfo> info = registeredThread->Info();

    DEBUG_LOG("profiler_unregister_thread: %s", info->Name());

    if (ActivePS::Exists(lock)) {
      ActivePS::UnregisterThread(lock, registeredThread);
    }

    // Clear the pointer to the RegisteredThread object that we're about to
    // destroy.
    TLSRegisteredThread::SetRegisteredThread(lock, nullptr);

    // Remove the thread from the list of registered threads. This deletes the
    // registeredThread object.
    CorePS::RemoveRegisteredThread(lock, registeredThread);
  } else {
    // There are two ways FindCurrentThreadRegisteredThread() might have failed.
    //
    // - TLSRegisteredThread::Init() failed in locked_register_thread().
    //
    // - We've already called profiler_unregister_thread() for this thread.
    //   (Whether or not it should, this does happen in practice.)
    //
    // Either way, TLSRegisteredThread should be empty.
    MOZ_RELEASE_ASSERT(!TLSRegisteredThread::RegisteredThread(lock));
  }
}

void profiler_register_page(uint64_t aBrowsingContextID,
                            uint64_t aInnerWindowID, const std::string& aUrl,
                            uint64_t aEmbedderInnerWindowID) {
  DEBUG_LOG("profiler_register_page(%" PRIu64 ", %" PRIu64 ", %s, %" PRIu64 ")",
            aBrowsingContextID, aInnerWindowID, aUrl.c_str(),
            aEmbedderInnerWindowID);

  MOZ_RELEASE_ASSERT(CorePS::Exists());

  PSAutoLock lock;

  // When a Browsing context is first loaded, the first url loaded in it will be
  // about:blank. Because of that, this call keeps the first non-about:blank
  // registration of window and discards the previous one.
  RefPtr<PageInformation> pageInfo = new PageInformation(
      aBrowsingContextID, aInnerWindowID, aUrl, aEmbedderInnerWindowID);
  CorePS::AppendRegisteredPage(lock, std::move(pageInfo));

  // After appending the given page to CorePS, look for the expired
  // pages and remove them if there are any.
  if (ActivePS::Exists(lock)) {
    ActivePS::DiscardExpiredPages(lock);
  }
}

void profiler_unregister_page(uint64_t aRegisteredInnerWindowID) {
  if (!CorePS::Exists()) {
    // This function can be called after the main thread has already shut down.
    return;
  }

  PSAutoLock lock;

  // During unregistration, if the profiler is active, we have to keep the
  // page information since there may be some markers associated with the given
  // page. But if profiler is not active. we have no reason to keep the
  // page information here because there can't be any marker associated with it.
  if (ActivePS::Exists(lock)) {
    ActivePS::UnregisterPage(lock, aRegisteredInnerWindowID);
  } else {
    CorePS::RemoveRegisteredPage(lock, aRegisteredInnerWindowID);
  }
}

void profiler_clear_all_pages() {
  if (!CorePS::Exists()) {
    // This function can be called after the main thread has already shut down.
    return;
  }

  {
    PSAutoLock lock;
    CorePS::ClearRegisteredPages(lock);
    if (ActivePS::Exists(lock)) {
      ActivePS::ClearUnregisteredPages(lock);
    }
  }
}

void profiler_thread_sleep() {
  // This function runs both on and off the main thread.

  MOZ_RELEASE_ASSERT(CorePS::Exists());

  RacyRegisteredThread* racyRegisteredThread =
      TLSRegisteredThread::RacyRegisteredThread();
  if (!racyRegisteredThread) {
    return;
  }

  racyRegisteredThread->SetSleeping();
}

void profiler_thread_wake() {
  // This function runs both on and off the main thread.

  MOZ_RELEASE_ASSERT(CorePS::Exists());

  RacyRegisteredThread* racyRegisteredThread =
      TLSRegisteredThread::RacyRegisteredThread();
  if (!racyRegisteredThread) {
    return;
  }

  racyRegisteredThread->SetAwake();
}

bool detail::IsThreadBeingProfiled() {
  MOZ_RELEASE_ASSERT(CorePS::Exists());

  const RacyRegisteredThread* racyRegisteredThread =
      TLSRegisteredThread::RacyRegisteredThread();
  return racyRegisteredThread && racyRegisteredThread->IsBeingProfiled();
}

bool profiler_thread_is_sleeping() {
  MOZ_RELEASE_ASSERT(CorePS::IsMainThread());
  MOZ_RELEASE_ASSERT(CorePS::Exists());

  RacyRegisteredThread* racyRegisteredThread =
      TLSRegisteredThread::RacyRegisteredThread();
  if (!racyRegisteredThread) {
    return false;
  }
  return racyRegisteredThread->IsSleeping();
}

double profiler_time() {
  MOZ_RELEASE_ASSERT(CorePS::Exists());

  TimeDuration delta = TimeStamp::NowUnfuzzed() - CorePS::ProcessStartTime();
  return delta.ToMilliseconds();
}

UniqueProfilerBacktrace profiler_get_backtrace() {
  MOZ_RELEASE_ASSERT(CorePS::Exists());

  PSAutoLock lock;

  if (!ActivePS::Exists(lock) || ActivePS::FeaturePrivacy(lock)) {
    return nullptr;
  }

  RegisteredThread* registeredThread =
      TLSRegisteredThread::RegisteredThread(lock);
  if (!registeredThread) {
    MOZ_ASSERT(registeredThread);
    return nullptr;
  }

  int tid = profiler_current_thread_id();

  TimeStamp now = TimeStamp::NowUnfuzzed();

  Registers regs;
#  if defined(HAVE_NATIVE_UNWIND)
  regs.SyncPopulate();
#  else
  regs.Clear();
#  endif

  // 65536 bytes should be plenty for a single backtrace.
  auto bufferManager = MakeUnique<BlocksRingBuffer>(
      BlocksRingBuffer::ThreadSafety::WithoutMutex);
  auto buffer =
      MakeUnique<ProfileBuffer>(*bufferManager, MakePowerOfTwo32<65536>());

  DoSyncSample(lock, *registeredThread, now, regs, *buffer.get());

  return UniqueProfilerBacktrace(new ProfilerBacktrace(
      "SyncProfile", tid, std::move(bufferManager), std::move(buffer)));
}

void ProfilerBacktraceDestructor::operator()(ProfilerBacktrace* aBacktrace) {
  delete aBacktrace;
}

static void racy_profiler_add_marker(const char* aMarkerName,
                                     ProfilingCategoryPair aCategoryPair,
                                     const ProfilerMarkerPayload* aPayload) {
  MOZ_RELEASE_ASSERT(CorePS::Exists());

  // This function is hot enough that we use RacyFeatures, not ActivePS.
  if (!profiler_can_accept_markers()) {
    return;
  }

  // Note that it's possible that the above test would change again before we
  // actually record the marker. Because of this imprecision it's possible to
  // miss a marker or record one we shouldn't. Either way is not a big deal.

  RacyRegisteredThread* racyRegisteredThread =
      TLSRegisteredThread::RacyRegisteredThread();
  if (!racyRegisteredThread || !racyRegisteredThread->IsBeingProfiled()) {
    return;
  }

  TimeStamp origin = (aPayload && !aPayload->GetStartTime().IsNull())
                         ? aPayload->GetStartTime()
                         : TimeStamp::NowUnfuzzed();
  TimeDuration delta = origin - CorePS::ProcessStartTime();
  CorePS::CoreBlocksRingBuffer().PutObjects(
      ProfileBufferEntry::Kind::MarkerData, racyRegisteredThread->ThreadId(),
      WrapBlocksRingBufferUnownedCString(aMarkerName),
      static_cast<uint32_t>(aCategoryPair), aPayload, delta.ToMilliseconds());
}

void profiler_add_marker(const char* aMarkerName,
                         ProfilingCategoryPair aCategoryPair,
                         const ProfilerMarkerPayload& aPayload) {
  racy_profiler_add_marker(aMarkerName, aCategoryPair, &aPayload);
}

void profiler_add_marker(const char* aMarkerName,
                         ProfilingCategoryPair aCategoryPair) {
  racy_profiler_add_marker(aMarkerName, aCategoryPair, nullptr);
}

// This is a simplified version of profiler_add_marker that can be easily passed
// into the JS engine.
void profiler_add_js_marker(const char* aMarkerName) {
  AUTO_PROFILER_STATS(base_add_marker);
  profiler_add_marker(aMarkerName, ProfilingCategoryPair::JS);
}

// This logic needs to add a marker for a different thread, so we actually need
// to lock here.
void profiler_add_marker_for_thread(int aThreadId,
                                    ProfilingCategoryPair aCategoryPair,
                                    const char* aMarkerName,
                                    UniquePtr<ProfilerMarkerPayload> aPayload) {
  MOZ_RELEASE_ASSERT(CorePS::Exists());

  if (!profiler_can_accept_markers()) {
    return;
  }

#  ifdef DEBUG
  {
    PSAutoLock lock;
    if (!ActivePS::Exists(lock)) {
      return;
    }

    // Assert that our thread ID makes sense
    bool realThread = false;
    const Vector<UniquePtr<RegisteredThread>>& registeredThreads =
        CorePS::RegisteredThreads(lock);
    for (auto& thread : registeredThreads) {
      RefPtr<ThreadInfo> info = thread->Info();
      if (info->ThreadId() == aThreadId) {
        realThread = true;
        break;
      }
    }
    MOZ_ASSERT(realThread, "Invalid thread id");
  }
#  endif

  // Insert the marker into the buffer
  TimeStamp origin = (aPayload && !aPayload->GetStartTime().IsNull())
                         ? aPayload->GetStartTime()
                         : TimeStamp::NowUnfuzzed();
  TimeDuration delta = origin - CorePS::ProcessStartTime();
  CorePS::CoreBlocksRingBuffer().PutObjects(
      ProfileBufferEntry::Kind::MarkerData, aThreadId,
      WrapBlocksRingBufferUnownedCString(aMarkerName),
      static_cast<uint32_t>(aCategoryPair), aPayload, delta.ToMilliseconds());
}

void profiler_tracing_marker(const char* aCategoryString,
                             const char* aMarkerName,
                             ProfilingCategoryPair aCategoryPair,
                             TracingKind aKind,
                             const Maybe<uint64_t>& aInnerWindowID) {
  MOZ_RELEASE_ASSERT(CorePS::Exists());

  VTUNE_TRACING(aMarkerName, aKind);

  // This function is hot enough that we use RacyFeatures, notActivePS.
  if (!profiler_can_accept_markers()) {
    return;
  }

  AUTO_PROFILER_STATS(base_add_marker_with_TracingMarkerPayload);
  profiler_add_marker(
      aMarkerName, aCategoryPair,
      TracingMarkerPayload(aCategoryString, aKind, aInnerWindowID));
}

void profiler_tracing_marker(const char* aCategoryString,
                             const char* aMarkerName,
                             ProfilingCategoryPair aCategoryPair,
                             TracingKind aKind, UniqueProfilerBacktrace aCause,
                             const Maybe<uint64_t>& aInnerWindowID) {
  MOZ_RELEASE_ASSERT(CorePS::Exists());

  VTUNE_TRACING(aMarkerName, aKind);

  // This function is hot enough that we use RacyFeatures, notActivePS.
  if (!profiler_can_accept_markers()) {
    return;
  }

  AUTO_PROFILER_STATS(base_add_marker_with_TracingMarkerPayload);
  profiler_add_marker(aMarkerName, aCategoryPair,
                      TracingMarkerPayload(aCategoryString, aKind,
                                           aInnerWindowID, std::move(aCause)));
}

void profiler_add_text_marker(const char* aMarkerName, const std::string& aText,
                              ProfilingCategoryPair aCategoryPair,
                              const TimeStamp& aStartTime,
                              const TimeStamp& aEndTime,
                              const Maybe<uint64_t>& aInnerWindowID,
                              UniqueProfilerBacktrace aCause) {
  AUTO_PROFILER_STATS(base_add_marker_with_TextMarkerPayload);
  profiler_add_marker(aMarkerName, aCategoryPair,
                      TextMarkerPayload(aText, aStartTime, aEndTime,
                                        aInnerWindowID, std::move(aCause)));
}

// NOTE: aCollector's methods will be called while the target thread is paused.
// Doing things in those methods like allocating -- which may try to claim
// locks -- is a surefire way to deadlock.
void profiler_suspend_and_sample_thread(int aThreadId, uint32_t aFeatures,
                                        ProfilerStackCollector& aCollector,
                                        bool aSampleNative /* = true */) {
  // Lock the profiler mutex
  PSAutoLock lock;

  const Vector<UniquePtr<RegisteredThread>>& registeredThreads =
      CorePS::RegisteredThreads(lock);
  for (auto& thread : registeredThreads) {
    RefPtr<ThreadInfo> info = thread->Info();
    RegisteredThread& registeredThread = *thread.get();

    if (info->ThreadId() == aThreadId) {
      if (info->IsMainThread()) {
        aCollector.SetIsMainThread();
      }

      // Allocate the space for the native stack
      NativeStack nativeStack;

      // Suspend, sample, and then resume the target thread.
      Sampler sampler(lock);
      TimeStamp now = TimeStamp::NowUnfuzzed();
      sampler.SuspendAndSampleAndResumeThread(
          lock, registeredThread, now,
          [&](const Registers& aRegs, const TimeStamp& aNow) {
            // The target thread is now suspended. Collect a native
            // backtrace, and call the callback.
            bool isSynchronous = false;
#  if defined(HAVE_FASTINIT_NATIVE_UNWIND)
            if (aSampleNative) {
          // We can only use FramePointerStackWalk or MozStackWalk from
          // suspend_and_sample_thread as other stackwalking methods may not be
          // initialized.
#    if defined(USE_FRAME_POINTER_STACK_WALK)
              DoFramePointerBacktrace(lock, registeredThread, aRegs,
                                      nativeStack);
#    elif defined(USE_MOZ_STACK_WALK)
              DoMozStackWalkBacktrace(lock, registeredThread, aRegs,
                                      nativeStack);
#    else
#      error "Invalid configuration"
#    endif

              MergeStacks(aFeatures, isSynchronous, registeredThread, aRegs,
                          nativeStack, aCollector);
            } else
#  endif
            {
              MergeStacks(aFeatures, isSynchronous, registeredThread, aRegs,
                          nativeStack, aCollector);

              if (ProfilerFeature::HasLeaf(aFeatures)) {
                aCollector.CollectNativeLeafAddr((void*)aRegs.mPC);
              }
            }
          });

      // NOTE: Make sure to disable the sampler before it is destroyed, in case
      // the profiler is running at the same time.
      sampler.Disable(lock);
      break;
    }
  }
}

// END externally visible functions
////////////////////////////////////////////////////////////////////////

}  // namespace baseprofiler
}  // namespace mozilla

#endif  // MOZ_BASE_PROFILER
