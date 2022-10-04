/*
* This file provides the interface to the JavaScript world (i.e. it
* provides all the APIs expected by the "backend adapter").
*
* It also handles the output audio buffer logic.
*
*
* The general coding conventions used in this project are:
*
*  - type names use camelcase & start uppercase
*  - method/function names use camelcase & start lowercase
*  - vars use lowercase and _ delimiters
*  - private vars (instance vars, etc) start with _
*  - global constants and defines are all uppercase
*  - cross C-file APIs (for those older parts that still use C)
*    start with a prefix that reflects the file that provides them,
*    e.g. "vic...()" is provided by "vic.c".
*
* WebSid (c) 2019 Jürgen Wothke
* version 0.93
*
* Terms of Use: This software is licensed under a CC BY-NC-SA
* (http://creativecommons.org/licenses/by-nc-sa/4.0/).
*/

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#ifdef EMSCRIPTEN
#include <emscripten.h>
#else
#define EMSCRIPTEN_KEEPALIVE
#endif

extern "C" {
#include "system.h"
#include "vic.h"
#include "core.h"
#include "memory.h"

#include "stereo/LVCS.h"
}
#include "filter6581.h"
#include "sid.h"
extern "C" uint8_t	sidReadMem(uint16_t addr);
extern "C" void 	sidWriteMem(uint16_t addr, uint8_t value);
extern "C" uint8_t	sidReadVoiceLevel(uint8_t sid_idx, uint8_t voice_idx);
extern "C" void		sidSetPanning(uint8_t sid_idx, uint8_t voice_idx, float panning);


#include "loaders.h"

static FileLoader*	_loader;


// ------ stereo postprocessing ---------------------
	// variable settings
static int32_t _effect_level = -1; // stereo disabled by default;	16384=low 32767=LVCS_EFFECT_HIGH
static LVM_UINT16 _reverb_level = 100;
static LVCS_SpeakerType_en _speaker_type= LVCS_HEADPHONES; // LVCS_EX_HEADPHONES

	// base lib data types
static LVCS_Handle_t _lvcs_handle = LVM_NULL;	// just a typecast PTR to be later set to the above instance
static LVCS_MemTab_t _lvcs_mem_tab;
static LVCS_Capabilities_t _lvcs_caps;
static LVCS_Params_t _lvcs_params;


// --------- audio output buffer management ------------------------

// WebAudio side processor buffer size
static uint32_t _procBufSize = 0;

// keep it down to one screen to allow for
// more direct feedback to WebAudio side:
#define BUFLEN 96000/50
#define CHANNELS 2

static int16_t 		_soundBuffer[BUFLEN * CHANNELS];

// max 10 sids*4 voices (1 digi channel)
#define MAX_SIDS 			10
#define MAX_VOICES 			40
#define MAX_SCOPE_BUFFERS 	40

// output "scope" streams corresponding to final audio buffer
static int16_t* 	_scope_buffers[MAX_SCOPE_BUFFERS];

// these buffers are "per frame" i.e. 1 screen refresh, e.g. 822 samples
static int16_t* 	_synth_buffer = 0;
static int16_t** 	_synth_trace_buffers = 0;

static uint16_t 	_chunk_size; 	// number of samples per call

static uint32_t 	_number_of_samples_rendered = 0;
static uint32_t 	_number_of_samples_to_render = 0;

static uint8_t	 	_sound_started;
static uint8_t	 	_skip_silence_loop;

static uint32_t		_sample_rate;

static uint32_t		_trace_sid = 0;
static uint8_t		_ready_to_play = 0;


static float 	_panning[] = {	// panning per SID/voice (max 10 SIDs..)
	0.5, 0.4, 0.6,
	0.5, 0.6, 0.4,
	0.5, 0.4, 0.6,
	0.5, 0.6, 0.4,
	0.5, 0.4, 0.6,
	0.5, 0.6, 0.4,
	0.5, 0.4, 0.6,
	0.5, 0.6, 0.4,
	0.5, 0.4, 0.6,
	0.5, 0.6, 0.4,
	};

static float 	_no_panning[] = {
	0.5, 0.5, 0.5,
	0.5, 0.5, 0.5,
	0.5, 0.5, 0.5,
	0.5, 0.5, 0.5,
	0.5, 0.5, 0.5,
	0.5, 0.5, 0.5,
	0.5, 0.5, 0.5,
	0.5, 0.5, 0.5,
	0.5, 0.5, 0.5,
	0.5, 0.5, 0.5,
	};

extern "C" void initPanningCfg(float p0, float p1, float p2, float p3, float p4, float p5, float p6, float p7, float p8, float p9,
								float p10, float p11, float p12, float p13, float p14, float p15, float p16, float p17, float p18, float p19,
								float p20, float p21, float p22, float p23, float p24, float p25, float p26, float p27, float p28, float p29) __attribute__((noinline));
extern "C" void initPanningCfg(float p0, float p1, float p2, float p3, float p4, float p5, float p6, float p7, float p8, float p9,
								float p10, float p11, float p12, float p13, float p14, float p15, float p16, float p17, float p18, float p19,
								float p20, float p21, float p22, float p23, float p24, float p25, float p26, float p27, float p28, float p29) {
	_panning[0] = p0;
	_panning[1] = p1;
	_panning[2] = p2;
	_panning[3] = p3;
	_panning[4] = p4;
	_panning[5] = p5;
	_panning[6] = p6;
	_panning[7] = p7;
	_panning[8] = p8;
	_panning[9] = p9;
	_panning[10] = p10;
	_panning[11] = p11;
	_panning[12] = p12;
	_panning[13] = p13;
	_panning[14] = p14;
	_panning[15] = p15;
	_panning[16] = p16;
	_panning[17] = p17;
	_panning[18] = p18;
	_panning[19] = p19;
	_panning[20] = p20;
	_panning[11] = p21;
	_panning[22] = p22;
	_panning[23] = p23;
	_panning[24] = p24;
	_panning[25] = p25;
	_panning[26] = p26;
	_panning[27] = p27;
	_panning[28] = p28;
	_panning[29] = p29;
}

#define MAX_SIDS 10				// absolute maximum currently used in the most exotic song

void enablePanning(uint8_t on) {
	for (int sid_idx= 0; sid_idx<MAX_SIDS; sid_idx++) {
		for (int voice_idx= 0; voice_idx<3; voice_idx++) {
			sidSetPanning(sid_idx, voice_idx, on ? _panning[sid_idx*3 + voice_idx] : 0.5);
		}
	}
}

extern "C" float getPanning(uint8_t sid_idx, uint8_t voice_idx) __attribute__((noinline));
extern "C" float getPanning(uint8_t sid_idx, uint8_t voice_idx) {
	if ((sid_idx < MAX_SIDS) && (voice_idx < 3))
		return _panning[sid_idx*3 + voice_idx];
	return -1;
}

extern "C" void setPanning(uint8_t sid_idx, uint8_t voice_idx, float panning) __attribute__((noinline));
extern "C" void setPanning(uint8_t sid_idx, uint8_t voice_idx, float panning) {
	if ((sid_idx < MAX_SIDS) && (voice_idx < 3)) {
		_panning[sid_idx*3 + voice_idx]= panning;

		enablePanning(_effect_level >= 0);
	}
}

// SID register recordings: in order to allow for "sufficiently accurate" SID register visualizations, the
// below circular buffers are used to record SID-snapshots in 50/60Hz (i.e. 1 frame) intervals (this should be
// good enough for applications like DeepSid's piano-view).

// hack: In order to avoid later re-shuffling/processing of this potentially unused data, the buffers used here
// are sized to relate to respective double buffered WebAudio audio buffers: The maximum buffer size used by WebAudio
// is 16384 - depending on the used sample rate this would supply 0.37s (or less) od audio data, e.g.
	// NTSC: 735*60=44100		800*60=48000
	// PAL: 882*50=44100		960*50=48000

#define REGS2RECORD (25 + 3)	// only the first 25 regs (trailing paddle regs (etc) are ignored) - but adding "envelope levels" of all three voices


static uint8_t** _sidRegSnapshots = 0;
static uint32_t _sidSnapshotSmplCount = 0;
static uint32_t _sidSnapshotToggle = 0;

static uint16_t _sidRegSnapshotAlloc = 0;
static uint32_t _sidRegSnapshotPos = 0;
static uint16_t _sidRegSnapshotMax = 0;


static void initSidRegSnapshotBuffers() {
	_sidSnapshotSmplCount = 0;

	if (!_sidRegSnapshots) {
		_sidRegSnapshots = (uint8_t**) calloc(MAX_SIDS, sizeof(uint8_t*));
	}

	uint16_t nSnapshots = (uint16_t)ceil((float)_procBufSize / _chunk_size);	// interval different from UI's "ticks" based calcs

	if (_sidRegSnapshotAlloc < nSnapshots) {
		for (uint8_t i = 0; i<MAX_SIDS; i++) {
			if (_sidRegSnapshots[i]) free(_sidRegSnapshots[i]);

			_sidRegSnapshots[i] = (uint8_t*)calloc(REGS2RECORD, nSnapshots * 2);	// double buffer the duration of WebAudio buffer
		}

		_sidRegSnapshotAlloc = nSnapshots;
	} else {
		// just leave excess size unused
	}
	_sidRegSnapshotMax = nSnapshots;
	_sidRegSnapshotPos = 0;
	_sidSnapshotToggle = 0;
}

extern "C" uint16_t getSIDRegister(uint8_t sidIdx, uint16_t reg) __attribute__((noinline));

static void recordSidRegSnapshot() {
	uint32_t offset = _sidRegSnapshotPos * REGS2RECORD;
	for (uint8_t i = 0; i < SID::getNumberUsedChips(); i++) {
		uint8_t* sidBuf = &(_sidRegSnapshots[i][offset]);

		uint16_t j;
		for (j = 0; j < REGS2RECORD-3; j++) {
			sidBuf[j] = getSIDRegister(i, j);
		}
		// save "envelope" levels of all three voices
		sidBuf[j++] = sidReadVoiceLevel(i, 0);
		sidBuf[j++] = sidReadVoiceLevel(i, 1);
		sidBuf[j] = sidReadVoiceLevel(i, 2);
	}

	_sidSnapshotSmplCount+= _chunk_size;

	// setup next target buffer location
	if (_sidSnapshotSmplCount >= _procBufSize) {
		if (!_sidSnapshotToggle) {
			_sidSnapshotToggle = 1;	// switch to 2nd buffer
			_sidRegSnapshotPos = _sidRegSnapshotMax;
		} else {
			_sidSnapshotToggle = 0;	// switch to 1st buffer
			_sidRegSnapshotPos = 0;
		}
		_sidSnapshotSmplCount -= _procBufSize; // track overflow

	} else {
		_sidRegSnapshotPos++;
	}
}

// gets snapshot SID state relating to specified playback time
extern "C" uint16_t getSIDRegister2(uint8_t sidIdx, uint16_t reg, uint8_t bufIdx, uint32_t tick) __attribute__((noinline));
extern "C" uint16_t EMSCRIPTEN_KEEPALIVE getSIDRegister2(uint8_t sidIdx, uint16_t reg, uint8_t bufIdx, uint32_t tick) {

	if (reg < (REGS2RECORD-3)) {
		// cached snapshots are spaced "1 frame" apart while WebAudio-side measures time in 256-sample ticks..
		// map the respective input to the corresponding cache block (the imprecision should not be relevant
		// for the actual use cases.. see "piano view" in DeepSid)

		uint8_t* sidBuf = &(_sidRegSnapshots[sidIdx][bufIdx ? _sidRegSnapshotMax*REGS2RECORD : 0]);
		uint32_t idx = (tick << 8) / _chunk_size;

		sidBuf += idx * REGS2RECORD;
		return sidBuf[reg];
	} else {
		// fallback to latest state of emulator
		return getSIDRegister(sidIdx, reg);
	}
}

extern "C" uint16_t readVoiceLevel(uint8_t sidIdx, uint8_t voiceIdx, uint8_t bufIdx, uint32_t tick) __attribute__((noinline));
extern "C" uint16_t EMSCRIPTEN_KEEPALIVE readVoiceLevel(uint8_t sidIdx, uint8_t voiceIdx, uint8_t bufIdx, uint32_t tick) {

	uint8_t* sidBuf = &(_sidRegSnapshots[sidIdx][bufIdx ? _sidRegSnapshotMax*REGS2RECORD : 0]);
	uint32_t idx = (tick << 8) / _chunk_size;
	sidBuf += idx * REGS2RECORD;

	return 	sidBuf[REGS2RECORD -3 + voiceIdx];
}

static void resetTimings(uint8_t is_ntsc) {
	vicSetModel(is_ntsc);	// see for timing details

	uint32_t clock_rate= 	sysGetClockRate(is_ntsc);
	uint8_t is_rsid=		FileLoader::isRSID();
	uint8_t is_compatible=	FileLoader::getCompatibility();

	SID::resetAll(_sample_rate, clock_rate, is_rsid, is_compatible);
}

static void resetScopeBuffers() {
	if (_scope_buffers[0] == 0) {
		// alloc once
		for (int i= 0; i<MAX_SCOPE_BUFFERS; i++) {
			_scope_buffers[i] = (int16_t*) calloc(BUFLEN, sizeof(int16_t));
		}
	} else {
		for (int i= 0; i<MAX_SCOPE_BUFFERS; i++) {
			// just to make sure there is no garbage left
			memset(_scope_buffers[i], 0, sizeof(int16_t)*BUFLEN);
		}
	}
}

static void resetSynthBuffer(uint16_t size) {
	if (_synth_buffer) free(_synth_buffer);

	_synth_buffer= (int16_t*)malloc(sizeof(int16_t)*
						(size * CHANNELS + 1));
}

static void discardSynthTraceBuffers() {
	// trace output (corresponding to _synth_buffer)
	if (_synth_trace_buffers) {
		for (int i= 0; i<MAX_VOICES; i++) {
			if (_synth_trace_buffers[i]) {
				free(_synth_trace_buffers[i]);
				_synth_trace_buffers[i] = 0;
			}
		}
		free(_synth_trace_buffers);
		_synth_trace_buffers = 0;
	}
}

static void allocSynthTraceBuffers(uint16_t size) {
	// availability of _synth_trace_buffers controls
	// if SID will generate the respective output

	if (_trace_sid) {
		if (!_synth_trace_buffers) {
			_synth_trace_buffers = (int16_t**)calloc(sizeof(int16_t*), MAX_VOICES);
		}
		for (int i= 0; i<MAX_VOICES; i++) {
			_synth_trace_buffers[i] = (int16_t*)calloc(sizeof(int16_t), size + 1);
		}

	} else {
		if (_synth_trace_buffers) {
			for (int i= 0; i<MAX_VOICES; i++) {
				if (_synth_trace_buffers[i]) {
					free(_synth_trace_buffers[i]);
					_synth_trace_buffers[i] = 0;
				}
			}
			free(_synth_trace_buffers);
			_synth_trace_buffers = 0; // disables respective SID rendering
		}
	}
}

static void resetSynthTraceBuffers(uint16_t size) {
	discardSynthTraceBuffers();
	allocSynthTraceBuffers(size);
}


static void resetAudioBuffers() {

	// number of samples corresponding to one simulated frame/
	// screen (granularity of emulation is 1 screen), e.g.
	// NTSC: 735*60=44100		800*60=48000
	// PAL: 882*50=44100		960*50=48000

	_chunk_size = _sample_rate / vicFramesPerSecond();

	resetScopeBuffers();
	resetSynthBuffer(_chunk_size);
	resetSynthTraceBuffers(_chunk_size);

	_number_of_samples_rendered = 0;
	_number_of_samples_to_render = 0;

	initSidRegSnapshotBuffers();
}

extern "C" uint8_t envSetNTSC(uint8_t is_ntsc)  __attribute__((noinline));
extern "C" uint8_t EMSCRIPTEN_KEEPALIVE envSetNTSC(uint8_t is_ntsc) {
	resetTimings(is_ntsc);
	resetAudioBuffers();

	return 0;
}

// ----------------- generic handling -----------------------------------------

// This is driving the emulation: Each call to computeAudioSamples() delivers
// some fixed numberof audio samples and the necessary emulation timespan is
// derived from it:

inline void applyStereoEnhance() {
	uint32_t s;
	if((_effect_level > 0) && (s = LVCS_Process(_lvcs_handle, (const LVM_INT16*)_synth_buffer, _synth_buffer, _chunk_size))) {
		fprintf(stderr, "error: LVCS_Process %lu %hu\n", s, _chunk_size);
	}
}

extern "C" int32_t computeAudioSamples()  __attribute__((noinline));
extern "C" int32_t EMSCRIPTEN_KEEPALIVE computeAudioSamples() {
	if(!_ready_to_play) return 0;

	uint8_t is_simple_sid_mode =	!FileLoader::isExtendedSidFile();
	int sid_voices =				SID::getNumberUsedChips() * 4;
	uint8_t speed =					FileLoader::getCurrentSongSpeed();

#ifdef TEST
	return 0;
#endif
	_number_of_samples_rendered = 0;

	uint32_t sample_buffer_idx = 0;

	while (_number_of_samples_rendered < _chunk_size) {
		if (_number_of_samples_to_render == 0) {
			_number_of_samples_to_render = _chunk_size;
			sample_buffer_idx = 0;

			// limit "skipping" so as not to make the browser unresponsive
			for (uint16_t i= 0; i<_skip_silence_loop; i++) {

				Core::runOneFrame(is_simple_sid_mode, speed, _synth_buffer,
									_synth_trace_buffers, _chunk_size);

				if (!_sound_started) {
					if (SID::isAudible()) {
						_sound_started = 1;

						applyStereoEnhance();
						break;
					}
				} else {
					applyStereoEnhance();
					break;
				}
			}
		}

		if (_number_of_samples_rendered + _number_of_samples_to_render > _chunk_size) {
			uint32_t available_space = _chunk_size-_number_of_samples_rendered;

			memcpy(	&_soundBuffer[_number_of_samples_rendered],
					&_synth_buffer[sample_buffer_idx],
					sizeof(int16_t) * available_space * CHANNELS);

			// In addition to the actual sample data played by WebAudio, buffers
			// containing raw voice data are also created here. These are 1:1 in
			// sync with the sample buffer, i.e. for each sample entry in the
			// sample buffer there is a corresponding entry in the additional
			// buffers - which are all exactly the same size as the sample buffer.

			if (_trace_sid) {
				// do the same for the respective voice traces
				for (int i= 0; i<sid_voices; i++) {
					if (is_simple_sid_mode || (sid_voices % 4) != 3) {	// no digi
						memcpy(	&(_scope_buffers[i][_number_of_samples_rendered]),
								&(_synth_trace_buffers[i][sample_buffer_idx]),
								sizeof(int16_t) * available_space);
					}
				}
			}
			sample_buffer_idx += available_space;
			_number_of_samples_to_render -= available_space;
			_number_of_samples_rendered = _chunk_size;
		} else {
			memcpy(	&_soundBuffer[_number_of_samples_rendered],
					&_synth_buffer[sample_buffer_idx],
					sizeof(int16_t) * CHANNELS * _number_of_samples_to_render);

			if (_trace_sid) {
				// do the same for the respecive voice traces
				for (int i= 0; i<sid_voices; i++) {
					if (is_simple_sid_mode || (sid_voices % 4) != 3) {	// no digi
						memcpy(	&(_scope_buffers[i][_number_of_samples_rendered]),
								&(_synth_trace_buffers[i][sample_buffer_idx]),
								sizeof(int16_t) * _number_of_samples_to_render);
					}
				}
			}
			_number_of_samples_rendered += _number_of_samples_to_render;
			_number_of_samples_to_render = 0;
		}
	}

	recordSidRegSnapshot();

	if (_loader->isTrackEnd()) { // "play" must have been called before 1st use of this check
		return -1;
	}

	return _number_of_samples_rendered;
}


extern "C" uint32_t enableVoice(uint8_t sid_idx, uint8_t voice, uint8_t on)  __attribute__((noinline));
extern "C" uint32_t EMSCRIPTEN_KEEPALIVE enableVoice(uint8_t sid_idx, uint8_t voice, uint8_t on) {
	SID::setMute(sid_idx, voice, !on);

	return 0;
}

LVM_Fs_en getSampleRateEn(uint32_t sample_rate) {
	switch (sample_rate) {
		case 8000:
			return LVM_FS_8000;
		case 11025:
			return LVM_FS_11025;
		case 12000:
			return LVM_FS_12000;
		case 16000:
			return LVM_FS_16000;
		case 22050:
			return LVM_FS_22050;
		case 24000:
			return LVM_FS_24000;
		case 32000:
			return LVM_FS_32000;
		case 44100:
			return LVM_FS_44100;
		case 48000:
			return LVM_FS_48000;
		default:
			fprintf(stderr, "warning: samplerate (%lu) is not supported by pseudo stereo impl.", sample_rate);
			return LVM_FS_DUMMY;	// higher sample rates should not be used here

	}
}

void configurePseudoStereo() {
	if (_lvcs_handle == LVM_NULL) {
		// capabilities used for LVCS_Memory and LVCS_Init must be the same!
		_lvcs_caps.MaxBlockSize= _chunk_size;
		_lvcs_caps.CallBack= LVM_NULL;

		if (LVCS_Memory(LVM_NULL, &_lvcs_mem_tab, &_lvcs_caps)) {	// orig code patched to alloc used buffers!
			fprintf(stderr, "error: LVCS_Memory\n");
		}

		if (LVCS_Init(&_lvcs_handle, &_lvcs_mem_tab, &_lvcs_caps)) {
			fprintf(stderr, "error: LVCS_Init\n");
		}
	}

	// caution: LVCS_GetParameters is a garbage API: changing the returned "ref to original" will cause any
	// changes to be ignored by LVCS_Control() since there is no difference to the "original". use a
	// separage copy for the input params:

	//#define LVCS_STEREOENHANCESWITCH    0x0001      /* Stereo enhancement enable control */
	//#define LVCS_REVERBSWITCH           0x0002      /* Reverberation enable control */
	//#define LVCS_EQUALISERSWITCH        0x0004      /* Equaliser enable control */
	//#define LVCS_BYPASSMIXSWITCH        0x0008      /* Bypass mixer enable control */
	_lvcs_params.OperatingMode = LVCS_ON;	// LVCS_ON means that all the above 4 bits are set

	_lvcs_params.EffectLevel    = _effect_level < 0 ? 0 : _effect_level;
	_lvcs_params.ReverbLevel    = _reverb_level; // supposedly in %!
	_lvcs_params.SpeakerType = _speaker_type;

	_lvcs_params.SourceFormat = LVCS_STEREO;		// with "per voice panning" input signal is "always" stereo
	_lvcs_params.CompressorMode = LVM_MODE_OFF;
	_lvcs_params.SampleRate = getSampleRateEn(_sample_rate);

	if (LVCS_Control(_lvcs_handle, &_lvcs_params)) {
		fprintf(stderr, "error: LVCS_Control\n");
	}
}

extern "C" int32_t getStereoLevel()  __attribute__((noinline));
extern "C" int32_t EMSCRIPTEN_KEEPALIVE getStereoLevel() {
	return _effect_level;
}
extern "C" void  setStereoLevel(int32_t effect_level)  __attribute__((noinline));
extern "C" void EMSCRIPTEN_KEEPALIVE setStereoLevel(int32_t effect_level) {
	_effect_level= effect_level;
	configurePseudoStereo();

	enablePanning(_effect_level >= 0);
}
extern "C" LVM_UINT16 getReverbLevel()  __attribute__((noinline));
extern "C" LVM_UINT16 EMSCRIPTEN_KEEPALIVE getReverbLevel() {
	return _reverb_level;
}
extern "C" void  setReverbLevel(LVM_UINT16 reverb_level)  __attribute__((noinline));
extern "C" void EMSCRIPTEN_KEEPALIVE setReverbLevel(LVM_UINT16 reverb_level) {
	_reverb_level= reverb_level;
	configurePseudoStereo();
}

extern "C" uint8_t getHeadphoneMode()  __attribute__((noinline));
extern "C" uint8_t EMSCRIPTEN_KEEPALIVE getHeadphoneMode() {
	return _speaker_type == LVCS_HEADPHONES ? 0 : 1;
}
extern "C" void setHeadphoneMode(uint8_t mode)  __attribute__((noinline));
extern "C" void EMSCRIPTEN_KEEPALIVE setHeadphoneMode(uint8_t mode) {
	_speaker_type = mode ? LVCS_EX_HEADPHONES : LVCS_HEADPHONES;
	configurePseudoStereo();
}


extern "C" uint32_t playTune(uint32_t selected_track, uint32_t trace_sid, uint32_t procBufSize)  __attribute__((noinline));
extern "C" uint32_t EMSCRIPTEN_KEEPALIVE playTune(uint32_t selected_track, uint32_t trace_sid, uint32_t procBufSize) {
	_ready_to_play = 0;
	_trace_sid = trace_sid;
	_procBufSize = (float) procBufSize;

	_sound_started = 0;

	// note: crappy BASIC songs like Baroque_Music_64_BASIC take 100sec before
	// they even start playing.. unfortunately the emulation is NOT fast enough
	// (at least on my old PC) to just skip that phase in "no time" and if the
	// calculations take too long then they will block the browser completely

	// performing respective skipping-logic directly within INIT is a bad idea
	// since it will completely block the browser until it is completed. From
	// "UI responsiveness" perspective it is preferable to attempt a "speedup"
	// on limited slices from within the audio-rendering loop.

	// should keep the UI responsive; means that on a fast machine the above
	// garbage song will still take 10 secs before it plays
	_skip_silence_loop = 10;

	// XXX FIXME the separate handling of the INIT call is an annoying legacy
	// of the old impl.. respective PSID handling should better to moved into
	// the respective C64 side driver so that users of the emulator do not need
	// to handle this potentially long running emu scenario (see SID callbacks
	// triggered on Raspberry SID device).
	_loader->initTune(_sample_rate, selected_track);

	SID::initPanning(_effect_level >= 0 ? _panning : _no_panning);

	resetAudioBuffers();

	configurePseudoStereo();

	_ready_to_play = 1;

	return 0;
}


extern "C" uint32_t loadSidFile(uint32_t is_mus, void* in_buffer, uint32_t in_buf_size,
								uint32_t sample_rate, char* filename, void* basic_ROM,
								void* char_ROM, void* kernal_ROM)  __attribute__((noinline));
extern "C" uint32_t EMSCRIPTEN_KEEPALIVE loadSidFile(uint32_t is_mus, void* in_buffer, uint32_t in_buf_size,
								uint32_t sample_rate, char* filename, void* basic_ROM,
								void* char_ROM, void* kernal_ROM) {

	_ready_to_play = 0;											// stop any emulator use
    _sample_rate = sample_rate > 48000 ? 48000 : sample_rate; 	// see hardcoded BUFLEN (and "pseudo stereo" limitation)

	_loader = FileLoader::getInstance(is_mus, in_buffer, in_buf_size);

	if (!_loader) return 1;	// error


	uint32_t result = _loader->load((uint8_t *)in_buffer, in_buf_size, filename,
									basic_ROM, char_ROM, kernal_ROM);

	if (!result) {
		uint8_t is_ntsc = FileLoader::getNTSCMode();
		envSetNTSC(is_ntsc);
	}
	return result;
}

extern "C" char** getMusicInfo() __attribute__((noinline));
extern "C" char** EMSCRIPTEN_KEEPALIVE getMusicInfo() {
	return FileLoader::getInfoStrings();
}

extern "C" uint32_t getSoundBufferLen() __attribute__((noinline));
extern "C" uint32_t EMSCRIPTEN_KEEPALIVE getSoundBufferLen() {
	return _number_of_samples_rendered;	// in samples
}

extern "C" char* getSoundBuffer() __attribute__((noinline));
extern "C" char* EMSCRIPTEN_KEEPALIVE getSoundBuffer() {
	return (char*) _soundBuffer;
}

extern "C" uint32_t getSampleRate() __attribute__((noinline));
extern "C" uint32_t EMSCRIPTEN_KEEPALIVE getSampleRate() {
	return _sample_rate;
}

// additional accessors that might be useful for tweaking defaults from the GUI

extern "C" uint8_t envIsSID6581()  __attribute__((noinline));
extern "C" uint8_t EMSCRIPTEN_KEEPALIVE envIsSID6581() {
	return (uint8_t)SID::isSID6581();
}

extern "C" uint8_t envSetSID6581(uint8_t is6581)  __attribute__((noinline));
extern "C" uint8_t EMSCRIPTEN_KEEPALIVE envSetSID6581(uint8_t is6581) {
	return SID::setSID6581((bool)is6581);
}

extern "C" uint8_t getDigiType()  __attribute__((noinline));
extern "C" uint8_t EMSCRIPTEN_KEEPALIVE getDigiType() {
	return SID::getGlobalDigiType();
}

extern "C" const char* getDigiTypeDesc()  __attribute__((noinline));
extern "C" const char* EMSCRIPTEN_KEEPALIVE getDigiTypeDesc() {
	return SID::getGlobalDigiTypeDesc();
}

extern "C" uint16_t getDigiRate()  __attribute__((noinline));
extern "C" uint16_t EMSCRIPTEN_KEEPALIVE getDigiRate() {
	return SID::getGlobalDigiRate()*vicFramesPerSecond();
}

extern "C" uint8_t envIsNTSC()  __attribute__((noinline));
extern "C" uint8_t EMSCRIPTEN_KEEPALIVE envIsNTSC() {
	return FileLoader::getNTSCMode();
}

/**
* @deprected use getSIDRegister instead
*/
extern "C" uint16_t getRegisterSID(uint16_t reg) __attribute__((noinline));
extern "C" uint16_t EMSCRIPTEN_KEEPALIVE getRegisterSID(uint16_t reg) {
	return  reg >= 0x1B ? sidReadMem(0xd400 + reg) : memReadIO(0xd400 + reg);
}

extern "C" void setRegisterSID(uint16_t reg, uint8_t value) __attribute__((noinline));
extern "C" void EMSCRIPTEN_KEEPALIVE setRegisterSID(uint16_t reg, uint8_t value) {
	sidWriteMem(0xd400 + reg, value);
}

extern "C" uint16_t getRAM(uint16_t addr) __attribute__((noinline));
extern "C" uint16_t EMSCRIPTEN_KEEPALIVE getRAM(uint16_t addr) {
	return  memReadRAM(addr);
}

extern "C" void setRAM(uint16_t addr, uint8_t value) __attribute__((noinline));
extern "C" void EMSCRIPTEN_KEEPALIVE setRAM(uint16_t addr, uint8_t value) {
	memWriteRAM(addr, value);
}

extern "C" uint16_t getGlobalDigiType() __attribute__((noinline));
extern "C" uint16_t EMSCRIPTEN_KEEPALIVE getGlobalDigiType() {
	return SID::getGlobalDigiType();
}

extern "C" const char * getGlobalDigiTypeDesc() __attribute__((noinline));
extern "C" const char * EMSCRIPTEN_KEEPALIVE getGlobalDigiTypeDesc() {
	uint16_t t = SID::getGlobalDigiType();
	return (t > 0) ? SID::getGlobalDigiTypeDesc() : "";
}

extern "C" uint16_t getGlobalDigiRate() __attribute__((noinline));
extern "C" uint16_t EMSCRIPTEN_KEEPALIVE getGlobalDigiRate() {
	uint16_t t = SID::getGlobalDigiType();
	return (t > 0) ? SID::getGlobalDigiRate() : 0;
}

extern "C" int countSIDs() __attribute__((noinline));
extern "C" int EMSCRIPTEN_KEEPALIVE countSIDs() {
	return SID::getNumberUsedChips();
}
extern "C" int getSIDBaseAddr(uint8_t sidIdx) __attribute__((noinline));
extern "C" int EMSCRIPTEN_KEEPALIVE getSIDBaseAddr(uint8_t sidIdx) {
	return SID::getSIDBaseAddr(sidIdx);
}

// gets the current state of the emulation
extern "C" uint16_t EMSCRIPTEN_KEEPALIVE getSIDRegister(uint8_t sidIdx, uint16_t reg) {
	return  reg >= 0x1B ? sidReadMem(SID::getSIDBaseAddr(sidIdx) + reg) : memReadIO(SID::getSIDBaseAddr(sidIdx) + reg);
}


extern "C" void setSIDRegister(uint8_t sidIdx, uint16_t reg, uint8_t value) __attribute__((noinline));
extern "C" void EMSCRIPTEN_KEEPALIVE setSIDRegister(uint8_t sidIdx, uint16_t reg, uint8_t value) {
	sidWriteMem(SID::getSIDBaseAddr(sidIdx) + reg, value);
}


extern "C" int getNumberTraceStreams() __attribute__((noinline));
extern "C" int EMSCRIPTEN_KEEPALIVE getNumberTraceStreams() {
	// always use additional stream for digi samples..
	return SID::getNumberUsedChips() * 4;
}
extern "C" const char** getTraceStreams() __attribute__((noinline));
extern "C" const char** EMSCRIPTEN_KEEPALIVE getTraceStreams() {
	return (const char**)_scope_buffers;	// ugly cast to make emscripten happy
}

extern "C" int setFilterConfig6581(double base, double max, double steepness, double x_offset, double distort, double distort_offset, double distort_scale, double distort_threshold, double kink) __attribute__((noinline));
extern "C" int EMSCRIPTEN_KEEPALIVE setFilterConfig6581(double base, double max, double steepness, double x_offset, double distort, double distort_offset, double distort_scale, double distort_threshold, double kink) {
	return Filter6581::setFilterConfig6581(base, max, steepness, x_offset, distort, distort_offset, distort_scale, distort_threshold, kink);
}

extern "C" double* getFilterConfig6581() __attribute__((noinline));
extern "C" double* EMSCRIPTEN_KEEPALIVE getFilterConfig6581() {
	return Filter6581::getFilterConfig6581();
}

extern "C" double* getCutoff6581(int slice) __attribute__((noinline));
extern "C" double* EMSCRIPTEN_KEEPALIVE getCutoff6581(int slice) {
	return Filter6581::getCutoff6581(slice);
}




// ----------- deprecated stuff that should no longer be used -----------------

/**
* @deprecated bit0=voice0, bit1=voice1,..
*/
extern "C" uint32_t enableVoices(uint32_t mask)  __attribute__((noinline));
extern "C" uint32_t EMSCRIPTEN_KEEPALIVE enableVoices(uint32_t mask) {
	for(uint8_t i= 0; i<3; i++) {
		SID::setMute(0, i, !(mask & 0x1));
		mask = mask >> 1;
	}

	return 0;
}
/**
* @deprecated use getTraceStreams instead
*/
extern "C" char* getBufferVoice1() __attribute__((noinline));
extern "C" char* EMSCRIPTEN_KEEPALIVE getBufferVoice1() {
	return (char*) _scope_buffers[0];
}
/**
* @deprecated use getTraceStreams instead
*/
extern "C" char* getBufferVoice2() __attribute__((noinline));
extern "C" char* EMSCRIPTEN_KEEPALIVE getBufferVoice2() {
	return (char*) _scope_buffers[1];
}
/**
* @deprecated use getTraceStreams instead
*/
extern "C" char* getBufferVoice3() __attribute__((noinline));
extern "C" char* EMSCRIPTEN_KEEPALIVE getBufferVoice3() {
	return (char*) _scope_buffers[2];
}
/**
* @deprecated use getTraceStreams instead
*/
extern "C" char* getBufferVoice4() __attribute__((noinline));
extern "C" char* EMSCRIPTEN_KEEPALIVE getBufferVoice4() {
	return (char*) _scope_buffers[3];
}
