#include "InnerLoopRingBuffer.h"

#include <stdlib.h>
#include <string.h>

bool InnerLoopRingBuffer_Init(InnerLoopRingBuffer* ioBuffer, UInt32 inFrameCapacity, UInt32 inChannelCount)
{
    memset(ioBuffer, 0, sizeof(InnerLoopRingBuffer));

    ioBuffer->mData = (Float32*)calloc((size_t)inFrameCapacity * inChannelCount, sizeof(Float32));
    if (ioBuffer->mData == NULL)
    {
        return false;
    }

    ioBuffer->mFrameCapacity = inFrameCapacity;
    ioBuffer->mChannelCount = inChannelCount;
    ioBuffer->mWriteHead = 0;
    ioBuffer->mReadHead = 0;
    ioBuffer->mHasBeenWrittenTo = false;

    pthread_mutex_init(&ioBuffer->mMutex, NULL);
    return true;
}

void InnerLoopRingBuffer_Destroy(InnerLoopRingBuffer* ioBuffer)
{
    pthread_mutex_destroy(&ioBuffer->mMutex);
    free(ioBuffer->mData);
    ioBuffer->mData = NULL;
}

void InnerLoopRingBuffer_Reset(InnerLoopRingBuffer* ioBuffer)
{
    pthread_mutex_lock(&ioBuffer->mMutex);
    memset(ioBuffer->mData, 0, (size_t)ioBuffer->mFrameCapacity * ioBuffer->mChannelCount * sizeof(Float32));
    ioBuffer->mWriteHead = 0;
    ioBuffer->mReadHead = 0;
    ioBuffer->mHasBeenWrittenTo = false;
    pthread_mutex_unlock(&ioBuffer->mMutex);
}

// Copies inFrameCount frames into mData starting at byte offset
// (inStartFrame % mFrameCapacity), wrapping around the end of the
// buffer as needed. Caller holds mMutex.
static void CopyFramesIn(InnerLoopRingBuffer* ioBuffer, const Float32* inData, UInt64 inStartFrame, UInt32 inFrameCount)
{
    const UInt32 channels = ioBuffer->mChannelCount;
    const UInt32 capacity = ioBuffer->mFrameCapacity;
    UInt32 startIndex = (UInt32)(inStartFrame % capacity);

    UInt32 framesUntilWrap = capacity - startIndex;
    UInt32 firstChunk = (inFrameCount < framesUntilWrap) ? inFrameCount : framesUntilWrap;
    UInt32 secondChunk = inFrameCount - firstChunk;

    memcpy(ioBuffer->mData + (size_t)startIndex * channels, inData, (size_t)firstChunk * channels * sizeof(Float32));
    if (secondChunk > 0)
    {
        memcpy(ioBuffer->mData, inData + (size_t)firstChunk * channels, (size_t)secondChunk * channels * sizeof(Float32));
    }
}

static void CopyFramesOut(InnerLoopRingBuffer* ioBuffer, Float32* outData, UInt64 inStartFrame, UInt32 inFrameCount)
{
    const UInt32 channels = ioBuffer->mChannelCount;
    const UInt32 capacity = ioBuffer->mFrameCapacity;
    UInt32 startIndex = (UInt32)(inStartFrame % capacity);

    UInt32 framesUntilWrap = capacity - startIndex;
    UInt32 firstChunk = (inFrameCount < framesUntilWrap) ? inFrameCount : framesUntilWrap;
    UInt32 secondChunk = inFrameCount - firstChunk;

    memcpy(outData, ioBuffer->mData + (size_t)startIndex * channels, (size_t)firstChunk * channels * sizeof(Float32));
    if (secondChunk > 0)
    {
        memcpy(outData + (size_t)firstChunk * channels, ioBuffer->mData, (size_t)secondChunk * channels * sizeof(Float32));
    }
}

void InnerLoopRingBuffer_Write(InnerLoopRingBuffer* ioBuffer, const Float32* inData, UInt32 inFrameCount)
{
    pthread_mutex_lock(&ioBuffer->mMutex);

    if (inFrameCount > ioBuffer->mFrameCapacity)
    {
        // Caller handed us more than we can ever hold in one shot; only the
        // tail end of it is still relevant once it lands in the ring.
        inData += (size_t)(inFrameCount - ioBuffer->mFrameCapacity) * ioBuffer->mChannelCount;
        inFrameCount = ioBuffer->mFrameCapacity;
    }

    CopyFramesIn(ioBuffer, inData, ioBuffer->mWriteHead, inFrameCount);
    ioBuffer->mWriteHead += inFrameCount;
    ioBuffer->mHasBeenWrittenTo = true;

    // If the writer has lapped the reader, snap the reader forward so it
    // never reads a stretch that's already been overwritten.
    UInt64 framesAvailable = ioBuffer->mWriteHead - ioBuffer->mReadHead;
    if (framesAvailable > ioBuffer->mFrameCapacity)
    {
        ioBuffer->mReadHead = ioBuffer->mWriteHead - ioBuffer->mFrameCapacity;
    }

    pthread_mutex_unlock(&ioBuffer->mMutex);
}

void InnerLoopRingBuffer_Read(InnerLoopRingBuffer* ioBuffer, Float32* outData, UInt32 inFrameCount)
{
    pthread_mutex_lock(&ioBuffer->mMutex);

    if (!ioBuffer->mHasBeenWrittenTo)
    {
        // Nothing has ever been written (output stream not yet running) —
        // hand back silence rather than uninitialized/stale data.
        memset(outData, 0, (size_t)inFrameCount * ioBuffer->mChannelCount * sizeof(Float32));
        pthread_mutex_unlock(&ioBuffer->mMutex);
        return;
    }

    UInt64 framesAvailable = (ioBuffer->mWriteHead > ioBuffer->mReadHead) ? (ioBuffer->mWriteHead - ioBuffer->mReadHead) : 0;

    if (framesAvailable >= inFrameCount)
    {
        CopyFramesOut(ioBuffer, outData, ioBuffer->mReadHead, inFrameCount);
        ioBuffer->mReadHead += inFrameCount;
    }
    else
    {
        // Underrun: the writer hasn't kept pace. Read what's actually
        // available and pad the remainder with silence, then advance the
        // read head only for what actually existed so we don't run ahead
        // of the writer permanently.
        UInt32 available = (UInt32)framesAvailable;
        if (available > 0)
        {
            CopyFramesOut(ioBuffer, outData, ioBuffer->mReadHead, available);
            ioBuffer->mReadHead += available;
        }
        memset(outData + (size_t)available * ioBuffer->mChannelCount, 0, (size_t)(inFrameCount - available) * ioBuffer->mChannelCount * sizeof(Float32));
    }

    pthread_mutex_unlock(&ioBuffer->mMutex);
}
