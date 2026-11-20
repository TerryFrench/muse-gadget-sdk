/*
 * Speaks reply text on the device with SteerTTS Nano (CONFIG_MUSE_LOCAL_TTS).
 *
 * The model (nano.bin) and its text front end (front.bin) are read from the
 * "steertts" flash partition. A task of its own turns text into 16 kHz PCM,
 * one sentence at a time, into a buffer in PSRAM; the chat session's task
 * takes finished sentences from there and queues them for the speaker.
 *
 * The SteerTTS runtime and weights are not part of this repository (CC BY-NC
 * 4.0, Whissle): the build takes the runtime from $MUSE_STEERTTS_DIR.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MUSE_TTS_LOCAL_RATE 16000

/* Maps the model and starts the task. False (logged) when the partition, the
 * model or the memory isn't there: replies then stay text. */
bool muse_tts_local_init(void);
bool muse_tts_local_ready(void);

/* Starts speaking `text` (copied), dropping whatever was being spoken. */
bool muse_tts_local_speak(const char *text);
void muse_tts_local_cancel(void);

/* Up to `max` samples of finished sentences; 0 when none are ready yet. */
size_t muse_tts_local_read(int16_t *pcm, size_t max);

/* The whole text is synthesized (its length is then muse_tts_local_total()). */
bool muse_tts_local_synthesized(void);
/* ... and all of it was read. */
bool muse_tts_local_done(void);
uint32_t muse_tts_local_total(void);

#ifdef __cplusplus
}
#endif
