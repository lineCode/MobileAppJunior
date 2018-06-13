/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef MEDIAENGINEWEBRTC_H_
#define MEDIAENGINEWEBRTC_H_

#include "prcvar.h"
#include "prthread.h"
#include "nsIThread.h"
#include "nsIRunnable.h"

#include "mozilla/dom/File.h"
#include "mozilla/Mutex.h"
#include "mozilla/StaticMutex.h"
#include "mozilla/Monitor.h"
#include "mozilla/Sprintf.h"
#include "mozilla/UniquePtr.h"
#include "nsAutoPtr.h"
#include "nsCOMPtr.h"
#include "nsThreadUtils.h"
#include "DOMMediaStream.h"
#include "nsDirectoryServiceDefs.h"
#include "nsComponentManagerUtils.h"
#include "nsRefPtrHashtable.h"

#include "ipc/IPCMessageUtils.h"
#include "VideoUtils.h"
#include "MediaEngineCameraVideoSource.h"
#include "VideoSegment.h"
#include "AudioSegment.h"
#include "StreamTracks.h"
#include "MediaStreamGraph.h"
#include "cubeb/cubeb.h"
#include "CubebUtils.h"
#include "AudioPacketizer.h"

#include "MediaEngineWrapper.h"
#include "mozilla/dom/MediaStreamTrackBinding.h"
#include "CamerasChild.h"

// WebRTC library includes follow
// Audio Engine
#include "webrtc/voice_engine/include/voe_base.h"
#include "webrtc/voice_engine/include/voe_codec.h"
#include "webrtc/voice_engine/include/voe_network.h"
#include "webrtc/voice_engine/include/voe_audio_processing.h"
#include "webrtc/voice_engine/include/voe_volume_control.h"
#include "webrtc/voice_engine/include/voe_external_media.h"
#include "webrtc/voice_engine/include/voe_audio_processing.h"
#include "webrtc/modules/audio_device/include/audio_device.h"
#include "webrtc/modules/audio_processing/include/audio_processing.h"

// Video Engine
// conflicts with #include of scoped_ptr.h
#undef FF

// WebRTC imports
#include "webrtc/modules/video_capture/video_capture_defines.h"

#include "NullTransport.h"

namespace mozilla {

class MediaEngineWebRTCAudioCaptureSource : public MediaEngineAudioSource
{
public:
  NS_DECL_THREADSAFE_ISUPPORTS

  explicit MediaEngineWebRTCAudioCaptureSource(const char* aUuid)
    : MediaEngineAudioSource(kReleased)
  {
  }
  void GetName(nsAString& aName) const override;
  void GetUUID(nsACString& aUUID) const override;
  nsresult Allocate(const dom::MediaTrackConstraints& aConstraints,
                    const MediaEnginePrefs& aPrefs,
                    const nsString& aDeviceId,
                    const mozilla::ipc::PrincipalInfo& aPrincipalInfo,
                    AllocationHandle** aOutHandle,
                    const char** aOutBadConstraint) override
  {
    // Nothing to do here, everything is managed in MediaManager.cpp
    *aOutHandle = nullptr;
    return NS_OK;
  }
  nsresult Deallocate(AllocationHandle* aHandle) override
  {
    // Nothing to do here, everything is managed in MediaManager.cpp
    MOZ_ASSERT(!aHandle);
    return NS_OK;
  }
  nsresult Start(SourceMediaStream* aMediaStream,
                 TrackID aId,
                 const PrincipalHandle& aPrincipalHandle) override;
  nsresult Stop(SourceMediaStream* aMediaStream, TrackID aId) override;
  nsresult Restart(AllocationHandle* aHandle,
                   const dom::MediaTrackConstraints& aConstraints,
                   const MediaEnginePrefs &aPrefs,
                   const nsString& aDeviceId,
                   const char** aOutBadConstraint) override;
  void NotifyOutputData(MediaStreamGraph* aGraph,
                        AudioDataValue* aBuffer, size_t aFrames,
                        TrackRate aRate, uint32_t aChannels) override
  {}
  void DeviceChanged() override
  {}
  void NotifyInputData(MediaStreamGraph* aGraph,
                       const AudioDataValue* aBuffer, size_t aFrames,
                       TrackRate aRate, uint32_t aChannels) override
  {}
  void NotifyPull(MediaStreamGraph* aGraph,
                  SourceMediaStream* aSource,
                  TrackID aID,
                  StreamTime aDesiredTime,
                  const PrincipalHandle& aPrincipalHandle) override
  {}
  dom::MediaSourceEnum GetMediaSource() const override
  {
    return dom::MediaSourceEnum::AudioCapture;
  }
  bool IsFake() override
  {
    return false;
  }
  nsresult TakePhoto(MediaEnginePhotoCallback* aCallback) override
  {
    return NS_ERROR_NOT_IMPLEMENTED;
  }
  uint32_t GetBestFitnessDistance(
    const nsTArray<const NormalizedConstraintSet*>& aConstraintSets,
    const nsString& aDeviceId) const override;

protected:
  virtual ~MediaEngineWebRTCAudioCaptureSource() {}
  nsCString mUUID;
};

// Small subset of VoEHardware
class AudioInput
{
public:
  AudioInput() = default;
  // Threadsafe because it's referenced from an MicrophoneSource, which can
  // had references to it on other threads.
  NS_INLINE_DECL_THREADSAFE_REFCOUNTING(AudioInput)

  virtual int GetNumOfRecordingDevices(int& aDevices) = 0;
  virtual int GetRecordingDeviceName(int aIndex, char (&aStrNameUTF8)[128],
                                     char aStrGuidUTF8[128]) = 0;
  virtual int GetRecordingDeviceStatus(bool& aIsAvailable) = 0;
  virtual void GetChannelCount(uint32_t& aChannels) = 0;
  virtual int GetMaxAvailableChannels(uint32_t& aChannels) = 0;
  virtual void StartRecording(SourceMediaStream *aStream, AudioDataListener *aListener) = 0;
  virtual void StopRecording(SourceMediaStream *aStream) = 0;
  virtual int SetRecordingDevice(int aIndex) = 0;
  virtual void SetUserChannelCount(uint32_t aChannels) = 0;

protected:
  // Protected destructor, to discourage deletion outside of Release():
  virtual ~AudioInput() {}
};

class AudioInputCubeb final : public AudioInput
{
public:
  explicit AudioInputCubeb(int aIndex = 0) :
    AudioInput(), mSelectedDevice(aIndex), mInUseCount(0)
  {
    if (!mDeviceIndexes) {
      mDeviceIndexes = new nsTArray<int>;
      mDeviceNames = new nsTArray<nsCString>;
      mDefaultDevice = -1;
    }
  }

  static void CleanupGlobalData()
  {
    cubeb_device_collection_destroy(CubebUtils::GetCubebContext(), &mDevices);
    delete mDeviceIndexes;
    mDeviceIndexes = nullptr;
    delete mDeviceNames;
    mDeviceNames = nullptr;
  }

  int GetNumOfRecordingDevices(int& aDevices)
  {
#ifdef MOZ_WIDGET_ANDROID
    // OpenSL ES does not support enumerate device.
    aDevices = 1;
#else
    UpdateDeviceList();
    aDevices = mDeviceIndexes->Length();
#endif
    return 0;
  }

  static int32_t DeviceIndex(int aIndex)
  {
    // -1 = system default if any
    if (aIndex == -1) {
      if (mDefaultDevice == -1) {
        aIndex = 0;
      } else {
        aIndex = mDefaultDevice;
      }
    }
    MOZ_ASSERT(mDeviceIndexes);
    if (aIndex < 0 || aIndex >= (int) mDeviceIndexes->Length()) {
      return -1;
    }
    // Note: if the device is gone, this will be -1
    return (*mDeviceIndexes)[aIndex]; // translate to mDevices index
  }

  static StaticMutex& Mutex()
  {
    return sMutex;
  }

  static bool GetDeviceID(int aDeviceIndex, CubebUtils::AudioDeviceID &aID)
  {
    // Assert sMutex is held
    sMutex.AssertCurrentThreadOwns();
#ifdef MOZ_WIDGET_ANDROID
    aID = nullptr;
    return true;
#else
    int dev_index = DeviceIndex(aDeviceIndex);
    if (dev_index != -1) {
      aID = mDevices.device[dev_index].devid;
      return true;
    }
    return false;
#endif
  }

  int GetRecordingDeviceName(int aIndex, char (&aStrNameUTF8)[128],
                             char aStrGuidUTF8[128])
  {
#ifdef MOZ_WIDGET_ANDROID
    aStrNameUTF8[0] = '\0';
    aStrGuidUTF8[0] = '\0';
#else
    int32_t devindex = DeviceIndex(aIndex);
    if (mDevices.count == 0 || devindex < 0) {
      return 1;
    }
    SprintfLiteral(aStrNameUTF8, "%s%s", aIndex == -1 ? "default: " : "",
                   mDevices.device[devindex].friendly_name);
    aStrGuidUTF8[0] = '\0';
#endif
    return 0;
  }

  int GetRecordingDeviceStatus(bool& aIsAvailable)
  {
    // With cubeb, we only expose devices of type CUBEB_DEVICE_TYPE_INPUT,
    // so unless it was removed, say it's available
    aIsAvailable = true;
    return 0;
  }

  void GetChannelCount(uint32_t& aChannels)
  {
    GetUserChannelCount(mSelectedDevice, aChannels);
  }

  static void GetUserChannelCount(int aDeviceIndex, uint32_t& aChannels)
  {
    aChannels = sUserChannelCount;
  }

  int GetMaxAvailableChannels(uint32_t& aChannels)
  {
    return GetDeviceMaxChannels(mSelectedDevice, aChannels);
  }

  static int GetDeviceMaxChannels(int aDeviceIndex, uint32_t& aChannels)
  {
#ifdef MOZ_WIDGET_ANDROID
    aChannels = 1;
#else
    int32_t devindex = DeviceIndex(aDeviceIndex);
    if (mDevices.count == 0 || devindex < 0) {
      return 1;
    }
    aChannels = mDevices.device[devindex].max_channels;
#endif
    return 0;
  }

  void SetUserChannelCount(uint32_t aChannels)
  {
    if (GetDeviceMaxChannels(mSelectedDevice, sUserChannelCount)) {
      sUserChannelCount = 1; // error capture mono
      return;
    }

    if (aChannels && aChannels < sUserChannelCount) {
      sUserChannelCount = aChannels;
    }
  }

  void StartRecording(SourceMediaStream *aStream, AudioDataListener *aListener)
  {
#ifdef MOZ_WIDGET_ANDROID
    // OpenSL ES does not support enumerating devices.
    MOZ_ASSERT(mDevices.count == 0);
#else
    MOZ_ASSERT(mDevices.count > 0);
#endif

    mAnyInUse = true;
    mInUseCount++;
    // Always tell the stream we're using it for input
    aStream->OpenAudioInput(mSelectedDevice, aListener);
  }

  void StopRecording(SourceMediaStream *aStream)
  {
    aStream->CloseAudioInput();
    if (--mInUseCount == 0) {
      mAnyInUse = false;
    }
  }

  int SetRecordingDevice(int aIndex)
  {
    mSelectedDevice = aIndex;
    return 0;
  }

protected:
  ~AudioInputCubeb() {
    MOZ_RELEASE_ASSERT(mInUseCount == 0);
  }

private:
  // It would be better to watch for device-change notifications
  void UpdateDeviceList();

  // We have an array, which consists of indexes to the current mDevices
  // list.  This is updated on mDevices updates.  Many devices in mDevices
  // won't be included in the array (wrong type, etc), or if a device is
  // removed it will map to -1 (and opens of this device will need to check
  // for this - and be careful of threading access.  The mappings need to
  // updated on each re-enumeration.
  int mSelectedDevice;
  uint32_t mInUseCount;

  // pointers to avoid static constructors
  static nsTArray<int>* mDeviceIndexes;
  static int mDefaultDevice; // -1 == not set
  static nsTArray<nsCString>* mDeviceNames;
  static cubeb_device_collection mDevices;
  static bool mAnyInUse;
  static StaticMutex sMutex;
  static uint32_t sUserChannelCount;
};

class WebRTCAudioDataListener : public AudioDataListener
{
protected:
  // Protected destructor, to discourage deletion outside of Release():
  virtual ~WebRTCAudioDataListener() {}

public:
  explicit WebRTCAudioDataListener(MediaEngineAudioSource* aAudioSource)
    : mMutex("WebRTCAudioDataListener")
    , mAudioSource(aAudioSource)
  {}

  // AudioDataListenerInterface methods
  virtual void NotifyOutputData(MediaStreamGraph* aGraph,
                                AudioDataValue* aBuffer, size_t aFrames,
                                TrackRate aRate, uint32_t aChannels) override
  {
    MutexAutoLock lock(mMutex);
    if (mAudioSource) {
      mAudioSource->NotifyOutputData(aGraph, aBuffer, aFrames, aRate, aChannels);
    }
  }
  virtual void NotifyInputData(MediaStreamGraph* aGraph,
                               const AudioDataValue* aBuffer, size_t aFrames,
                               TrackRate aRate, uint32_t aChannels) override
  {
    MutexAutoLock lock(mMutex);
    if (mAudioSource) {
      mAudioSource->NotifyInputData(aGraph, aBuffer, aFrames, aRate, aChannels);
    }
  }
  virtual void DeviceChanged() override
  {
    MutexAutoLock lock(mMutex);
    if (mAudioSource) {
      mAudioSource->DeviceChanged();
    }
  }

  void Shutdown()
  {
    MutexAutoLock lock(mMutex);
    mAudioSource = nullptr;
  }

private:
  Mutex mMutex;
  RefPtr<MediaEngineAudioSource> mAudioSource;
};

class MediaEngineWebRTCMicrophoneSource : public MediaEngineAudioSource
{
  typedef MediaEngineAudioSource Super;
public:
  MediaEngineWebRTCMicrophoneSource(mozilla::AudioInput* aAudioInput,
                                    int aIndex,
                                    const char* name,
                                    const char* uuid,
                                    bool aDelayAgnostic,
                                    bool aExtendedFilter);

  void GetName(nsAString& aName) const override;
  void GetUUID(nsACString& aUUID) const override;

  nsresult Deallocate(AllocationHandle* aHandle) override;
  nsresult Start(SourceMediaStream* aStream,
                 TrackID aID,
                 const PrincipalHandle& aPrincipalHandle) override;
  nsresult Stop(SourceMediaStream* aSource, TrackID aID) override;
  nsresult Restart(AllocationHandle* aHandle,
                   const dom::MediaTrackConstraints& aConstraints,
                   const MediaEnginePrefs &aPrefs,
                   const nsString& aDeviceId,
                   const char** aOutBadConstraint) override;

  void NotifyPull(MediaStreamGraph* aGraph,
                  SourceMediaStream* aSource,
                  TrackID aId,
                  StreamTime aDesiredTime,
                  const PrincipalHandle& aPrincipalHandle) override;

  // AudioDataListenerInterface methods
  void NotifyOutputData(MediaStreamGraph* aGraph,
                        AudioDataValue* aBuffer, size_t aFrames,
                        TrackRate aRate, uint32_t aChannels) override;
  void NotifyInputData(MediaStreamGraph* aGraph,
                       const AudioDataValue* aBuffer, size_t aFrames,
                       TrackRate aRate, uint32_t aChannels) override;

  void DeviceChanged() override;

  bool IsFake() override {
    return false;
  }

  dom::MediaSourceEnum GetMediaSource() const override {
    return dom::MediaSourceEnum::Microphone;
  }

  nsresult TakePhoto(MediaEnginePhotoCallback* aCallback) override
  {
    return NS_ERROR_NOT_IMPLEMENTED;
  }

  uint32_t GetBestFitnessDistance(
      const nsTArray<const NormalizedConstraintSet*>& aConstraintSets,
      const nsString& aDeviceId) const override;

  void Shutdown() override;

  NS_DECL_THREADSAFE_ISUPPORTS

protected:
  ~MediaEngineWebRTCMicrophoneSource() {}

private:
  nsresult
  UpdateSingleSource(const AllocationHandle* aHandle,
                     const NormalizedConstraints& aNetConstraints,
                     const NormalizedConstraints& aNewConstraint,
                     const MediaEnginePrefs& aPrefs,
                     const nsString& aDeviceId,
                     const char** aOutBadConstraint) override;


  void UpdateAECSettingsIfNeeded(bool aEnable, webrtc::EcModes aMode);
  void UpdateAGCSettingsIfNeeded(bool aEnable, webrtc::AgcModes aMode);
  void UpdateNSSettingsIfNeeded(bool aEnable, webrtc::NsModes aMode);

  void SetLastPrefs(const MediaEnginePrefs& aPrefs);

  // These allocate/configure and release the channel
  bool AllocChannel();
  void FreeChannel();
  template<typename T>
  void InsertInGraph(const T* aBuffer,
                     size_t aFrames,
                     uint32_t aChannels);

  void PacketizeAndProcess(MediaStreamGraph* aGraph,
                           const AudioDataValue* aBuffer,
                           size_t aFrames,
                           TrackRate aRate,
                           uint32_t aChannels);


  // This is true when all processing is disabled, we can skip
  // packetization, resampling and other processing passes.
  bool PassThrough() {
    return mSkipProcessing;
  }
  void SetPassThrough(bool aPassThrough) {
    mSkipProcessing = aPassThrough;
  }

  RefPtr<mozilla::AudioInput> mAudioInput;
  RefPtr<WebRTCAudioDataListener> mListener;

  // Note: shared across all microphone sources
  static int sChannelsOpen;

  const UniquePtr<webrtc::AudioProcessing> mAudioProcessing;

  // accessed from the GraphDriver thread except for deletion.
  nsAutoPtr<AudioPacketizer<AudioDataValue, float>> mPacketizerInput;
  nsAutoPtr<AudioPacketizer<AudioDataValue, float>> mPacketizerOutput;

  // mMonitor protects mSources[] and mPrinicpalIds[] access/changes, and
  // transitions of mState from kStarted to kStopped (which are combined with
  // EndTrack()). mSources[] and mPrincipalHandles[] are accessed from webrtc
  // threads.
  Monitor mMonitor;
  nsTArray<RefPtr<SourceMediaStream>> mSources;
  nsTArray<PrincipalHandle> mPrincipalHandles; // Maps to mSources.

  int mCapIndex;
  bool mDelayAgnostic;
  bool mExtendedFilter;
  MOZ_INIT_OUTSIDE_CTOR TrackID mTrackID;
  bool mStarted;

  nsString mDeviceName;
  nsCString mDeviceUUID;

  uint64_t mTotalFrames;
  uint64_t mLastLogFrames;

  // mSkipProcessing is true if none of the processing passes are enabled,
  // because of prefs or constraints. This allows simply copying the audio into
  // the MSG, skipping resampling and the whole webrtc.org code.
  // This is read and written to only on the MSG thread.
  bool mSkipProcessing;

  // To only update microphone when needed, we keep track of previous settings.
  MediaEnginePrefs mLastPrefs;

  // Stores the mixed audio output for the reverse-stream of the AEC.
  AlignedFloatBuffer mOutputBuffer;

  AlignedFloatBuffer mInputBuffer;
  AlignedFloatBuffer mDeinterleavedBuffer;
  AlignedFloatBuffer mInputDownmixBuffer;
};

class MediaEngineWebRTC : public MediaEngine
{
  typedef MediaEngine Super;
public:
  explicit MediaEngineWebRTC(MediaEnginePrefs& aPrefs);

  virtual void SetFakeDeviceChangeEvents() override;

  // Clients should ensure to clean-up sources video/audio sources
  // before invoking Shutdown on this class.
  void Shutdown() override;

  // Returns whether the host supports duplex audio stream.
  bool SupportsDuplex();

  void EnumerateVideoDevices(dom::MediaSourceEnum,
                             nsTArray<RefPtr<MediaEngineVideoSource>>*) override;
  void EnumerateAudioDevices(dom::MediaSourceEnum,
                             nsTArray<RefPtr<MediaEngineAudioSource>>*) override;
private:
  ~MediaEngineWebRTC() {}

  nsCOMPtr<nsIThread> mThread;

  // gUM runnables can e.g. Enumerate from multiple threads
  Mutex mMutex;
  RefPtr<mozilla::AudioInput> mAudioInput;
  bool mFullDuplex;
  bool mDelayAgnostic;
  bool mExtendedFilter;
  bool mHasTabVideoSource;

  // Store devices we've already seen in a hashtable for quick return.
  // Maps UUID to MediaEngineSource (one set for audio, one for video).
  nsRefPtrHashtable<nsStringHashKey, MediaEngineVideoSource> mVideoSources;
  nsRefPtrHashtable<nsStringHashKey, MediaEngineAudioSource> mAudioSources;
};

}

#endif /* NSMEDIAENGINEWEBRTC_H_ */