// InnerLoopTypes.h
// Shared constants and identifiers for the InnerLoop virtual audio driver.

#ifndef InnerLoopTypes_h
#define InnerLoopTypes_h

#include <CoreAudio/AudioServerPlugIn.h>

// Bundle / plug-in identity. Must match Info.plist.
#define kInnerLoop_BundleID        "com.itai.virtualaudio.loopback"
#define kInnerLoop_FactoryUUID     "508D873C-0599-47CE-A0E0-4B07A7D8349B"

// Device identity (persistent across reboots/reinstalls with the same UID).
#define kInnerLoop_DeviceUID       "InnerLoopVirtualDevice_UID"
#define kInnerLoop_DeviceModelUID  "InnerLoopVirtualDevice_ModelUID"
#define kInnerLoop_DeviceName      "InnerLoop"
#define kInnerLoop_Manufacturer    "InnerLoop Project"

// Fixed object IDs for our (very small) object graph:
//   PlugIn -> Device -> { Stream_Input, Stream_Output }
enum
{
    kObjectID_PlugIn        = kAudioObjectPlugInObject,
    kObjectID_Device        = 2,
    kObjectID_Stream_Input  = 3,
    kObjectID_Stream_Output = 4,
};

// Audio format. Start simple: stereo, 32-bit float, interleaved.
#define kInnerLoop_ChannelCount     2
#define kInnerLoop_BitsPerChannel   32
#define kInnerLoop_BytesPerFrame    (kInnerLoop_ChannelCount * sizeof(Float32))

// Supported nominal sample rates.
#define kInnerLoop_DefaultSampleRate 44100.0

// IO buffer frame size bounds negotiated with the HAL.
#define kInnerLoop_MinBufferFrameSize  16
#define kInnerLoop_MaxBufferFrameSize  4096

// The shared ring buffer is sized as a multiple of the largest buffer
// the HAL is allowed to request, so a slow reader/writer never wraps
// into data the other side hasn't consumed/produced yet under normal
// operation.
#define kInnerLoop_RingBufferFrames (kInnerLoop_MaxBufferFrameSize * 8)

#endif /* InnerLoopTypes_h */
