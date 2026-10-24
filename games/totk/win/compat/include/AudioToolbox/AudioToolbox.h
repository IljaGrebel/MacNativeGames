/* Stand-in for the part of AudioToolbox the upstream mixer uses: one output unit that pulls stereo float frames from
   a render callback. compat_audio.c drives the callback from the port's audio output (a silent clock in M0, the
   system's output device later); the mixer itself (runtime/audio_mixer.c) is unchanged. */
#pragma once
#include <stdint.h>
typedef int32_t OSStatus;
typedef uint32_t UInt32;
typedef double Float64;
typedef uint32_t AudioUnitRenderActionFlags;
#define noErr 0
typedef struct AudioTimeStamp { Float64 mSampleTime; uint64_t mHostTime; } AudioTimeStamp;
typedef struct AudioBuffer { UInt32 mNumberChannels, mDataByteSize; void *mData; } AudioBuffer;
typedef struct AudioBufferList { UInt32 mNumberBuffers; AudioBuffer mBuffers[1]; } AudioBufferList;
typedef OSStatus (*AURenderCallback)(void *, AudioUnitRenderActionFlags *, const AudioTimeStamp *, UInt32, UInt32, AudioBufferList *);
typedef struct AURenderCallbackStruct { AURenderCallback inputProc; void *inputProcRefCon; } AURenderCallbackStruct;
typedef struct AudioStreamBasicDescription {
    Float64 mSampleRate; UInt32 mFormatID, mFormatFlags, mBytesPerPacket, mFramesPerPacket, mBytesPerFrame,
        mChannelsPerFrame, mBitsPerChannel, mReserved;
} AudioStreamBasicDescription;
typedef struct AudioComponentDescription { UInt32 componentType, componentSubType, componentManufacturer, flags, mask; } AudioComponentDescription;
typedef struct CompatAudioUnit *AudioComponentInstance, *AudioUnit;
typedef const struct CompatAudioComponent *AudioComponent;
#define kAudioUnitType_Output 0x61756f75
#define kAudioUnitSubType_DefaultOutput 0x64656620
#define kAudioUnitSubType_RemoteIO 0x72696f63
#define kAudioUnitManufacturer_Apple 0x6170706c
#define kAudioFormatLinearPCM 0x6c70636d
#define kAudioFormatFlagIsFloat 1
#define kAudioFormatFlagIsPacked 8
#define kAudioUnitProperty_StreamFormat 8
#define kAudioUnitProperty_SetRenderCallback 23
#define kAudioUnitScope_Input 1
AudioComponent AudioComponentFindNext(AudioComponent after, const AudioComponentDescription *d);
OSStatus AudioComponentInstanceNew(AudioComponent c, AudioComponentInstance *out);
OSStatus AudioComponentInstanceDispose(AudioComponentInstance u);
OSStatus AudioUnitSetProperty(AudioUnit u, UInt32 id, UInt32 scope, UInt32 element, const void *data, UInt32 size);
OSStatus AudioUnitInitialize(AudioUnit u);
OSStatus AudioUnitUninitialize(AudioUnit u);
OSStatus AudioOutputUnitStart(AudioUnit u);
OSStatus AudioOutputUnitStop(AudioUnit u);
