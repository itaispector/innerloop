// InnerLoopRingBuffer.h
//
// A small mutex-guarded circular buffer of interleaved Float32 frames
// shared between the virtual device's output stream (writer) and its
// input stream (reader). One IO cycle's worth of audio moves through
// here per call to DoIOOperation.
//
// Mutex-guarded rather than lock-free to start (see build plan Phase 3);
// the critical sections are short (a memcpy or two) so contention is not
// expected to be audible at typical buffer sizes.

#ifndef InnerLoopRingBuffer_h
#define InnerLoopRingBuffer_h

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <CoreAudio/CoreAudioTypes.h>

typedef struct
{
    Float32*        mData;          // mFrameCapacity * mChannelCount samples
    UInt32          mFrameCapacity;
    UInt32          mChannelCount;
    UInt64          mWriteHead;     // total frames ever written
    UInt64          mReadHead;      // total frames ever read
    bool            mHasBeenWrittenTo;
    pthread_mutex_t mMutex;
} InnerLoopRingBuffer;

// Allocates mData and initializes state. Returns false on allocation failure.
bool InnerLoopRingBuffer_Init(InnerLoopRingBuffer* ioBuffer, UInt32 inFrameCapacity, UInt32 inChannelCount);

// Frees mData and destroys the mutex.
void InnerLoopRingBuffer_Destroy(InnerLoopRingBuffer* ioBuffer);

// Resets read/write heads to zero and clears the buffer to silence.
// Call on StartIO so a stale session's tail doesn't leak into a new one.
void InnerLoopRingBuffer_Reset(InnerLoopRingBuffer* ioBuffer);

// Writes inFrameCount frames of interleaved audio starting at the current
// write head, advancing it. Always succeeds (older data is overwritten if
// the reader has fallen behind by more than the buffer's capacity).
void InnerLoopRingBuffer_Write(InnerLoopRingBuffer* ioBuffer, const Float32* inData, UInt32 inFrameCount);

// Reads inFrameCount frames starting at the current read head into outData,
// advancing it. If the writer hasn't produced that many frames yet (or
// nothing at all), the missing frames are filled with silence rather than
// stale/garbage data.
void InnerLoopRingBuffer_Read(InnerLoopRingBuffer* ioBuffer, Float32* outData, UInt32 inFrameCount);

#endif /* InnerLoopRingBuffer_h */
