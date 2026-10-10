// InnerLoopDriver.c
//
// A minimal Core Audio Server Plug-In (HAL driver) exposing one virtual
// device with a 2-channel input stream and a 2-channel output stream that
// share a single ring buffer. Anything played to the device's output
// stream can be read back from its input stream, so it can sit between
// an app's output and a DAW's input without a kernel extension.
//
// This runs in-process inside coreaudiod. Every exported entry point is
// called directly by the HAL, so a missing vtable slot or a crash here
// takes the system's audio down with it — keep this file self-contained
// and defensive about malformed inputs from the HAL.
//
// See ../../docs/ROUTING.md and ../../docs/TESTING.md for how to wire
// this device up in Audio MIDI Setup / Logic once it's installed, and
// ../Makefile + ../../scripts for how to build, sign and install it.

#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreAudio/AudioHardware.h> // BufferFrameSize*, StreamConfiguration selectors
#include <CoreFoundation/CoreFoundation.h>
#include <CoreFoundation/CFPlugInCOM.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>

#include "InnerLoopRingBuffer.h"
#include "InnerLoopTypes.h"

#pragma mark - Driver state

// Anchors the device's zero-timestamp timeline. Recomputed on every
// StartIO so a stopped-then-restarted device doesn't drift against the
// host's absolute time.
typedef struct
{
    Float64 mAnchorSampleTime;
    UInt64  mAnchorHostTime;
    UInt64  mNumberTimeStamps;
} InnerLoopTimeline;

typedef struct
{
    pthread_mutex_t         mStateMutex;
    AudioServerPlugInHostRef mHost;

    Float64                 mSampleRate;
    UInt32                  mIOBufferFrameSize;   // negotiated buffer frame size
    _Atomic UInt32          mIOIsRunningRefCount;

    InnerLoopTimeline       mTimeline;
    mach_timebase_info_data_t mTimebase;

    InnerLoopRingBuffer     mRingBuffer;
    bool                    mRingBufferInitialized;
} InnerLoopDriverState;

static InnerLoopDriverState gState = { .mSampleRate = kInnerLoop_DefaultSampleRate };

#define kInnerLoop_ZeroTimeStampPeriod 4096

#pragma mark - IUnknown / factory boilerplate

static HRESULT InnerLoop_QueryInterface(void* inDriver, REFIID inUUID, LPVOID* outInterface);
static ULONG InnerLoop_AddRef(void* inDriver);
static ULONG InnerLoop_Release(void* inDriver);

static OSStatus InnerLoop_Initialize(AudioServerPlugInDriverRef inDriver, AudioServerPlugInHostRef inHost);
static OSStatus InnerLoop_CreateDevice(AudioServerPlugInDriverRef inDriver, CFDictionaryRef inDescription, const AudioServerPlugInClientInfo* inClientInfo, AudioObjectID* outDeviceObjectID);
static OSStatus InnerLoop_DestroyDevice(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID);
static OSStatus InnerLoop_AddDeviceClient(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, const AudioServerPlugInClientInfo* inClientInfo);
static OSStatus InnerLoop_RemoveDeviceClient(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, const AudioServerPlugInClientInfo* inClientInfo);
static OSStatus InnerLoop_PerformDeviceConfigurationChange(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt64 inChangeAction, void* inChangeInfo);
static OSStatus InnerLoop_AbortDeviceConfigurationChange(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt64 inChangeAction, void* inChangeInfo);

static Boolean InnerLoop_HasProperty(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress);
static OSStatus InnerLoop_IsPropertySettable(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, Boolean* outIsSettable);
static OSStatus InnerLoop_GetPropertyDataSize(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32* outDataSize);
static OSStatus InnerLoop_GetPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32 inDataSize, UInt32* outDataSize, void* outData);
static OSStatus InnerLoop_SetPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32 inDataSize, const void* inData);

static OSStatus InnerLoop_StartIO(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID);
static OSStatus InnerLoop_StopIO(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID);
static OSStatus InnerLoop_GetZeroTimeStamp(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, Float64* outSampleTime, UInt64* outHostTime, UInt64* outSeed);
static OSStatus InnerLoop_WillDoIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, UInt32 inOperationID, Boolean* outWillDo, Boolean* outWillDoInPlace);
static OSStatus InnerLoop_BeginIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, UInt32 inOperationID, UInt32 inIOBufferFrameSize, const AudioServerPlugInIOCycleInfo* inIOCycleInfo);
static OSStatus InnerLoop_DoIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, AudioObjectID inStreamObjectID, UInt32 inClientID, UInt32 inOperationID, UInt32 inIOBufferFrameSize, const AudioServerPlugInIOCycleInfo* inIOCycleInfo, void* ioMainBuffer, void* ioSecondaryBuffer);
static OSStatus InnerLoop_EndIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, UInt32 inOperationID, UInt32 inIOBufferFrameSize, const AudioServerPlugInIOCycleInfo* inIOCycleInfo);

static AudioServerPlugInDriverInterface gInterface = {
    NULL,
    InnerLoop_QueryInterface,
    InnerLoop_AddRef,
    InnerLoop_Release,
    InnerLoop_Initialize,
    InnerLoop_CreateDevice,
    InnerLoop_DestroyDevice,
    InnerLoop_AddDeviceClient,
    InnerLoop_RemoveDeviceClient,
    InnerLoop_PerformDeviceConfigurationChange,
    InnerLoop_AbortDeviceConfigurationChange,
    InnerLoop_HasProperty,
    InnerLoop_IsPropertySettable,
    InnerLoop_GetPropertyDataSize,
    InnerLoop_GetPropertyData,
    InnerLoop_SetPropertyData,
    InnerLoop_StartIO,
    InnerLoop_StopIO,
    InnerLoop_GetZeroTimeStamp,
    InnerLoop_WillDoIOOperation,
    InnerLoop_BeginIOOperation,
    InnerLoop_DoIOOperation,
    InnerLoop_EndIOOperation,
};
static AudioServerPlugInDriverInterface* gInterfacePtr = &gInterface;
static AudioServerPlugInDriverRef gDriverRef = &gInterfacePtr;
static _Atomic ULONG gRefCount = 1;

void* InnerLoop_Create(CFAllocatorRef inAllocator, CFUUIDRef inRequestedTypeUUID);
void* InnerLoop_Create(CFAllocatorRef inAllocator, CFUUIDRef inRequestedTypeUUID)
{
    (void)inAllocator;
    void* theAnswer = NULL;
    if (CFEqual(inRequestedTypeUUID, kAudioServerPlugInTypeUUID))
    {
        theAnswer = gDriverRef;
    }
    return theAnswer;
}

static HRESULT InnerLoop_QueryInterface(void* inDriver, REFIID inUUID, LPVOID* outInterface)
{
    (void)inDriver;
    if (outInterface == NULL)
    {
        return kAudioHardwareIllegalOperationError;
    }
    *outInterface = NULL;

    CFUUIDRef theRequestedUUID = CFUUIDCreateFromUUIDBytes(NULL, inUUID);
    if (theRequestedUUID == NULL)
    {
        return E_NOINTERFACE;
    }

    HRESULT theAnswer = E_NOINTERFACE;
    if (CFEqual(theRequestedUUID, IUnknownUUID) || CFEqual(theRequestedUUID, kAudioServerPlugInDriverInterfaceUUID))
    {
        InnerLoop_AddRef(inDriver);
        *outInterface = gDriverRef;
        theAnswer = 0;
    }
    CFRelease(theRequestedUUID);
    return theAnswer;
}

static ULONG InnerLoop_AddRef(void* inDriver)
{
    (void)inDriver;
    return atomic_fetch_add(&gRefCount, 1) + 1;
}

static ULONG InnerLoop_Release(void* inDriver)
{
    (void)inDriver;
    ULONG theValue = atomic_fetch_sub(&gRefCount, 1) - 1;
    // This driver's vtable/state is a static singleton with no dynamic
    // teardown; refcounting exists only to satisfy the COM-style contract
    // the HAL expects, so we never actually free anything here.
    return theValue;
}

#pragma mark - Lifecycle

static OSStatus InnerLoop_Initialize(AudioServerPlugInDriverRef inDriver, AudioServerPlugInHostRef inHost)
{
    (void)inDriver;

    pthread_mutex_init(&gState.mStateMutex, NULL);
    gState.mHost = inHost;
    gState.mSampleRate = kInnerLoop_DefaultSampleRate;
    gState.mIOBufferFrameSize = 512;
    atomic_store(&gState.mIOIsRunningRefCount, 0);
    mach_timebase_info(&gState.mTimebase);

    gState.mRingBufferInitialized = InnerLoopRingBuffer_Init(&gState.mRingBuffer, kInnerLoop_RingBufferFrames, kInnerLoop_ChannelCount);
    if (!gState.mRingBufferInitialized)
    {
        return kAudioHardwareUnspecifiedError;
    }

    return kAudioHardwareNoError;
}

static OSStatus InnerLoop_CreateDevice(AudioServerPlugInDriverRef inDriver, CFDictionaryRef inDescription, const AudioServerPlugInClientInfo* inClientInfo, AudioObjectID* outDeviceObjectID)
{
    (void)inDriver; (void)inDescription; (void)inClientInfo; (void)outDeviceObjectID;
    // Our single device is fixed at Initialize time; dynamic creation of
    // additional devices isn't supported.
    return kAudioHardwareUnsupportedOperationError;
}

static OSStatus InnerLoop_DestroyDevice(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID)
{
    (void)inDriver; (void)inDeviceObjectID;
    return kAudioHardwareUnsupportedOperationError;
}

static OSStatus InnerLoop_AddDeviceClient(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, const AudioServerPlugInClientInfo* inClientInfo)
{
    (void)inDriver; (void)inDeviceObjectID; (void)inClientInfo;
    return kAudioHardwareNoError;
}

static OSStatus InnerLoop_RemoveDeviceClient(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, const AudioServerPlugInClientInfo* inClientInfo)
{
    (void)inDriver; (void)inDeviceObjectID; (void)inClientInfo;
    return kAudioHardwareNoError;
}

// Only kAudioDevicePropertyNominalSampleRate changes go through the
// asynchronous RequestDeviceConfigurationChange/Perform dance; everything
// else we support (buffer frame size) is safe to apply synchronously in
// SetPropertyData because it doesn't require re-negotiating IO timing.
static OSStatus InnerLoop_PerformDeviceConfigurationChange(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt64 inChangeAction, void* inChangeInfo)
{
    (void)inDriver; (void)inDeviceObjectID; (void)inChangeInfo;

    pthread_mutex_lock(&gState.mStateMutex);
    // inChangeAction carries the pending sample rate encoded as a raw bit
    // pattern (see SetPropertyData below); decode it back via memcpy to
    // avoid a strict-aliasing violation.
    Float64 theNewSampleRate;
    memcpy(&theNewSampleRate, &inChangeAction, sizeof(theNewSampleRate));
    gState.mSampleRate = theNewSampleRate;
    InnerLoopRingBuffer_Reset(&gState.mRingBuffer);
    pthread_mutex_unlock(&gState.mStateMutex);

    return kAudioHardwareNoError;
}

static OSStatus InnerLoop_AbortDeviceConfigurationChange(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt64 inChangeAction, void* inChangeInfo)
{
    (void)inDriver; (void)inDeviceObjectID; (void)inChangeAction; (void)inChangeInfo;
    return kAudioHardwareNoError;
}

#pragma mark - Property helpers

static bool IsValidObjectID(AudioObjectID inObjectID)
{
    return inObjectID == kObjectID_PlugIn || inObjectID == kObjectID_Device ||
           inObjectID == kObjectID_Stream_Input || inObjectID == kObjectID_Stream_Output;
}

static AudioValueRange gAvailableSampleRates[2] = {
    { 44100.0, 44100.0 },
    { 48000.0, 48000.0 },
};

static void FillStreamFormat(AudioObjectID inStreamID, AudioStreamBasicDescription* outFormat)
{
    memset(outFormat, 0, sizeof(AudioStreamBasicDescription));
    outFormat->mSampleRate = gState.mSampleRate;
    outFormat->mFormatID = kAudioFormatLinearPCM;
    outFormat->mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    outFormat->mBytesPerPacket = kInnerLoop_BytesPerFrame;
    outFormat->mFramesPerPacket = 1;
    outFormat->mBytesPerFrame = kInnerLoop_BytesPerFrame;
    outFormat->mChannelsPerFrame = kInnerLoop_ChannelCount;
    outFormat->mBitsPerChannel = kInnerLoop_BitsPerChannel;
    (void)inStreamID;
}

#pragma mark - HasProperty / IsPropertySettable / GetPropertyDataSize / GetPropertyData

static Boolean InnerLoop_HasProperty(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress)
{
    (void)inDriver; (void)inClientProcessID;
    if (!IsValidObjectID(inObjectID) || inAddress == NULL)
    {
        return false;
    }

    switch (inAddress->mSelector)
    {
        case kAudioObjectPropertyBaseClass:
        case kAudioObjectPropertyClass:
        case kAudioObjectPropertyOwner:
        case kAudioObjectPropertyName:
        case kAudioObjectPropertyManufacturer:
        case kAudioObjectPropertyOwnedObjects:
            return true;
        default:
            break;
    }

    if (inObjectID == kObjectID_PlugIn)
    {
        switch (inAddress->mSelector)
        {
            case kAudioPlugInPropertyBoxList:
            case kAudioPlugInPropertyDeviceList:
            case kAudioPlugInPropertyTranslateUIDToDevice:
            case kAudioPlugInPropertyResourceBundle:
                return true;
            default:
                return false;
        }
    }
    else if (inObjectID == kObjectID_Device)
    {
        switch (inAddress->mSelector)
        {
            case kAudioDevicePropertyDeviceUID:
            case kAudioDevicePropertyModelUID:
            case kAudioDevicePropertyTransportType:
            case kAudioDevicePropertyRelatedDevices:
            case kAudioDevicePropertyClockDomain:
            case kAudioDevicePropertyDeviceIsAlive:
            case kAudioDevicePropertyDeviceIsRunning:
            case kAudioDevicePropertyDeviceCanBeDefaultDevice:
            case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
            case kAudioDevicePropertyLatency:
            case kAudioDevicePropertyStreams:
            case kAudioObjectPropertyControlList:
            case kAudioDevicePropertySafetyOffset:
            case kAudioDevicePropertyNominalSampleRate:
            case kAudioDevicePropertyAvailableNominalSampleRates:
            case kAudioDevicePropertyIsHidden:
            case kAudioDevicePropertyZeroTimeStampPeriod:
            case kAudioDevicePropertyBufferFrameSize:
            case kAudioDevicePropertyBufferFrameSizeRange:
            case kAudioDevicePropertyPreferredChannelsForStereo:
            case kAudioDevicePropertyStreamConfiguration:
                return true;
            default:
                return false;
        }
    }
    else // one of the two streams
    {
        switch (inAddress->mSelector)
        {
            case kAudioStreamPropertyIsActive:
            case kAudioStreamPropertyDirection:
            case kAudioStreamPropertyTerminalType:
            case kAudioStreamPropertyStartingChannel:
            case kAudioStreamPropertyLatency:
            case kAudioStreamPropertyVirtualFormat:
            case kAudioStreamPropertyPhysicalFormat:
            case kAudioStreamPropertyAvailableVirtualFormats:
            case kAudioStreamPropertyAvailablePhysicalFormats:
                return true;
            default:
                return false;
        }
    }
}

static OSStatus InnerLoop_IsPropertySettable(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, Boolean* outIsSettable)
{
    (void)inDriver; (void)inClientProcessID;
    if (!IsValidObjectID(inObjectID) || inAddress == NULL || outIsSettable == NULL)
    {
        return kAudioHardwareIllegalOperationError;
    }

    *outIsSettable = false;

    if (inObjectID == kObjectID_Device)
    {
        if (inAddress->mSelector == kAudioDevicePropertyNominalSampleRate ||
            inAddress->mSelector == kAudioDevicePropertyBufferFrameSize)
        {
            *outIsSettable = true;
        }
    }

    return kAudioHardwareNoError;
}

static OSStatus InnerLoop_GetPropertyDataSize(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32* outDataSize)
{
    (void)inDriver; (void)inClientProcessID; (void)inQualifierDataSize; (void)inQualifierData;
    if (!IsValidObjectID(inObjectID) || inAddress == NULL || outDataSize == NULL)
    {
        return kAudioHardwareIllegalOperationError;
    }

    switch (inAddress->mSelector)
    {
        case kAudioObjectPropertyBaseClass:
        case kAudioObjectPropertyClass:
        case kAudioObjectPropertyOwner:
            *outDataSize = sizeof(AudioClassID);
            return kAudioHardwareNoError;
        case kAudioObjectPropertyName:
        case kAudioObjectPropertyManufacturer:
            *outDataSize = sizeof(CFStringRef);
            return kAudioHardwareNoError;
        case kAudioObjectPropertyOwnedObjects:
            if (inObjectID == kObjectID_PlugIn) *outDataSize = sizeof(AudioObjectID) * 1;
            else if (inObjectID == kObjectID_Device) *outDataSize = sizeof(AudioObjectID) * 2;
            else *outDataSize = 0;
            return kAudioHardwareNoError;
        default:
            break;
    }

    if (inObjectID == kObjectID_PlugIn)
    {
        switch (inAddress->mSelector)
        {
            case kAudioPlugInPropertyBoxList:      *outDataSize = 0; return kAudioHardwareNoError;
            case kAudioPlugInPropertyDeviceList:   *outDataSize = sizeof(AudioObjectID); return kAudioHardwareNoError;
            case kAudioPlugInPropertyTranslateUIDToDevice: *outDataSize = sizeof(AudioObjectID); return kAudioHardwareNoError;
            case kAudioPlugInPropertyResourceBundle: *outDataSize = sizeof(CFStringRef); return kAudioHardwareNoError;
            default: return kAudioHardwareUnknownPropertyError;
        }
    }
    else if (inObjectID == kObjectID_Device)
    {
        switch (inAddress->mSelector)
        {
            case kAudioDevicePropertyDeviceUID:
            case kAudioDevicePropertyModelUID:
                *outDataSize = sizeof(CFStringRef); return kAudioHardwareNoError;
            case kAudioDevicePropertyTransportType:    *outDataSize = sizeof(UInt32); return kAudioHardwareNoError;
            case kAudioDevicePropertyRelatedDevices:   *outDataSize = sizeof(AudioObjectID); return kAudioHardwareNoError;
            case kAudioDevicePropertyClockDomain:
            case kAudioDevicePropertyDeviceIsAlive:
            case kAudioDevicePropertyDeviceIsRunning:
            case kAudioDevicePropertyDeviceCanBeDefaultDevice:
            case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
            case kAudioDevicePropertyIsHidden:
            case kAudioDevicePropertyZeroTimeStampPeriod:
            case kAudioDevicePropertyBufferFrameSize:
                *outDataSize = sizeof(UInt32); return kAudioHardwareNoError;
            case kAudioDevicePropertyLatency:
            case kAudioDevicePropertySafetyOffset:
                *outDataSize = sizeof(UInt32); return kAudioHardwareNoError;
            case kAudioDevicePropertyStreams:
                *outDataSize = sizeof(AudioObjectID); return kAudioHardwareNoError;
            case kAudioObjectPropertyControlList:
                *outDataSize = 0; return kAudioHardwareNoError;
            case kAudioDevicePropertyNominalSampleRate:
                *outDataSize = sizeof(Float64); return kAudioHardwareNoError;
            case kAudioDevicePropertyAvailableNominalSampleRates:
                *outDataSize = sizeof(gAvailableSampleRates); return kAudioHardwareNoError;
            case kAudioDevicePropertyBufferFrameSizeRange:
                *outDataSize = sizeof(AudioValueRange); return kAudioHardwareNoError;
            case kAudioDevicePropertyPreferredChannelsForStereo:
                *outDataSize = sizeof(UInt32) * 2; return kAudioHardwareNoError;
            case kAudioDevicePropertyStreamConfiguration:
                // One buffer (interleaved) per direction; sizeof(AudioBufferList)
                // already accounts for the single AudioBuffer in mBuffers[1].
                *outDataSize = sizeof(AudioBufferList); return kAudioHardwareNoError;
            default: return kAudioHardwareUnknownPropertyError;
        }
    }
    else
    {
        switch (inAddress->mSelector)
        {
            case kAudioStreamPropertyIsActive:
            case kAudioStreamPropertyDirection:
            case kAudioStreamPropertyTerminalType:
            case kAudioStreamPropertyStartingChannel:
            case kAudioStreamPropertyLatency:
                *outDataSize = sizeof(UInt32); return kAudioHardwareNoError;
            case kAudioStreamPropertyVirtualFormat:
            case kAudioStreamPropertyPhysicalFormat:
                *outDataSize = sizeof(AudioStreamBasicDescription); return kAudioHardwareNoError;
            case kAudioStreamPropertyAvailableVirtualFormats:
            case kAudioStreamPropertyAvailablePhysicalFormats:
                *outDataSize = sizeof(AudioStreamRangedDescription) * 2; return kAudioHardwareNoError;
            default: return kAudioHardwareUnknownPropertyError;
        }
    }
}

static OSStatus InnerLoop_GetPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32 inDataSize, UInt32* outDataSize, void* outData)
{
    (void)inDriver; (void)inClientProcessID;
    if (!IsValidObjectID(inObjectID) || inAddress == NULL || outDataSize == NULL || outData == NULL)
    {
        return kAudioHardwareIllegalOperationError;
    }

    switch (inAddress->mSelector)
    {
        case kAudioObjectPropertyBaseClass:
        {
            // All of our object classes (plug-in, device, stream) descend
            // directly from AudioObject.
            if (inDataSize < sizeof(AudioClassID)) return kAudioHardwareBadPropertySizeError;
            *(AudioClassID*)outData = kAudioObjectClassID;
            *outDataSize = sizeof(AudioClassID);
            return kAudioHardwareNoError;
        }
        case kAudioObjectPropertyClass:
        {
            if (inDataSize < sizeof(AudioClassID)) return kAudioHardwareBadPropertySizeError;
            AudioClassID theClass = kAudioPlugInClassID;
            if (inObjectID == kObjectID_Device) theClass = kAudioDeviceClassID;
            else if (inObjectID == kObjectID_Stream_Input || inObjectID == kObjectID_Stream_Output) theClass = kAudioStreamClassID;
            *(AudioClassID*)outData = theClass;
            *outDataSize = sizeof(AudioClassID);
            return kAudioHardwareNoError;
        }
        case kAudioObjectPropertyOwner:
        {
            if (inDataSize < sizeof(AudioObjectID)) return kAudioHardwareBadPropertySizeError;
            AudioObjectID theOwner = kAudioObjectUnknown;
            if (inObjectID == kObjectID_Device) theOwner = kObjectID_PlugIn;
            else if (inObjectID == kObjectID_Stream_Input || inObjectID == kObjectID_Stream_Output) theOwner = kObjectID_Device;
            *(AudioObjectID*)outData = theOwner;
            *outDataSize = sizeof(AudioObjectID);
            return kAudioHardwareNoError;
        }
        case kAudioObjectPropertyName:
        {
            if (inDataSize < sizeof(CFStringRef)) return kAudioHardwareBadPropertySizeError;
            CFStringRef theName;
            if (inObjectID == kObjectID_PlugIn) theName = CFSTR(kInnerLoop_DeviceName);
            else if (inObjectID == kObjectID_Device) theName = CFSTR(kInnerLoop_DeviceName);
            else if (inObjectID == kObjectID_Stream_Input) theName = CFSTR(kInnerLoop_DeviceName " Input");
            else theName = CFSTR(kInnerLoop_DeviceName " Output");
            *(CFStringRef*)outData = CFStringCreateCopy(NULL, theName);
            *outDataSize = sizeof(CFStringRef);
            return kAudioHardwareNoError;
        }
        case kAudioObjectPropertyManufacturer:
        {
            if (inDataSize < sizeof(CFStringRef)) return kAudioHardwareBadPropertySizeError;
            *(CFStringRef*)outData = CFStringCreateCopy(NULL, CFSTR(kInnerLoop_Manufacturer));
            *outDataSize = sizeof(CFStringRef);
            return kAudioHardwareNoError;
        }
        case kAudioObjectPropertyOwnedObjects:
        {
            if (inObjectID == kObjectID_PlugIn)
            {
                if (inDataSize < sizeof(AudioObjectID)) { *outDataSize = 0; return kAudioHardwareNoError; }
                ((AudioObjectID*)outData)[0] = kObjectID_Device;
                *outDataSize = sizeof(AudioObjectID);
            }
            else if (inObjectID == kObjectID_Device)
            {
                UInt32 theCount = inDataSize / sizeof(AudioObjectID);
                if (theCount > 2) theCount = 2;
                AudioObjectID theIDs[2] = { kObjectID_Stream_Input, kObjectID_Stream_Output };
                memcpy(outData, theIDs, theCount * sizeof(AudioObjectID));
                *outDataSize = theCount * sizeof(AudioObjectID);
            }
            else
            {
                *outDataSize = 0;
            }
            return kAudioHardwareNoError;
        }
        default:
            break;
    }

    if (inObjectID == kObjectID_PlugIn)
    {
        switch (inAddress->mSelector)
        {
            case kAudioPlugInPropertyBoxList:
                *outDataSize = 0;
                return kAudioHardwareNoError;
            case kAudioPlugInPropertyDeviceList:
                if (inDataSize < sizeof(AudioObjectID)) { *outDataSize = 0; return kAudioHardwareNoError; }
                ((AudioObjectID*)outData)[0] = kObjectID_Device;
                *outDataSize = sizeof(AudioObjectID);
                return kAudioHardwareNoError;
            case kAudioPlugInPropertyTranslateUIDToDevice:
            {
                if (inQualifierData == NULL || inQualifierDataSize < sizeof(CFStringRef) || inDataSize < sizeof(AudioObjectID))
                {
                    return kAudioHardwareBadPropertySizeError;
                }
                CFStringRef theUID = *(CFStringRef*)inQualifierData;
                AudioObjectID theDeviceID = kAudioObjectUnknown;
                if (theUID != NULL && CFStringCompare(theUID, CFSTR(kInnerLoop_DeviceUID), 0) == kCFCompareEqualTo)
                {
                    theDeviceID = kObjectID_Device;
                }
                *(AudioObjectID*)outData = theDeviceID;
                *outDataSize = sizeof(AudioObjectID);
                return kAudioHardwareNoError;
            }
            case kAudioPlugInPropertyResourceBundle:
                if (inDataSize < sizeof(CFStringRef)) return kAudioHardwareBadPropertySizeError;
                *(CFStringRef*)outData = CFStringCreateCopy(NULL, CFSTR(""));
                *outDataSize = sizeof(CFStringRef);
                return kAudioHardwareNoError;
            default:
                return kAudioHardwareUnknownPropertyError;
        }
    }
    else if (inObjectID == kObjectID_Device)
    {
        switch (inAddress->mSelector)
        {
            case kAudioDevicePropertyDeviceUID:
                if (inDataSize < sizeof(CFStringRef)) return kAudioHardwareBadPropertySizeError;
                *(CFStringRef*)outData = CFStringCreateCopy(NULL, CFSTR(kInnerLoop_DeviceUID));
                *outDataSize = sizeof(CFStringRef);
                return kAudioHardwareNoError;
            case kAudioDevicePropertyModelUID:
                if (inDataSize < sizeof(CFStringRef)) return kAudioHardwareBadPropertySizeError;
                *(CFStringRef*)outData = CFStringCreateCopy(NULL, CFSTR(kInnerLoop_DeviceModelUID));
                *outDataSize = sizeof(CFStringRef);
                return kAudioHardwareNoError;
            case kAudioDevicePropertyTransportType:
                if (inDataSize < sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
                *(UInt32*)outData = kAudioDeviceTransportTypeVirtual;
                *outDataSize = sizeof(UInt32);
                return kAudioHardwareNoError;
            case kAudioDevicePropertyRelatedDevices:
                if (inDataSize < sizeof(AudioObjectID)) { *outDataSize = 0; return kAudioHardwareNoError; }
                ((AudioObjectID*)outData)[0] = kObjectID_Device;
                *outDataSize = sizeof(AudioObjectID);
                return kAudioHardwareNoError;
            case kAudioDevicePropertyClockDomain:
                if (inDataSize < sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
                *(UInt32*)outData = 0;
                *outDataSize = sizeof(UInt32);
                return kAudioHardwareNoError;
            case kAudioDevicePropertyDeviceIsAlive:
                if (inDataSize < sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
                *(UInt32*)outData = 1;
                *outDataSize = sizeof(UInt32);
                return kAudioHardwareNoError;
            case kAudioDevicePropertyDeviceIsRunning:
                if (inDataSize < sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
                *(UInt32*)outData = atomic_load(&gState.mIOIsRunningRefCount) > 0 ? 1 : 0;
                *outDataSize = sizeof(UInt32);
                return kAudioHardwareNoError;
            case kAudioDevicePropertyDeviceCanBeDefaultDevice:
            case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
                if (inDataSize < sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
                *(UInt32*)outData = 1;
                *outDataSize = sizeof(UInt32);
                return kAudioHardwareNoError;
            case kAudioDevicePropertyIsHidden:
                if (inDataSize < sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
                *(UInt32*)outData = 0;
                *outDataSize = sizeof(UInt32);
                return kAudioHardwareNoError;
            case kAudioDevicePropertyZeroTimeStampPeriod:
                if (inDataSize < sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
                *(UInt32*)outData = kInnerLoop_ZeroTimeStampPeriod;
                *outDataSize = sizeof(UInt32);
                return kAudioHardwareNoError;
            case kAudioDevicePropertyLatency:
            case kAudioDevicePropertySafetyOffset:
                if (inDataSize < sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
                *(UInt32*)outData = 0;
                *outDataSize = sizeof(UInt32);
                return kAudioHardwareNoError;
            case kAudioDevicePropertyStreams:
            {
                UInt32 theCount = inDataSize / sizeof(AudioObjectID);
                if (inAddress->mScope == kAudioObjectPropertyScopeInput)
                {
                    if (theCount >= 1) { ((AudioObjectID*)outData)[0] = kObjectID_Stream_Input; *outDataSize = sizeof(AudioObjectID); }
                    else *outDataSize = 0;
                }
                else if (inAddress->mScope == kAudioObjectPropertyScopeOutput)
                {
                    if (theCount >= 1) { ((AudioObjectID*)outData)[0] = kObjectID_Stream_Output; *outDataSize = sizeof(AudioObjectID); }
                    else *outDataSize = 0;
                }
                else
                {
                    AudioObjectID theIDs[2] = { kObjectID_Stream_Input, kObjectID_Stream_Output };
                    if (theCount > 2) theCount = 2;
                    memcpy(outData, theIDs, theCount * sizeof(AudioObjectID));
                    *outDataSize = theCount * sizeof(AudioObjectID);
                }
                return kAudioHardwareNoError;
            }
            case kAudioObjectPropertyControlList:
                *outDataSize = 0;
                return kAudioHardwareNoError;
            case kAudioDevicePropertyNominalSampleRate:
                if (inDataSize < sizeof(Float64)) return kAudioHardwareBadPropertySizeError;
                pthread_mutex_lock(&gState.mStateMutex);
                *(Float64*)outData = gState.mSampleRate;
                pthread_mutex_unlock(&gState.mStateMutex);
                *outDataSize = sizeof(Float64);
                return kAudioHardwareNoError;
            case kAudioDevicePropertyAvailableNominalSampleRates:
            {
                UInt32 theCount = inDataSize / sizeof(AudioValueRange);
                if (theCount > 2) theCount = 2;
                memcpy(outData, gAvailableSampleRates, theCount * sizeof(AudioValueRange));
                *outDataSize = theCount * sizeof(AudioValueRange);
                return kAudioHardwareNoError;
            }
            case kAudioDevicePropertyBufferFrameSize:
                if (inDataSize < sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
                pthread_mutex_lock(&gState.mStateMutex);
                *(UInt32*)outData = gState.mIOBufferFrameSize;
                pthread_mutex_unlock(&gState.mStateMutex);
                *outDataSize = sizeof(UInt32);
                return kAudioHardwareNoError;
            case kAudioDevicePropertyBufferFrameSizeRange:
                if (inDataSize < sizeof(AudioValueRange)) return kAudioHardwareBadPropertySizeError;
                ((AudioValueRange*)outData)->mMinimum = kInnerLoop_MinBufferFrameSize;
                ((AudioValueRange*)outData)->mMaximum = kInnerLoop_MaxBufferFrameSize;
                *outDataSize = sizeof(AudioValueRange);
                return kAudioHardwareNoError;
            case kAudioDevicePropertyPreferredChannelsForStereo:
                if (inDataSize < sizeof(UInt32) * 2) return kAudioHardwareBadPropertySizeError;
                ((UInt32*)outData)[0] = 1;
                ((UInt32*)outData)[1] = 2;
                *outDataSize = sizeof(UInt32) * 2;
                return kAudioHardwareNoError;
            case kAudioDevicePropertyStreamConfiguration:
            {
                if (inDataSize < sizeof(AudioBufferList)) return kAudioHardwareBadPropertySizeError;
                AudioBufferList* theList = (AudioBufferList*)outData;
                theList->mNumberBuffers = 1;
                theList->mBuffers[0].mNumberChannels = kInnerLoop_ChannelCount;
                pthread_mutex_lock(&gState.mStateMutex);
                theList->mBuffers[0].mDataByteSize = gState.mIOBufferFrameSize * kInnerLoop_BytesPerFrame;
                pthread_mutex_unlock(&gState.mStateMutex);
                theList->mBuffers[0].mData = NULL;
                *outDataSize = sizeof(AudioBufferList);
                return kAudioHardwareNoError;
            }
            default:
                return kAudioHardwareUnknownPropertyError;
        }
    }
    else // stream object
    {
        switch (inAddress->mSelector)
        {
            case kAudioStreamPropertyIsActive:
                if (inDataSize < sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
                *(UInt32*)outData = 1;
                *outDataSize = sizeof(UInt32);
                return kAudioHardwareNoError;
            case kAudioStreamPropertyDirection:
                if (inDataSize < sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
                *(UInt32*)outData = (inObjectID == kObjectID_Stream_Input) ? 1 : 0;
                *outDataSize = sizeof(UInt32);
                return kAudioHardwareNoError;
            case kAudioStreamPropertyTerminalType:
                if (inDataSize < sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
                *(UInt32*)outData = kAudioStreamTerminalTypeLine;
                *outDataSize = sizeof(UInt32);
                return kAudioHardwareNoError;
            case kAudioStreamPropertyStartingChannel:
                if (inDataSize < sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
                *(UInt32*)outData = 1;
                *outDataSize = sizeof(UInt32);
                return kAudioHardwareNoError;
            case kAudioStreamPropertyLatency:
                if (inDataSize < sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
                *(UInt32*)outData = 0;
                *outDataSize = sizeof(UInt32);
                return kAudioHardwareNoError;
            case kAudioStreamPropertyVirtualFormat:
            case kAudioStreamPropertyPhysicalFormat:
                if (inDataSize < sizeof(AudioStreamBasicDescription)) return kAudioHardwareBadPropertySizeError;
                pthread_mutex_lock(&gState.mStateMutex);
                FillStreamFormat(inObjectID, (AudioStreamBasicDescription*)outData);
                pthread_mutex_unlock(&gState.mStateMutex);
                *outDataSize = sizeof(AudioStreamBasicDescription);
                return kAudioHardwareNoError;
            case kAudioStreamPropertyAvailableVirtualFormats:
            case kAudioStreamPropertyAvailablePhysicalFormats:
            {
                UInt32 theCount = inDataSize / sizeof(AudioStreamRangedDescription);
                if (theCount > 2) theCount = 2;
                AudioStreamRangedDescription* theOut = (AudioStreamRangedDescription*)outData;
                for (UInt32 i = 0; i < theCount; i++)
                {
                    memset(&theOut[i], 0, sizeof(AudioStreamRangedDescription));
                    theOut[i].mFormat.mSampleRate = gAvailableSampleRates[i].mMinimum;
                    theOut[i].mFormat.mFormatID = kAudioFormatLinearPCM;
                    theOut[i].mFormat.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
                    theOut[i].mFormat.mBytesPerPacket = kInnerLoop_BytesPerFrame;
                    theOut[i].mFormat.mFramesPerPacket = 1;
                    theOut[i].mFormat.mBytesPerFrame = kInnerLoop_BytesPerFrame;
                    theOut[i].mFormat.mChannelsPerFrame = kInnerLoop_ChannelCount;
                    theOut[i].mFormat.mBitsPerChannel = kInnerLoop_BitsPerChannel;
                    theOut[i].mSampleRateRange = gAvailableSampleRates[i];
                }
                *outDataSize = theCount * sizeof(AudioStreamRangedDescription);
                return kAudioHardwareNoError;
            }
            default:
                return kAudioHardwareUnknownPropertyError;
        }
    }
}

static OSStatus InnerLoop_SetPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32 inDataSize, const void* inData)
{
    (void)inDriver; (void)inClientProcessID; (void)inQualifierDataSize; (void)inQualifierData;
    if (!IsValidObjectID(inObjectID) || inAddress == NULL || inData == NULL)
    {
        return kAudioHardwareIllegalOperationError;
    }

    if (inObjectID == kObjectID_Device)
    {
        if (inAddress->mSelector == kAudioDevicePropertyNominalSampleRate)
        {
            if (inDataSize < sizeof(Float64)) return kAudioHardwareBadPropertySizeError;
            Float64 theNewRate = *(const Float64*)inData;
            if (theNewRate != 44100.0 && theNewRate != 48000.0)
            {
                return kAudioHardwareIllegalOperationError;
            }
            if (gState.mHost != NULL)
            {
                // Encode the pending rate into the 64-bit change-action
                // token so PerformDeviceConfigurationChange can pick it up
                // once the HAL calls back into us.
                UInt64 theToken;
                memcpy(&theToken, &theNewRate, sizeof(theToken));
                gState.mHost->RequestDeviceConfigurationChange(gState.mHost, kObjectID_Device, theToken, NULL);
            }
            else
            {
                pthread_mutex_lock(&gState.mStateMutex);
                gState.mSampleRate = theNewRate;
                pthread_mutex_unlock(&gState.mStateMutex);
            }
            return kAudioHardwareNoError;
        }
        else if (inAddress->mSelector == kAudioDevicePropertyBufferFrameSize)
        {
            if (inDataSize < sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
            UInt32 theNewSize = *(const UInt32*)inData;
            if (theNewSize < kInnerLoop_MinBufferFrameSize || theNewSize > kInnerLoop_MaxBufferFrameSize)
            {
                return kAudioHardwareIllegalOperationError;
            }
            pthread_mutex_lock(&gState.mStateMutex);
            gState.mIOBufferFrameSize = theNewSize;
            pthread_mutex_unlock(&gState.mStateMutex);
            return kAudioHardwareNoError;
        }
    }

    return kAudioHardwareUnknownPropertyError;
}

#pragma mark - IO cycle

static OSStatus InnerLoop_StartIO(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID)
{
    (void)inDriver; (void)inDeviceObjectID; (void)inClientID;

    UInt32 thePreviousCount = atomic_fetch_add(&gState.mIOIsRunningRefCount, 1);
    if (thePreviousCount == 0)
    {
        // First client to start IO: (re)anchor the timeline and clear any
        // stale audio from a previous session so playback doesn't start
        // with a burst of old samples.
        pthread_mutex_lock(&gState.mStateMutex);
        gState.mTimeline.mAnchorHostTime = mach_absolute_time();
        gState.mTimeline.mAnchorSampleTime = 0;
        gState.mTimeline.mNumberTimeStamps = 0;
        pthread_mutex_unlock(&gState.mStateMutex);
        InnerLoopRingBuffer_Reset(&gState.mRingBuffer);
    }
    return kAudioHardwareNoError;
}

static OSStatus InnerLoop_StopIO(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID)
{
    (void)inDriver; (void)inDeviceObjectID; (void)inClientID;
    atomic_fetch_sub(&gState.mIOIsRunningRefCount, 1);
    return kAudioHardwareNoError;
}

static Float64 HostTicksPerFrame(void)
{
    double theHostTicksPerSecond = 1000000000.0 * (double)gState.mTimebase.denom / (double)gState.mTimebase.numer;
    return theHostTicksPerSecond / gState.mSampleRate;
}

static OSStatus InnerLoop_GetZeroTimeStamp(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, Float64* outSampleTime, UInt64* outHostTime, UInt64* outSeed)
{
    (void)inDriver; (void)inDeviceObjectID; (void)inClientID;
    if (outSampleTime == NULL || outHostTime == NULL || outSeed == NULL)
    {
        return kAudioHardwareIllegalOperationError;
    }

    pthread_mutex_lock(&gState.mStateMutex);

    Float64 theHostTicksPerPeriod = HostTicksPerFrame() * kInnerLoop_ZeroTimeStampPeriod;
    UInt64 theCurrentHostTime = mach_absolute_time();

    Float64 theHostTickOffset = ((Float64)gState.mTimeline.mNumberTimeStamps + 1) * theHostTicksPerPeriod;
    Float64 theNextHostTime = (Float64)gState.mTimeline.mAnchorHostTime + theHostTickOffset;
    while (theNextHostTime <= (Float64)theCurrentHostTime + theHostTicksPerPeriod)
    {
        gState.mTimeline.mNumberTimeStamps++;
        theHostTickOffset = ((Float64)gState.mTimeline.mNumberTimeStamps + 1) * theHostTicksPerPeriod;
        theNextHostTime = (Float64)gState.mTimeline.mAnchorHostTime + theHostTickOffset;
    }

    *outSampleTime = (Float64)gState.mTimeline.mNumberTimeStamps * kInnerLoop_ZeroTimeStampPeriod;
    *outHostTime = gState.mTimeline.mAnchorHostTime + (UInt64)((Float64)gState.mTimeline.mNumberTimeStamps * theHostTicksPerPeriod);
    *outSeed = 1;

    pthread_mutex_unlock(&gState.mStateMutex);
    return kAudioHardwareNoError;
}

static OSStatus InnerLoop_WillDoIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, UInt32 inOperationID, Boolean* outWillDo, Boolean* outWillDoInPlace)
{
    (void)inDriver; (void)inDeviceObjectID; (void)inClientID;
    Boolean theWillDo = false;
    Boolean theWillDoInPlace = true;

    switch (inOperationID)
    {
        case kAudioServerPlugInIOOperationReadInput:
        case kAudioServerPlugInIOOperationWriteMix:
            theWillDo = true;
            break;
        default:
            break;
    }

    if (outWillDo != NULL) *outWillDo = theWillDo;
    if (outWillDoInPlace != NULL) *outWillDoInPlace = theWillDoInPlace;
    return kAudioHardwareNoError;
}

static OSStatus InnerLoop_BeginIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, UInt32 inOperationID, UInt32 inIOBufferFrameSize, const AudioServerPlugInIOCycleInfo* inIOCycleInfo)
{
    (void)inDriver; (void)inDeviceObjectID; (void)inClientID; (void)inOperationID; (void)inIOBufferFrameSize; (void)inIOCycleInfo;
    return kAudioHardwareNoError;
}

static OSStatus InnerLoop_DoIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, AudioObjectID inStreamObjectID, UInt32 inClientID, UInt32 inOperationID, UInt32 inIOBufferFrameSize, const AudioServerPlugInIOCycleInfo* inIOCycleInfo, void* ioMainBuffer, void* ioSecondaryBuffer)
{
    (void)inDriver; (void)inDeviceObjectID; (void)inClientID; (void)inIOCycleInfo; (void)ioSecondaryBuffer;

    if (ioMainBuffer == NULL)
    {
        return kAudioHardwareIllegalOperationError;
    }

    if (inOperationID == kAudioServerPlugInIOOperationWriteMix && inStreamObjectID == kObjectID_Stream_Output)
    {
        InnerLoopRingBuffer_Write(&gState.mRingBuffer, (const Float32*)ioMainBuffer, inIOBufferFrameSize);
    }
    else if (inOperationID == kAudioServerPlugInIOOperationReadInput && inStreamObjectID == kObjectID_Stream_Input)
    {
        InnerLoopRingBuffer_Read(&gState.mRingBuffer, (Float32*)ioMainBuffer, inIOBufferFrameSize);
    }

    return kAudioHardwareNoError;
}

static OSStatus InnerLoop_EndIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, UInt32 inOperationID, UInt32 inIOBufferFrameSize, const AudioServerPlugInIOCycleInfo* inIOCycleInfo)
{
    (void)inDriver; (void)inDeviceObjectID; (void)inClientID; (void)inOperationID; (void)inIOBufferFrameSize; (void)inIOCycleInfo;
    return kAudioHardwareNoError;
}
