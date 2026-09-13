/* SPDX-License-Identifier: CC0-1.0
 * The VST2 plug-in interface, as much of it as a host needs to drive an
 * instrument. Layout only - no Steinberg code. */
#ifndef SCVA_VST2_H
#define SCVA_VST2_H
#include <stdint.h>

typedef struct AEffect AEffect;
typedef intptr_t (*audioMasterCallback)(AEffect *, int32_t, int32_t, intptr_t, void *, float);
typedef intptr_t (*DispatcherProc)(AEffect *, int32_t, int32_t, intptr_t, void *, float);
typedef void (*ProcessProc)(AEffect *, float **, float **, int32_t);
typedef void (*SetParameterProc)(AEffect *, int32_t, float);
typedef float (*GetParameterProc)(AEffect *, int32_t);

struct AEffect {
  int32_t magic;                 /* 'VstP' */
  DispatcherProc dispatcher;
  ProcessProc process;
  SetParameterProc setParameter;
  GetParameterProc getParameter;
  int32_t numPrograms, numParams, numInputs, numOutputs, flags;
  intptr_t resvd1, resvd2;
  int32_t initialDelay, realQualities, offQualities;
  float ioRatio;
  void *object, *user;
  int32_t uniqueID, version;
  ProcessProc processReplacing;
  void *processDoubleReplacing;
  char future[56];
};

/* to the plug-in */
enum { effOpen=0, effClose=1, effSetProgram=2, effSetSampleRate=10,
       effSetBlockSize=11, effMainsChanged=12, effProcessEvents=25,
       effStartProcess=71, effStopProcess=72 };
/* to the host */
enum { amVersion=1, amCurrentId=2, amIdle=3, amGetTime=7, amProcessEvents=8,
       amGetSampleRate=16, amGetBlockSize=17, amGetCurrentProcessLevel=23,
       amGetVendorString=32, amGetProductString=33, amGetVendorVersion=34,
       amCanDo=37, amGetDirectory=41, amUpdateDisplay=42 };
enum { effFlagsCanReplacing = 1 << 4 };

typedef struct {
  int32_t type;          /* 1 = MIDI */
  int32_t byteSize;
  int32_t deltaFrames;
  int32_t flags;
  int32_t noteLength, noteOffset;
  char midiData[4];
  char detune, noteOffVelocity, reserved1, reserved2;
} VstMidiEvent;

typedef struct {
  int32_t type;          /* 6 = sysex */
  int32_t byteSize;
  int32_t deltaFrames;
  int32_t flags;
  int32_t dumpBytes;
  intptr_t resvd1;
  char *sysexDump;
  intptr_t resvd2;
} VstMidiSysexEvent;

typedef struct {
  int32_t numEvents;
  intptr_t reserved;
  void *events[1];       /* numEvents pointers follow */
} VstEvents;
#endif
