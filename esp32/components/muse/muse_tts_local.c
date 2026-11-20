/*
 * On-device speech for replies: see muse_tts_local.h.
 *
 * Internal RAM is scarce on Muse boards, so everything here is in PSRAM: the
 * model's arena, the task's stack, the text and the PCM. Measured on the
 * Waveshare 1.75C, synthesis takes about 0.6 s per second of speech that way.
 */
#include "muse_tts_local.h"

#include <stdatomic.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nano_front.h"
#include "nano_partition.h"
#include "steertts_nano.h"

static const char *TAG = "muse_tts";

#define PARTITION "steertts"
#define TEXT_CAP 8192                          /* bytes of one reply that get spoken */
#define PCM_CAP (MUSE_TTS_LOCAL_RATE * 20)     /* samples waiting for the speaker: 640 KB */
#define PIECE_CHARS 160                        /* a sentence longer than this is cut at a comma or a space */
#define GAP_MS 180                             /* silence after each sentence */
#define STACK_BYTES (16 * 1024)

static nano_model *s_model;
static nano_front_ctx *s_front;
static nano_partition_map s_map_model, s_map_front;
static nano_ctx *s_ctx;
static int *s_ids, *s_widx;
static char *s_text;          /* the job's text, owned by the task while it runs */
static char *s_next;          /* the next job's text, under s_lock */
static int16_t *s_pcm;        /* ring, under s_lock */
static uint32_t s_head, s_count, s_released;   /* ring read position, samples held, samples the reader may take */
static uint32_t s_total;      /* samples synthesized for this job */
static bool s_pending, s_running, s_synthesized;
static uint32_t s_gen;        /* bumped by every speak and cancel; a job stops when it changes */
static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;
static atomic_bool s_ready;
static esp_pm_lock_handle_t s_pm;

static void *psram(size_t n)
{
    return heap_caps_aligned_calloc(16, 1, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

/* Queues samples (NULL: silence) for job `gen`. False once that job is cancelled. */
static bool put(uint32_t gen, const int16_t *pcm, uint32_t n)
{
    while (n) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (gen != s_gen) {
            xSemaphoreGive(s_lock);
            return false;
        }
        uint32_t room = PCM_CAP - s_count;
        uint32_t k = n < room ? n : room;
        for (uint32_t done = 0; done < k;) {
            uint32_t at = (s_head + s_count + done) % PCM_CAP;
            uint32_t run = k - done < PCM_CAP - at ? k - done : PCM_CAP - at;
            if (pcm) {
                memcpy(s_pcm + at, pcm + done, run * sizeof(int16_t));
            } else {
                memset(s_pcm + at, 0, run * sizeof(int16_t));
            }
            done += run;
        }
        s_count += k;
        s_total += k;
        if (k < n) {
            s_released = s_count;   /* full mid-sentence: let the speaker have what's here */
        }
        xSemaphoreGive(s_lock);
        if (pcm) {
            pcm += k;
        }
        n -= k;
        if (n) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
    return true;
}

static void release(uint32_t gen)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (gen == s_gen) {
        s_released = s_count;
    }
    xSemaphoreGive(s_lock);
}

typedef struct {
    uint32_t gen;
    int blocks;
} job_t;

static int on_pcm(void *user, const int16_t *pcm, int n)
{
    job_t *job = user;
    if (!put(job->gen, pcm, (uint32_t)n)) {
        return 1;
    }
    if (++job->blocks % 8 == 0) {
        vTaskDelay(1);   /* the idle task and lower priorities get a turn */
    }
    return 0;
}

/* One piece of text, at most a sentence. Too many phonemes: its halves, cut at a space. */
static bool say(job_t *job, char *piece, int depth)
{
    int max = s_model->cfg[NANO_CFG_MAX_PHONEMES];
    int k = nano_front_phonemes(s_front, piece, s_ids, s_widx, NF_MAX_PH);
    if (k < 0) {
        ESP_LOGW(TAG, "skipped a sentence: %s", s_front->err);
        return true;
    }
    if (k == 0) {
        return true;
    }
    if (k > max) {
        size_t len = strlen(piece);
        char *cut = NULL;
        for (size_t d = 0; d < len / 2 && !cut; d++) {
            if (piece[len / 2 + d] == ' ') {
                cut = piece + len / 2 + d;
            } else if (piece[len / 2 - d] == ' ') {
                cut = piece + len / 2 - d;
            }
        }
        if (!cut || depth >= 4) {
            k = max;   /* one endless word: say what fits */
        } else {
            *cut = '\0';
            return say(job, piece, depth + 1) && say(job, cut + 1, depth + 1);
        }
    }
    nano_request q;
    nano_request_default(&q);
    q.ids = s_ids;
    q.n_ids = k;
    q.widx = s_widx;
    if (nano_synthesize(s_ctx, &q, on_pcm, job) < 0) {
        ESP_LOGW(TAG, "synthesis failed: %s", nano_last_error());
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool live = job->gen == s_gen;
    xSemaphoreGive(s_lock);
    return live;
}

/*
 * Replies are chat text: Markdown marks, emoji and typographic punctuation
 * the front end doesn't read are dropped or mapped to ASCII, in place.
 */
static void clean(char *text)
{
    const unsigned char *in = (const unsigned char *)text;
    char *out = text;
    while (*in) {
        unsigned c = *in;
        if (c < 0x80) {
            in++;
            if (strchr("*_#`[]<>{}|~^\\", (int)c)) {
                c = ' ';
            } else if (c == '\r' || c == '\t') {
                c = ' ';
            }
            *out++ = (char)c;
            continue;
        }
        int n = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
        uint32_t cp = n == 4 ? c & 7 : n == 3 ? c & 15 : c & 31;
        int have = 1;
        while (have < n && (in[have] & 0xC0) == 0x80) {
            cp = cp << 6 | (in[have] & 0x3F);
            have++;
        }
        in += have;
        if (cp == 0x2018 || cp == 0x2019) {
            *out++ = '\'';
        } else if (cp == 0x201C || cp == 0x201D) {
            *out++ = '"';
        } else if (cp == 0x2013 || cp == 0x2014) {
            *out++ = ',';
        } else if (cp == 0x2026) {
            *out++ = '.';
        } else {
            *out++ = ' ';   /* always shorter than what it replaces */
        }
    }
    *out = '\0';
}

/* Cuts the text into sentences (and long ones at commas or spaces) and says each. */
static void run_job(job_t *job)
{
    clean(s_text);
    char *p = s_text;
    int64_t t0 = esp_timer_get_time();
    int pieces = 0;
    while (*p) {
        while (*p == ' ' || *p == '\n') {
            p++;
        }
        char *end = p, *comma = NULL, *space = NULL;
        while (*end && *end != '\n') {
            bool stop = (*end == '.' || *end == '!' || *end == '?' || *end == ';' || *end == ':') &&
                        (end[1] == ' ' || end[1] == '\n' || end[1] == '\0' || end[1] == '"' || end[1] == ')');
            if (stop) {
                end++;
                break;
            }
            if (*end == ',') {
                comma = end;
            } else if (*end == ' ') {
                space = end;
            }
            if (end - p >= PIECE_CHARS && (comma || space)) {
                end = (comma ? comma : space) + 1;
                break;
            }
            end++;
        }
        if (end == p) {
            break;
        }
        char kept = *end;
        *end = '\0';
        bool live = say(job, p, 0) && put(job->gen, NULL, MUSE_TTS_LOCAL_RATE * GAP_MS / 1000);
        *end = kept;
        if (!live) {
            ESP_LOGI(TAG, "stopped");
            return;
        }
        release(job->gen);
        pieces++;
        p = end;
        vTaskDelay(1);
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool live = job->gen == s_gen;
    uint32_t total = s_total;
    if (live) {
        s_released = s_count;
        s_synthesized = true;
    }
    xSemaphoreGive(s_lock);
    if (live) {
        double audio = (double)total / MUSE_TTS_LOCAL_RATE, wall = (esp_timer_get_time() - t0) / 1e6;
        ESP_LOGI(TAG, "spoke %d sentences: %.1f s of speech in %.1f s (x%.2f)", pieces, audio, wall,
                 audio > 0 ? wall / audio : 0.0);
    }
}

static void tts_task(void *arg)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        for (;;) {
            job_t job = {0};
            xSemaphoreTake(s_lock, portMAX_DELAY);
            bool go = s_pending;
            if (go) {
                s_pending = false;
                s_running = true;
                job.gen = s_gen;
                strlcpy(s_text, s_next, TEXT_CAP);
            } else {
                s_running = false;
            }
            xSemaphoreGive(s_lock);
            if (!go) {
                break;
            }
            /* Without this the CPU drops to its low frequency between interrupts
             * (CONFIG_PM_ENABLE) and synthesis takes several times as long. */
            if (s_pm) {
                esp_pm_lock_acquire(s_pm);
            }
            run_job(&job);
            if (s_pm) {
                esp_pm_lock_release(s_pm);
            }
        }
    }
}

bool muse_tts_local_init(void)
{
    if (s_lock) {
        return atomic_load(&s_ready);
    }
    s_lock = xSemaphoreCreateMutex();
    s_model = psram(sizeof(*s_model));
    s_front = psram(sizeof(*s_front));
    s_ids = psram(NF_MAX_PH * sizeof(int));
    s_widx = psram(NF_MAX_PH * sizeof(int));
    s_text = psram(TEXT_CAP);
    s_next = psram(TEXT_CAP);
    s_pcm = psram(PCM_CAP * sizeof(int16_t));
    if (!s_lock || !s_model || !s_front || !s_ids || !s_widx || !s_text || !s_next || !s_pcm) {
        ESP_LOGE(TAG, "no memory for on-device speech");
        return false;
    }
    int rc = nano_model_open_partition(s_model, PARTITION, &s_map_model);
    if (rc) {
        ESP_LOGW(TAG, "no speech model in the \"%s\" partition (%d: %s): replies stay text", PARTITION, rc,
                 nano_last_error());
        return false;
    }
    if (nano_front_open_partition(s_front, PARTITION, &s_map_front)) {
        ESP_LOGW(TAG, "no front end data in the \"%s\" partition: %s", PARTITION, s_front->err);
        return false;
    }
    size_t need = nano_arena_bytes(s_model, 0);
    void *arena = psram(need);
    s_ctx = arena ? nano_ctx_create(s_model, arena, need) : NULL;
    if (!s_ctx) {
        ESP_LOGE(TAG, "no memory for the model's %u byte arena", (unsigned)need);
        return false;
    }
    esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "muse_tts", &s_pm);
    /* Below the chat session (5) and the voice task: it only has to keep ahead of the speaker. */
    if (xTaskCreatePinnedToCoreWithCaps(tts_task, "muse_tts", STACK_BYTES, NULL, 3, &s_task, 1,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGE(TAG, "task start failed");
        return false;
    }
    ESP_LOGI(TAG, "on-device speech ready: model %u bytes, arena %u bytes, %s kernels",
             (unsigned)s_model->hdr->file_bytes, (unsigned)need, nano_kernel_impl());
    atomic_store(&s_ready, true);
    return true;
}

bool muse_tts_local_ready(void)
{
    return atomic_load(&s_ready);
}

static void reset_locked(void)
{
    s_gen++;
    s_head = s_count = s_released = s_total = 0;
    s_synthesized = false;
    s_pending = false;
}

bool muse_tts_local_speak(const char *text)
{
    if (!muse_tts_local_ready() || !text || !text[0]) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    reset_locked();
    strlcpy(s_next, text, TEXT_CAP);
    s_pending = true;
    xSemaphoreGive(s_lock);
    xTaskNotifyGive(s_task);
    return true;
}

void muse_tts_local_cancel(void)
{
    if (!muse_tts_local_ready()) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    reset_locked();
    xSemaphoreGive(s_lock);
}

size_t muse_tts_local_read(int16_t *pcm, size_t max)
{
    if (!muse_tts_local_ready()) {
        return 0;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint32_t n = s_released < max ? s_released : (uint32_t)max;
    for (uint32_t done = 0; done < n;) {
        uint32_t at = (s_head + done) % PCM_CAP;
        uint32_t run = n - done < PCM_CAP - at ? n - done : PCM_CAP - at;
        memcpy(pcm + done, s_pcm + at, run * sizeof(int16_t));
        done += run;
    }
    s_head = (s_head + n) % PCM_CAP;
    s_count -= n;
    s_released -= n;
    xSemaphoreGive(s_lock);
    return n;
}

bool muse_tts_local_synthesized(void)
{
    if (!muse_tts_local_ready()) {
        return true;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool done = s_synthesized;
    xSemaphoreGive(s_lock);
    return done;
}

bool muse_tts_local_done(void)
{
    if (!muse_tts_local_ready()) {
        return true;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool done = s_synthesized && s_count == 0;
    xSemaphoreGive(s_lock);
    return done;
}

uint32_t muse_tts_local_total(void)
{
    if (!muse_tts_local_ready()) {
        return 0;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint32_t total = s_total;
    xSemaphoreGive(s_lock);
    return total;
}
