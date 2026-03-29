// doomgeneric_macos.m — macOS native display backend for DOOM
// Uses AppKit/CoreGraphics for window and rendering
// Compile with: clang -ObjC -framework AppKit -framework CoreGraphics

#import <AppKit/AppKit.h>
#include <stdint.h>
#include <mach/mach_time.h>
#include "doomgeneric.h"
#include "doomkeys.h"

// Key queue
#define KEY_QUEUE_SIZE 128
static unsigned short s_KeyQueue[KEY_QUEUE_SIZE];
static int s_KeyQueueRead = 0;
static int s_KeyQueueWrite = 0;

static void addKeyToQueue(int pressed, unsigned char key) {
    s_KeyQueue[s_KeyQueueWrite] = (pressed << 8) | key;
    s_KeyQueueWrite = (s_KeyQueueWrite + 1) % KEY_QUEUE_SIZE;
}

// Map NSEvent keyCode to DOOM key
static unsigned char mapKey(unsigned short keyCode) {
    switch (keyCode) {
        case 126: return KEY_UPARROW;
        case 125: return KEY_DOWNARROW;
        case 123: return KEY_LEFTARROW;
        case 124: return KEY_RIGHTARROW;
        case 36:  return KEY_ENTER;
        case 53:  return KEY_ESCAPE;
        case 49:  return KEY_USE;       // space = use/open
        case 59:  return KEY_FIRE;      // left ctrl = fire
        case 3:   return KEY_FIRE;      // 'f' = also fire
        case 56:  return KEY_RSHIFT;    // left shift
        case 48:  return KEY_TAB;
        case 18:  return '1';
        case 19:  return '2';
        case 20:  return '3';
        case 21:  return '4';
        case 22:  return '5';
        case 23:  return '6';
        case 24:  return '7';
        case 25:  return '8';
        case 0:   return 'a';
        case 1:   return 's';
        case 2:   return 'd';
        case 13:  return 'w';
        default:  return 0;
    }
}

// Timing
static uint64_t s_startTime;
static mach_timebase_info_data_t s_timebaseInfo;

// Subclass NSImageView to suppress beeps on key presses
@interface DoomView : NSImageView
@end
@implementation DoomView
- (BOOL)acceptsFirstResponder { return YES; }
- (void)keyDown:(NSEvent *)event {}
- (void)keyUp:(NSEvent *)event {}
@end

// Window and image
static NSWindow *s_window;
static DoomView *s_imageView;

// DOOM callbacks
void DG_Init(void) {
    mach_timebase_info(&s_timebaseInfo);
    s_startTime = mach_absolute_time();
}

void DG_DrawFrame(void) {
    @autoreleasepool {
        if (!DG_ScreenBuffer) return;

        // Create CGImage from DG_ScreenBuffer (XRGB8888)
        int w = DOOMGENERIC_RESX;
        int h = DOOMGENERIC_RESY;
        CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
        CGContextRef ctx = CGBitmapContextCreate(
            DG_ScreenBuffer, w, h, 8, w * 4,
            cs, kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little
        );
        CGImageRef img = CGBitmapContextCreateImage(ctx);

        NSImage *nsImage = [[NSImage alloc] initWithCGImage:img size:NSMakeSize(w, h)];
        [s_imageView setImage:nsImage];
        [s_imageView setNeedsDisplay:YES];

        CGImageRelease(img);
        CGContextRelease(ctx);
        CGColorSpaceRelease(cs);

        // Process events
        NSEvent *event;
        while ((event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                           untilDate:nil
                                              inMode:NSDefaultRunLoopMode
                                             dequeue:YES])) {
            if (event.type == NSEventTypeKeyDown) {
                unsigned char dk = mapKey(event.keyCode);
                if (dk) addKeyToQueue(1, dk);
            } else if (event.type == NSEventTypeKeyUp) {
                unsigned char dk = mapKey(event.keyCode);
                if (dk) addKeyToQueue(0, dk);
            } else if (event.type == NSEventTypeFlagsChanged) {
                // Handle modifier keys (Ctrl, Shift, Alt)
                static NSEventModifierFlags prevFlags = 0;
                NSEventModifierFlags flags = event.modifierFlags;
                // Ctrl
                if ((flags & NSEventModifierFlagControl) && !(prevFlags & NSEventModifierFlagControl))
                    addKeyToQueue(1, KEY_FIRE);
                if (!(flags & NSEventModifierFlagControl) && (prevFlags & NSEventModifierFlagControl))
                    addKeyToQueue(0, KEY_FIRE);
                // Shift
                if ((flags & NSEventModifierFlagShift) && !(prevFlags & NSEventModifierFlagShift))
                    addKeyToQueue(1, KEY_RSHIFT);
                if (!(flags & NSEventModifierFlagShift) && (prevFlags & NSEventModifierFlagShift))
                    addKeyToQueue(0, KEY_RSHIFT);
                // Alt → strafe
                if ((flags & NSEventModifierFlagOption) && !(prevFlags & NSEventModifierFlagOption))
                    addKeyToQueue(1, KEY_RALT);
                if (!(flags & NSEventModifierFlagOption) && (prevFlags & NSEventModifierFlagOption))
                    addKeyToQueue(0, KEY_RALT);
                prevFlags = flags;
            }
            [NSApp sendEvent:event];
        }
    }
}

void DG_SleepMs(uint32_t ms) {
    usleep(ms * 1000);
}

uint32_t DG_GetTicksMs(void) {
    uint64_t elapsed = mach_absolute_time() - s_startTime;
    elapsed = elapsed * s_timebaseInfo.numer / s_timebaseInfo.denom;
    return (uint32_t)(elapsed / 1000000);
}

int DG_GetKey(int *pressed, unsigned char *key) {
    if (s_KeyQueueRead == s_KeyQueueWrite) return 0;
    unsigned short val = s_KeyQueue[s_KeyQueueRead];
    s_KeyQueueRead = (s_KeyQueueRead + 1) % KEY_QUEUE_SIZE;
    *pressed = (val >> 8) & 1;
    *key = val & 0xFF;
    return 1;
}

void DG_SetWindowTitle(const char *title) {
    @autoreleasepool {
        if (s_window)
            [s_window setTitle:[NSString stringWithUTF8String:title]];
    }
}

// --- CoreAudio SFX + Music ---
#import <AudioToolbox/AudioToolbox.h>

#undef true
#undef false
#include "doomtype.h"
#include "i_sound.h"
#include "w_wad.h"
#include "memio.h"
#include "mus2mid.h"

#define CA_NUM_CHANNELS 16
#define CA_SAMPLE_RATE 44100

int use_libsamplerate = 0;
float libsamplerate_scale = 0.65f;

typedef struct {
    uint8_t *data;
    uint32_t length;     // in 16.16 fixed point
    uint32_t position;   // in 16.16 fixed point
    uint32_t samplerate;
    int volume;          // 0-127
    int sep;             // 0-254
    int playing;
} ca_channel_t;

static ca_channel_t ca_channels[CA_NUM_CHANNELS];
static AudioComponentInstance ca_audio_unit;
static int ca_sfx_initialized = 0;

static OSStatus ca_renderCallback(void *inRefCon,
                                  AudioUnitRenderActionFlags *ioActionFlags,
                                  const AudioTimeStamp *inTimeStamp,
                                  UInt32 inBusNumber,
                                  UInt32 inNumberFrames,
                                  AudioBufferList *ioData) {
    int16_t *buf = (int16_t *)ioData->mBuffers[0].mData;
    memset(buf, 0, inNumberFrames * 4);

    for (UInt32 i = 0; i < inNumberFrames; i++) {
        int32_t mix_l = 0, mix_r = 0;
        for (int ch = 0; ch < CA_NUM_CHANNELS; ch++) {
            ca_channel_t *c = &ca_channels[ch];
            if (!c->playing || !c->data) continue;
            uint32_t src_pos = c->position >> 16;
            if (src_pos >= (c->length >> 16)) { c->playing = 0; continue; }
            int32_t sample = ((int32_t)c->data[src_pos] - 128) * 256;
            sample = sample * c->volume / 127;
            mix_l += (sample * c->sep) / 254;
            mix_r += (sample * (254 - c->sep)) / 254;
            c->position += (uint32_t)((uint64_t)c->samplerate * 65536 / CA_SAMPLE_RATE);
        }
        if (mix_l > 32767) mix_l = 32767; if (mix_l < -32768) mix_l = -32768;
        if (mix_r > 32767) mix_r = 32767; if (mix_r < -32768) mix_r = -32768;
        buf[i * 2]     = (int16_t)mix_l;
        buf[i * 2 + 1] = (int16_t)mix_r;
    }
    return noErr;
}

static boolean ca_InitSound(boolean use_sfx_prefix) {
    memset(ca_channels, 0, sizeof(ca_channels));
    AudioComponentDescription desc = {
        .componentType = kAudioUnitType_Output,
        .componentSubType = kAudioUnitSubType_DefaultOutput,
        .componentManufacturer = kAudioUnitManufacturer_Apple,
    };
    AudioComponent comp = AudioComponentFindNext(NULL, &desc);
    if (!comp) return 0;
    if (AudioComponentInstanceNew(comp, &ca_audio_unit) != noErr) return 0;
    AudioStreamBasicDescription fmt = {
        .mSampleRate = CA_SAMPLE_RATE,
        .mFormatID = kAudioFormatLinearPCM,
        .mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked,
        .mBytesPerPacket = 4, .mFramesPerPacket = 1,
        .mBytesPerFrame = 4, .mChannelsPerFrame = 2, .mBitsPerChannel = 16,
    };
    AudioUnitSetProperty(ca_audio_unit, kAudioUnitProperty_StreamFormat,
                         kAudioUnitScope_Input, 0, &fmt, sizeof(fmt));
    AURenderCallbackStruct cb = { ca_renderCallback, NULL };
    AudioUnitSetProperty(ca_audio_unit, kAudioUnitProperty_SetRenderCallback,
                         kAudioUnitScope_Input, 0, &cb, sizeof(cb));
    if (AudioUnitInitialize(ca_audio_unit) != noErr) {
        AudioComponentInstanceDispose(ca_audio_unit); return 0;
    }
    AudioOutputUnitStart(ca_audio_unit);
    ca_sfx_initialized = 1;
    return 1;
}

static void ca_ShutdownSound(void) {
    if (ca_sfx_initialized) {
        AudioOutputUnitStop(ca_audio_unit);
        AudioUnitUninitialize(ca_audio_unit);
        AudioComponentInstanceDispose(ca_audio_unit);
        ca_sfx_initialized = 0;
    }
}

static int ca_GetSfxLumpNum(sfxinfo_t *sfxinfo) {
    char namebuf[16];
    snprintf(namebuf, sizeof(namebuf), "DS%s", sfxinfo->name);
    for (char *p = namebuf; *p; p++)
        if (*p >= 'a' && *p <= 'z') *p -= 32;
    return W_GetNumForName(namebuf);
}

static void ca_Update(void) {}
static void ca_UpdateSoundParams(int channel, int vol, int sep) {
    if (channel >= 0 && channel < CA_NUM_CHANNELS) {
        ca_channels[channel].volume = vol;
        ca_channels[channel].sep = sep;
    }
}

static int ca_StartSound(sfxinfo_t *sfxinfo, int channel, int vol, int sep) {
    if (channel < 0 || channel >= CA_NUM_CHANNELS) return -1;
    int lumpnum = sfxinfo->lumpnum;
    uint8_t *lumpdata = (uint8_t *)W_CacheLumpNum(lumpnum, 8);
    int lumplen = W_LumpLength(lumpnum);
    if (lumplen < 8) return -1;
    uint16_t fmt = lumpdata[0] | (lumpdata[1] << 8);
    uint16_t rate = lumpdata[2] | (lumpdata[3] << 8);
    uint32_t nsamples = lumpdata[4] | (lumpdata[5] << 8)
                      | (lumpdata[6] << 16) | (lumpdata[7] << 24);
    if (fmt != 3) return -1;
    if (nsamples + 8 > (uint32_t)lumplen) nsamples = lumplen - 8;
    ca_channel_t *c = &ca_channels[channel];
    c->data = lumpdata + 8;
    c->length = nsamples << 16;
    c->position = 0;
    c->samplerate = rate;
    c->volume = vol;
    c->sep = sep;
    c->playing = 1;
    return channel;
}

static void ca_StopSound(int ch) {
    if (ch >= 0 && ch < CA_NUM_CHANNELS) ca_channels[ch].playing = 0;
}
static boolean ca_SoundIsPlaying(int ch) {
    return (ch >= 0 && ch < CA_NUM_CHANNELS) ? ca_channels[ch].playing : 0;
}
static void ca_CacheSounds(sfxinfo_t *s, int n) {}

static snddevice_t ca_snd_devices[] = { SNDDEVICE_SB };

sound_module_t DG_sound_module = {
    ca_snd_devices, 1,
    ca_InitSound, ca_ShutdownSound, ca_GetSfxLumpNum,
    ca_Update, ca_UpdateSoundParams, ca_StartSound,
    ca_StopSound, ca_SoundIsPlaying, ca_CacheSounds
};

// --- Music: MUS → MIDI via AudioToolbox ---
static MusicPlayer ca_music_player = NULL;
static MusicSequence ca_music_seq = NULL;
static AUGraph ca_music_graph = NULL;
static int ca_music_playing = 0;

static void ca_setup_music_graph(void) {
    if (ca_music_graph) return;
    NewAUGraph(&ca_music_graph);
    AudioComponentDescription synthDesc = {
        .componentType = kAudioUnitType_MusicDevice,
        .componentSubType = kAudioUnitSubType_DLSSynth,
        .componentManufacturer = kAudioUnitManufacturer_Apple,
    };
    AUNode synthNode;
    AUGraphAddNode(ca_music_graph, &synthDesc, &synthNode);
    AudioComponentDescription outDesc = {
        .componentType = kAudioUnitType_Output,
        .componentSubType = kAudioUnitSubType_DefaultOutput,
        .componentManufacturer = kAudioUnitManufacturer_Apple,
    };
    AUNode outNode;
    AUGraphAddNode(ca_music_graph, &outDesc, &outNode);
    AUGraphConnectNodeInput(ca_music_graph, synthNode, 0, outNode, 0);
    AUGraphOpen(ca_music_graph);
    AUGraphInitialize(ca_music_graph);
}

static boolean ca_InitMusic(void) {
    ca_setup_music_graph();
    NewMusicPlayer(&ca_music_player);
    return 1;
}
static void ca_ShutdownMusic(void) {
    if (ca_music_player) { MusicPlayerStop(ca_music_player); DisposeMusicPlayer(ca_music_player); ca_music_player = NULL; }
    if (ca_music_seq) { DisposeMusicSequence(ca_music_seq); ca_music_seq = NULL; }
    if (ca_music_graph) { AUGraphStop(ca_music_graph); AUGraphUninitialize(ca_music_graph); DisposeAUGraph(ca_music_graph); ca_music_graph = NULL; }
}
static void ca_SetMusicVolume(int volume) { /* volume handled by MIDI data */ }
static void ca_PauseMusic(void) { if (ca_music_player && ca_music_playing) MusicPlayerStop(ca_music_player); }
static void ca_ResumeMusic(void) { if (ca_music_player && ca_music_playing) MusicPlayerStart(ca_music_player); }

typedef struct { void *midi_data; size_t midi_len; } ca_midi_song_t;

static void *ca_RegisterSong(void *data, int len) {
    MEMFILE *mus_in = mem_fopen_read(data, len);
    MEMFILE *mid_out = mem_fopen_write();
    if (!mus2mid(mus_in, mid_out)) { mem_fclose(mus_in); mem_fclose(mid_out); return NULL; }
    ca_midi_song_t *song = malloc(sizeof(ca_midi_song_t));
    void *buf; size_t buflen;
    mem_get_buf(mid_out, &buf, &buflen);
    song->midi_data = malloc(buflen);
    memcpy(song->midi_data, buf, buflen);
    song->midi_len = buflen;
    mem_fclose(mus_in); mem_fclose(mid_out);
    return song;
}
static void ca_UnRegisterSong(void *handle) {
    if (!handle) return;
    ca_midi_song_t *s = handle; free(s->midi_data); free(s);
}
static void ca_PlaySong(void *handle, boolean looping) {
    if (!handle || !ca_music_player) return;
    ca_midi_song_t *song = handle;
    if (ca_music_seq) { MusicPlayerStop(ca_music_player); MusicPlayerSetSequence(ca_music_player, NULL); DisposeMusicSequence(ca_music_seq); ca_music_seq = NULL; }
    NewMusicSequence(&ca_music_seq);
    CFDataRef midi_cf = CFDataCreate(NULL, song->midi_data, song->midi_len);
    MusicSequenceFileLoadData(ca_music_seq, midi_cf, kMusicSequenceFile_MIDIType, 0);
    CFRelease(midi_cf);
    MusicSequenceSetAUGraph(ca_music_seq, ca_music_graph);
    if (looping) {
        UInt32 tc; MusicSequenceGetTrackCount(ca_music_seq, &tc);
        for (UInt32 i = 0; i < tc; i++) {
            MusicTrack trk; MusicSequenceGetIndTrack(ca_music_seq, i, &trk);
            MusicTimeStamp tlen; UInt32 ps = sizeof(tlen);
            MusicTrackGetProperty(trk, kSequenceTrackProperty_TrackLength, &tlen, &ps);
            MusicTrackLoopInfo li = { .loopDuration = tlen, .numberOfLoops = 0 };
            MusicTrackSetProperty(trk, kSequenceTrackProperty_LoopInfo, &li, sizeof(li));
        }
    }
    MusicPlayerSetSequence(ca_music_player, ca_music_seq);
    MusicPlayerPreroll(ca_music_player);
    MusicPlayerStart(ca_music_player);
    ca_music_playing = 1;
}
static void ca_StopSong(void) { if (ca_music_player) MusicPlayerStop(ca_music_player); ca_music_playing = 0; }
static boolean ca_MusicIsPlaying(void) { return ca_music_playing; }
static void ca_Poll(void) {}

music_module_t DG_music_module = {
    ca_snd_devices, 1,
    ca_InitMusic, ca_ShutdownMusic, ca_SetMusicVolume,
    ca_PauseMusic, ca_ResumeMusic,
    ca_RegisterSong, ca_UnRegisterSong,
    ca_PlaySong, ca_StopSong, ca_MusicIsPlaying, ca_Poll
};

// Main: create window then run DOOM
int main(int argc, char **argv) {
    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

        // Create window
        NSRect frame = NSMakeRect(100, 100, DOOMGENERIC_RESX, DOOMGENERIC_RESY);
        s_window = [[NSWindow alloc]
            initWithContentRect:frame
                      styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                        backing:NSBackingStoreBuffered
                          defer:NO];
        [s_window setTitle:@"DOOM"];

        s_imageView = [[DoomView alloc] initWithFrame:frame];
        [s_imageView setImageScaling:NSImageScaleAxesIndependently];
        [s_window setContentView:s_imageView];
        [s_window setAcceptsMouseMovedEvents:YES];
        [s_window makeKeyAndOrderFront:nil];
        [s_window makeFirstResponder:s_imageView];
        [NSApp activateIgnoringOtherApps:YES];

        // Start DOOM
        doomgeneric_Create(argc, argv);

        // Game loop
        while (1) {
            doomgeneric_Tick();
        }
    }
    return 0;
}
