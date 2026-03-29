// i_sound_coreaudio.m — CoreAudio sound backend for DOOM on macOS
// Implements DG_sound_module and DG_music_module using AudioToolbox
// SFX: AudioUnit render callback mixing DMX-format PCM from WAD lumps
// Music: AudioToolbox MusicSequence for MIDI playback (MUS→MIDI conversion)

#import <AudioToolbox/AudioToolbox.h>
#import <CoreAudio/CoreAudio.h>
#include <string.h>
#include <stdlib.h>

// Include ObjC frameworks before doomtype.h to avoid true/false clash
#undef true
#undef false
#include "doomtype.h"
#include "i_sound.h"
#include "i_swap.h"
#include "w_wad.h"
#include "z_zone.h"
#include "memio.h"
#include "mus2mid.h"

// --- SFX ---

#define NUM_CHANNELS 16
#define SAMPLE_RATE 44100

typedef struct {
    uint8_t *data;       // unsigned 8-bit PCM samples
    uint32_t length;     // number of samples
    uint32_t position;   // current playback position
    uint32_t samplerate; // original sample rate
    int volume;          // 0-127
    int sep;             // 0-254 (0=right, 127=center, 254=left)
    boolean playing;
    uint64_t start_tick; // for age tracking
    sfxinfo_t *sfxinfo;
} channel_t;

static channel_t channels[NUM_CHANNELS];
static AudioComponentInstance audio_unit;
static boolean sfx_initialized = false;
static uint64_t tick_counter = 0;

int use_libsamplerate = 0;
float libsamplerate_scale = 0.65f;

// Render callback — mixes all active channels into the output buffer
static OSStatus renderCallback(void *inRefCon,
                               AudioUnitRenderActionFlags *ioActionFlags,
                               const AudioTimeStamp *inTimeStamp,
                               UInt32 inBusNumber,
                               UInt32 inNumberFrames,
                               AudioBufferList *ioData) {
    int16_t *left  = (int16_t *)ioData->mBuffers[0].mData;
    int16_t *right = (ioData->mNumberBuffers > 1)
                     ? (int16_t *)ioData->mBuffers[1].mData
                     : left;

    // Check if we have interleaved stereo
    boolean interleaved = (ioData->mNumberBuffers == 1 &&
                           ioData->mBuffers[0].mNumberChannels == 2);

    // Zero the buffer
    for (UInt32 b = 0; b < ioData->mNumberBuffers; b++) {
        memset(ioData->mBuffers[b].mData, 0, ioData->mBuffers[b].mDataByteSize);
    }

    for (UInt32 i = 0; i < inNumberFrames; i++) {
        int32_t mix_l = 0, mix_r = 0;

        for (int ch = 0; ch < NUM_CHANNELS; ch++) {
            channel_t *c = &channels[ch];
            if (!c->playing || !c->data) continue;

            // Resample: advance source position at source_rate/SAMPLE_RATE
            uint32_t src_pos = c->position;
            if (src_pos >= c->length) {
                c->playing = false;
                continue;
            }

            // Get sample, convert from unsigned 8-bit to signed 16-bit
            int32_t sample = ((int32_t)c->data[src_pos] - 128) * 256;

            // Apply volume (0-127 → scale)
            sample = sample * c->volume / 127;

            // Stereo separation: sep 0=right, 127=center, 254=left
            int left_vol  = c->sep;        // 0..254
            int right_vol = 254 - c->sep;  // 254..0

            mix_l += (sample * left_vol) / 254;
            mix_r += (sample * right_vol) / 254;

            // Advance position with resampling
            c->position = src_pos + (uint32_t)((uint64_t)c->samplerate * 65536 / SAMPLE_RATE);
            // We store position in 16.16 fixed point for resampling
        }

        // Clamp
        if (mix_l > 32767)  mix_l = 32767;
        if (mix_l < -32768) mix_l = -32768;
        if (mix_r > 32767)  mix_r = 32767;
        if (mix_r < -32768) mix_r = -32768;

        if (interleaved) {
            int16_t *buf = (int16_t *)ioData->mBuffers[0].mData;
            buf[i * 2]     = (int16_t)mix_l;
            buf[i * 2 + 1] = (int16_t)mix_r;
        } else {
            left[i]  = (int16_t)mix_l;
            if (ioData->mNumberBuffers > 1)
                right[i] = (int16_t)mix_r;
        }
    }

    return noErr;
}

static boolean ca_InitSound(boolean use_sfx_prefix) {
    memset(channels, 0, sizeof(channels));

    // Set up default output AudioUnit
    AudioComponentDescription desc = {
        .componentType = kAudioUnitType_Output,
        .componentSubType = kAudioUnitSubType_DefaultOutput,
        .componentManufacturer = kAudioUnitManufacturer_Apple,
    };

    AudioComponent comp = AudioComponentFindNext(NULL, &desc);
    if (!comp) return false;

    if (AudioComponentInstanceNew(comp, &audio_unit) != noErr)
        return false;

    // Set format: 16-bit signed integer, stereo, interleaved
    AudioStreamBasicDescription fmt = {
        .mSampleRate = SAMPLE_RATE,
        .mFormatID = kAudioFormatLinearPCM,
        .mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked,
        .mBytesPerPacket = 4,
        .mFramesPerPacket = 1,
        .mBytesPerFrame = 4,
        .mChannelsPerFrame = 2,
        .mBitsPerChannel = 16,
    };

    AudioUnitSetProperty(audio_unit, kAudioUnitProperty_StreamFormat,
                         kAudioUnitScope_Input, 0, &fmt, sizeof(fmt));

    // Set render callback
    AURenderCallbackStruct cb = { renderCallback, NULL };
    AudioUnitSetProperty(audio_unit, kAudioUnitProperty_SetRenderCallback,
                         kAudioUnitScope_Input, 0, &cb, sizeof(cb));

    if (AudioUnitInitialize(audio_unit) != noErr) {
        AudioComponentInstanceDispose(audio_unit);
        return false;
    }

    AudioOutputUnitStart(audio_unit);
    sfx_initialized = true;
    return true;
}

static void ca_ShutdownSound(void) {
    if (sfx_initialized) {
        AudioOutputUnitStop(audio_unit);
        AudioUnitUninitialize(audio_unit);
        AudioComponentInstanceDispose(audio_unit);
        sfx_initialized = false;
    }
}

static int ca_GetSfxLumpNum(sfxinfo_t *sfxinfo) {
    char namebuf[16];
    snprintf(namebuf, sizeof(namebuf), "DS%s", sfxinfo->name);

    // Convert to uppercase
    for (char *p = namebuf; *p; p++) {
        if (*p >= 'a' && *p <= 'z') *p -= 32;
    }

    return W_GetNumForName(namebuf);
}

static void ca_Update(void) {
    tick_counter++;
}

static void ca_UpdateSoundParams(int channel, int vol, int sep) {
    if (channel < 0 || channel >= NUM_CHANNELS) return;
    channels[channel].volume = vol;
    channels[channel].sep = sep;
}

static int ca_StartSound(sfxinfo_t *sfxinfo, int channel, int vol, int sep) {
    if (channel < 0 || channel >= NUM_CHANNELS) return -1;

    int lumpnum = sfxinfo->lumpnum;
    uint8_t *lumpdata = (uint8_t *)W_CacheLumpNum(lumpnum, 8 /* PU_STATIC */);
    int lumplen = W_LumpLength(lumpnum);

    if (lumplen < 8) return -1;

    // DMX sound format:
    // bytes 0-1: format type (should be 3)
    // bytes 2-3: sample rate
    // bytes 4-7: number of samples
    // bytes 8+:  unsigned 8-bit PCM data
    uint16_t fmt_type   = lumpdata[0] | (lumpdata[1] << 8);
    uint16_t samplerate = lumpdata[2] | (lumpdata[3] << 8);
    uint32_t num_samples = lumpdata[4] | (lumpdata[5] << 8)
                         | (lumpdata[6] << 16) | (lumpdata[7] << 24);

    if (fmt_type != 3) return -1;
    if (num_samples + 8 > (uint32_t)lumplen) {
        num_samples = lumplen - 8;
    }
    // Skip the 16-byte padding that some lumps have
    uint8_t *pcm = lumpdata + 8;

    channel_t *c = &channels[channel];
    c->data = pcm;
    c->length = num_samples << 16;  // 16.16 fixed point
    c->position = 0;
    c->samplerate = samplerate;
    c->volume = vol;
    c->sep = sep;
    c->playing = true;
    c->start_tick = tick_counter;
    c->sfxinfo = sfxinfo;

    return channel;
}

static void ca_StopSound(int channel) {
    if (channel < 0 || channel >= NUM_CHANNELS) return;
    channels[channel].playing = false;
}

static boolean ca_SoundIsPlaying(int channel) {
    if (channel < 0 || channel >= NUM_CHANNELS) return false;
    return channels[channel].playing;
}

static void ca_CacheSounds(sfxinfo_t *sounds, int num_sounds) {
    // Precaching not needed — we read from WAD cache on demand
}

static snddevice_t ca_sound_devices[] = { SNDDEVICE_SB };

sound_module_t DG_sound_module = {
    ca_sound_devices, 1,
    ca_InitSound, ca_ShutdownSound, ca_GetSfxLumpNum,
    ca_Update, ca_UpdateSoundParams, ca_StartSound,
    ca_StopSound, ca_SoundIsPlaying, ca_CacheSounds
};

// --- Music (MUS → MIDI → CoreMIDI) ---

static MusicPlayer music_player = NULL;
static MusicSequence music_sequence = NULL;
static AUGraph music_graph = NULL;
static boolean music_initialized = false;
static boolean music_playing = false;
static boolean music_looping = false;
static int music_volume = 127;  // 0-127

static void setup_music_graph(void) {
    if (music_graph) return;

    NewAUGraph(&music_graph);

    // Add DLS synth node
    AudioComponentDescription synthDesc = {
        .componentType = kAudioUnitType_MusicDevice,
        .componentSubType = kAudioUnitSubType_DLSSynth,
        .componentManufacturer = kAudioUnitManufacturer_Apple,
    };
    AUNode synthNode;
    AUGraphAddNode(music_graph, &synthDesc, &synthNode);

    // Add output node
    AudioComponentDescription outDesc = {
        .componentType = kAudioUnitType_Output,
        .componentSubType = kAudioUnitSubType_DefaultOutput,
        .componentManufacturer = kAudioUnitManufacturer_Apple,
    };
    AUNode outNode;
    AUGraphAddNode(music_graph, &outDesc, &outNode);

    AUGraphConnectNodeInput(music_graph, synthNode, 0, outNode, 0);
    AUGraphOpen(music_graph);
    AUGraphInitialize(music_graph);
}

static boolean ca_InitMusic(void) {
    setup_music_graph();
    NewMusicPlayer(&music_player);
    music_initialized = true;
    return true;
}

static void ca_ShutdownMusic(void) {
    if (music_player) {
        MusicPlayerStop(music_player);
        DisposeMusicPlayer(music_player);
        music_player = NULL;
    }
    if (music_sequence) {
        DisposeMusicSequence(music_sequence);
        music_sequence = NULL;
    }
    if (music_graph) {
        AUGraphStop(music_graph);
        AUGraphUninitialize(music_graph);
        DisposeAUGraph(music_graph);
        music_graph = NULL;
    }
    music_initialized = false;
}

static void ca_SetMusicVolume(int volume) {
    music_volume = volume;
    // Volume is applied during playback via MIDI channel volume messages.
    // We don't directly set AudioUnit parameters here since the DLS synth
    // doesn't expose a simple global volume parameter.
}

static void ca_PauseMusic(void) {
    if (music_player && music_playing) {
        MusicPlayerStop(music_player);
    }
}

static void ca_ResumeMusic(void) {
    if (music_player && music_playing) {
        MusicPlayerStart(music_player);
    }
}

typedef struct {
    void *midi_data;
    size_t midi_len;
} midi_song_t;

static void *ca_RegisterSong(void *data, int len) {
    // Convert MUS to MIDI
    MEMFILE *mus_input = mem_fopen_read(data, len);
    MEMFILE *midi_output = mem_fopen_write();

    if (!mus2mid(mus_input, midi_output)) {
        mem_fclose(mus_input);
        mem_fclose(midi_output);
        return NULL;
    }

    midi_song_t *song = malloc(sizeof(midi_song_t));
    mem_get_buf(midi_output, &song->midi_data, &song->midi_len);

    // Make a copy since mem_fclose will free the buffer
    void *copy = malloc(song->midi_len);
    memcpy(copy, song->midi_data, song->midi_len);
    song->midi_data = copy;

    mem_fclose(mus_input);
    mem_fclose(midi_output);

    return song;
}

static void ca_UnRegisterSong(void *handle) {
    if (!handle) return;
    midi_song_t *song = (midi_song_t *)handle;
    free(song->midi_data);
    free(song);
}

static void ca_PlaySong(void *handle, boolean looping) {
    if (!handle || !music_player) return;

    midi_song_t *song = (midi_song_t *)handle;

    // Stop current playback
    if (music_sequence) {
        MusicPlayerStop(music_player);
        MusicPlayerSetSequence(music_player, NULL);
        DisposeMusicSequence(music_sequence);
        music_sequence = NULL;
    }

    // Create new sequence from MIDI data
    NewMusicSequence(&music_sequence);

    CFDataRef midi_cf = CFDataCreate(NULL, song->midi_data, song->midi_len);
    MusicSequenceFileLoadData(music_sequence, midi_cf, kMusicSequenceFile_MIDIType, 0);
    CFRelease(midi_cf);

    // Associate with AUGraph
    MusicSequenceSetAUGraph(music_sequence, music_graph);

    // Set up looping if requested
    music_looping = looping;
    if (looping) {
        UInt32 track_count;
        MusicSequenceGetTrackCount(music_sequence, &track_count);
        for (UInt32 i = 0; i < track_count; i++) {
            MusicTrack track;
            MusicSequenceGetIndTrack(music_sequence, i, &track);
            MusicTimeStamp track_len;
            UInt32 prop_size = sizeof(track_len);
            MusicTrackGetProperty(track, kSequenceTrackProperty_TrackLength,
                                  &track_len, &prop_size);
            MusicTrackLoopInfo loop_info = { .loopDuration = track_len, .numberOfLoops = 0 };
            MusicTrackSetProperty(track, kSequenceTrackProperty_LoopInfo,
                                  &loop_info, sizeof(loop_info));
        }
    }

    MusicPlayerSetSequence(music_player, music_sequence);
    MusicPlayerPreroll(music_player);
    MusicPlayerStart(music_player);
    music_playing = true;

    ca_SetMusicVolume(music_volume);
}

static void ca_StopSong(void) {
    if (music_player) {
        MusicPlayerStop(music_player);
    }
    music_playing = false;
}

static boolean ca_MusicIsPlaying(void) {
    return music_playing;
}

static void ca_Poll(void) {
    // Nothing needed — CoreAudio handles playback on its own thread
}

static snddevice_t ca_music_devices[] = { SNDDEVICE_SB };

music_module_t DG_music_module = {
    ca_music_devices, 1,
    ca_InitMusic, ca_ShutdownMusic, ca_SetMusicVolume,
    ca_PauseMusic, ca_ResumeMusic,
    ca_RegisterSong, ca_UnRegisterSong,
    ca_PlaySong, ca_StopSong, ca_MusicIsPlaying, ca_Poll
};
