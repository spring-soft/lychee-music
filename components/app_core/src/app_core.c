#include <string.h>
#include "app_core.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"

static const char *TAG = "core";

static player_status_t  s_st;
static SemaphoreHandle_t s_lock;

void player_status_init(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
    }
    memset(&s_st, 0, sizeof(s_st));
    s_st.volume = 40;
}

void player_status_get(player_status_t *out)
{
    if (s_lock == NULL) {
        memset(out, 0, sizeof(*out));
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_st;
    xSemaphoreGive(s_lock);
}

void player_status_set_song(const char *song_id, const char *title, const char *artist,
                            const char *album, const char *cover_id, uint32_t duration_ms,
                            bool starred)
{
    if (s_lock == NULL) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(s_st.song_id,  song_id  ? song_id  : "", sizeof(s_st.song_id));
    strlcpy(s_st.title,    title    ? title    : "", sizeof(s_st.title));
    strlcpy(s_st.artist,   artist   ? artist   : "", sizeof(s_st.artist));
    strlcpy(s_st.album,    album    ? album    : "", sizeof(s_st.album));
    strlcpy(s_st.cover_id, cover_id ? cover_id : "", sizeof(s_st.cover_id));
    s_st.duration_ms = duration_ms;
    s_st.position_ms = 0;
    s_st.starred     = starred;
    s_st.playing     = false;
    s_st.paused      = false;
    s_st.finished    = false;
    s_st.gen++;                       /* 换歌：异步结果靠它对账 */
    uint32_t gen = s_st.gen;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "换歌 gen=%" PRIu32 ": \"%s\" - %s (%" PRIu32 " ms)%s",
             gen, s_st.title, s_st.artist, duration_ms, starred ? " ★" : "");
}

void player_status_set_queue(int index, int count, const char *source, bool content_changed)
{
    if (s_lock == NULL) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_st.queue_index = index;
    s_st.queue_count = count;
    if (source) strlcpy(s_st.source, source, sizeof(s_st.source));
    if (content_changed) s_st.queue_gen++;
    xSemaphoreGive(s_lock);
}

void player_status_set_progress(uint32_t position_ms, bool playing, bool paused, bool finished)
{
    if (s_lock == NULL) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_st.position_ms = position_ms;
    s_st.playing     = playing;
    s_st.paused      = paused;
    s_st.finished    = finished;
    xSemaphoreGive(s_lock);
}

void player_status_set_starred(bool starred)
{
    if (s_lock == NULL) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_st.starred = starred;
    xSemaphoreGive(s_lock);
}

void player_status_set_volume(int vol)
{
    if (s_lock == NULL) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_st.volume = vol;
    xSemaphoreGive(s_lock);
}
