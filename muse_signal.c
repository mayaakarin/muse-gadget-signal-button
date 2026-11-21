/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * Signal buttons: a gadget-to-agent emit on the Link session.
 *
 * The button set is NOT compiled in. Once the session is up, the
 * firmware asks the agent for it through the chat itself (the
 * /fs/read route is not open to every credential; the conversation
 * routes are). It posts a request signal:
 *
 *     [SIG] code=CFGREQ seq=N nonce=XXXXXXXX
 *
 * and the agent answers with an ordinary chat message carrying the
 * button list between markers, echoing the nonce:
 *
 *     [CFG] nonce=XXXXXXXX
 *     01:21,02:47,03:48,04:38
 *     [/CFG]
 *
 * The list is one line of "<index>:<GPIO>" pairs separated by
 * commas. The index is the button number: the device builds the
 * code "BTN<index>" itself, so no code strings travel. Pairs must
 * run in order (01, 02, 03, ...); at most 32 pairs, so the line
 * stays under 200 bytes. The firmware polls GET /chat/history
 * until a [CFG] block with the matching nonce shows up. The nonce
 * keeps a stale [CFG] from an earlier boot from being picked up.
 *
 * Buttons are active low with the internal pull-up. A press posts one
 * short message to the chat:
 *
 *     [SIG] code=BTN2 press=short seq=N
 *
 * as a POST /chat/stream on Link's own Noise session, the same route
 * the no-PSRAM chat uses (noise_ctrl_req_*). The body follows the
 * shape a product sender uses (DFRobot K10 fork, muse_event_sender):
 * {"message": ..., "output_modality": "text", "device_id": node_id},
 * header x-app-id: musegadget. The send is acknowledged when the reply
 * carries HTTP 200 and a result message_id; the reply text itself is
 * not used (any answer comes back through the chat / draw_file).
 *
 * The device sends a code only. What the signal means -- the action
 * it asks for -- is agreed on the agent side, never in the device.
 * The agent's copy of the button list is the config; editing it and
 * rebooting is enough to add, remove or reassign buttons; no
 * rebuild.
 *
 * If the config cannot be loaded (no session, no [CFG] answer in
 * time, no valid line), the firmware falls back to a single button,
 * BTN1 on CONFIG_HOMEHUB_SIGNAL_GPIO, so the device is never left
 * buttonless.
 *
 * One signal in flight at a time: presses while one is pending or
 * sending are coalesced (counted, not queued). A failed or timed-out
 * send is retried with the SAME seq after a pause, so the agent can
 * dedup retries by seq; seq advances only after an ack, and is kept
 * in RAM (a reboot restarts it at 1).
 */

#include "muse_signal.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "identity.h"
#include "noise_control.h"

static const char *TAG = "signal";

#if CONFIG_HOMEHUB_SIGNAL_BUTTON

#define SIG_POLL_MS      10
#define SIG_DEBOUNCE_MS  30
#define SIG_ACK_CAP      512   /* only the reply head is inspected */
#define SIG_TIMEOUT_MS   45000 /* ack wait, as in the K10 sender */
#define SIG_RETRY_MS     60000 /* pause before retrying a failed send */
#define SIG_DOWN_MS      5000  /* pause when the session is down */

#define SIG_MAX_BUTTONS  32
#define SIG_CODE_CAP     16
#define SIG_CFG_CAP      1536  /* decoded [CFG] text scratch */
#define SIG_HIST_CAP     8192  /* /chat/history reply scratch (PSRAM) */
#define SIG_CFG_WAIT_MS  60000 /* wait for the session before asking */
#define SIG_CFG_POLL_MS  5000  /* history poll interval while waiting */
#define SIG_CFG_TOTAL_MS 120000 /* total wait for a [CFG] answer */

typedef struct {
    int     gpio;
    char    code[SIG_CODE_CAP];
    bool    stable_pressed;
    bool    candidate;
    int64_t candidate_since;
} sig_button_t;

static sig_button_t s_buttons[SIG_MAX_BUTTONS];
static size_t s_button_count;
static bool s_cfg_via_chat; /* the table came from a [CFG] answer */

static struct {
    SemaphoreHandle_t lock;
    /* The pending signal (waiting or in flight). */
    bool     pending;
    char     code[SIG_CODE_CAP]; /* code of the pending signal */
    uint32_t seq;        /* seq of the pending signal */
    uint32_t next_seq;   /* seq the next new signal will take */
    int      coalesced;  /* presses merged into the pending one */
    /* The last acknowledged signal, for device.signal. */
    uint32_t last_seq;
    char     last_code[SIG_CODE_CAP];
    bool     last_acked;
    int64_t  last_ack_ms;
    /* In-flight reply state, written by the session-task callback. */
    uint32_t gen;
    int      status;
    char     ack[SIG_ACK_CAP];
    size_t   ack_len;
    bool     rx_done;
} s_sig;

/* Runs on the session task: copy what arrived, never block. */
static void sig_on_frame(void *ctx, int status, const uint8_t *data,
                         size_t len, bool end) {
    uint32_t gen = (uint32_t)(uintptr_t)ctx;
    xSemaphoreTake(s_sig.lock, portMAX_DELAY);
    if (gen == s_sig.gen && !s_sig.rx_done) {
        if (status) s_sig.status = status;
        if (data && len) {
            size_t room = SIG_ACK_CAP - 1 - s_sig.ack_len;
            if (len > room) len = room;
            memcpy(s_sig.ack + s_sig.ack_len, data, len);
            s_sig.ack_len += len;
            s_sig.ack[s_sig.ack_len] = '\0';
        }
        s_sig.rx_done = end || status < 0;
    }
    xSemaphoreGive(s_sig.lock);
}

static bool sig_ack_ok(void) {
    xSemaphoreTake(s_sig.lock, portMAX_DELAY);
    bool ok = s_sig.status == 200 && strstr(s_sig.ack, "\"message_id\"") != NULL;
    xSemaphoreGive(s_sig.lock);
    return ok;
}

/* True when the wait for a reply can stop: acked, or the stream ended. */
static bool sig_wait_over(void) {
    xSemaphoreTake(s_sig.lock, portMAX_DELAY);
    bool done = s_sig.rx_done;
    xSemaphoreGive(s_sig.lock);
    return done || sig_ack_ok();
}

typedef enum {
    SIG_SEND_ACKED,
    SIG_SEND_DOWN,   /* the session is down: nothing was sent */
    SIG_SEND_FAILED, /* sent (or tried), but no ack */
} sig_send_result_t;

/* One send attempt for a fully formatted chat message. Shared by the
 * button sender and the boot-time config request; only one message
 * is ever in flight (buttons start after the config is settled). */
static sig_send_result_t sig_post_message(const char *message) {
    xSemaphoreTake(s_sig.lock, portMAX_DELAY);
    uint32_t gen = ++s_sig.gen;
    s_sig.status = 0;
    s_sig.ack_len = 0;
    s_sig.ack[0] = '\0';
    s_sig.rx_done = false;
    xSemaphoreGive(s_sig.lock);

    char body[320];
    snprintf(body, sizeof(body),
             "{\"message\":\"%s\",\"output_modality\":\"text\","
             "\"device_id\":\"%s\"}",
             message, identity_node_id());
    char req_id[48];
    snprintf(req_id, sizeof(req_id), "sig-%08" PRIx32 "%08" PRIx32,
             esp_random(), esp_random());
    const char *headers[] = {"x-request-id", req_id,
                             "x-app-id",     "musegadget",
                             "Content-Type", "application/json",
                             NULL};

    int64_t id = noise_ctrl_req_open("POST", "/chat/stream", headers, false,
                                     sig_on_frame, (void *)(uintptr_t)gen);
    if (!id) return SIG_SEND_DOWN;
    if (!noise_ctrl_req_send(id, body, strlen(body), true, 5000)) {
        noise_ctrl_req_cancel(id);
        return SIG_SEND_FAILED;
    }
    ESP_LOGI(TAG, "sent %s", message);

    int64_t deadline = esp_timer_get_time() + (int64_t)SIG_TIMEOUT_MS * 1000;
    while (esp_timer_get_time() < deadline) {
        if (sig_wait_over()) break;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    bool ok = sig_ack_ok();
    if (!ok) noise_ctrl_req_cancel(id);
    return ok ? SIG_SEND_ACKED : SIG_SEND_FAILED;
}

/* One send attempt for the pending signal. */
static sig_send_result_t sig_send_once(void) {
    uint32_t seq;
    char code[sizeof(s_sig.code)];
    xSemaphoreTake(s_sig.lock, portMAX_DELAY);
    seq = s_sig.seq;
    strncpy(code, s_sig.code, sizeof(code) - 1);
    code[sizeof(code) - 1] = '\0';
    xSemaphoreGive(s_sig.lock);

    char message[96];
    snprintf(message, sizeof(message), "[SIG] code=%s press=short seq=%" PRIu32,
             code, seq);
    return sig_post_message(message);
}

static void sig_sender_task(void *arg) {
    (void)arg;
    while (true) {
        xSemaphoreTake(s_sig.lock, portMAX_DELAY);
        bool pending = s_sig.pending;
        xSemaphoreGive(s_sig.lock);
        if (!pending) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        sig_send_result_t res = sig_send_once();
        if (res == SIG_SEND_ACKED) {
            xSemaphoreTake(s_sig.lock, portMAX_DELAY);
            s_sig.last_seq = s_sig.seq;
            strncpy(s_sig.last_code, s_sig.code, sizeof(s_sig.last_code) - 1);
            s_sig.last_code[sizeof(s_sig.last_code) - 1] = '\0';
            s_sig.last_acked = true;
            s_sig.last_ack_ms = esp_timer_get_time() / 1000;
            s_sig.next_seq = s_sig.seq + 1;
            s_sig.pending = false;
            if (s_sig.coalesced) {
                ESP_LOGI(TAG, "acked seq=%" PRIu32 " (%d presses coalesced)",
                         s_sig.last_seq, s_sig.coalesced);
                s_sig.coalesced = 0;
            } else {
                ESP_LOGI(TAG, "acked seq=%" PRIu32, s_sig.last_seq);
            }
            xSemaphoreGive(s_sig.lock);
        } else if (res == SIG_SEND_DOWN) {
            vTaskDelay(pdMS_TO_TICKS(SIG_DOWN_MS));
        } else {
            ESP_LOGW(TAG, "send not acked; retrying in %d s",
                     SIG_RETRY_MS / 1000);
            vTaskDelay(pdMS_TO_TICKS(SIG_RETRY_MS));
        }
    }
}

static void sig_on_press(const char *code) {
    xSemaphoreTake(s_sig.lock, portMAX_DELAY);
    if (!s_sig.pending) {
        s_sig.pending = true;
        strncpy(s_sig.code, code, sizeof(s_sig.code) - 1);
        s_sig.code[sizeof(s_sig.code) - 1] = '\0';
        s_sig.seq = s_sig.next_seq;
        s_sig.coalesced = 0;
        ESP_LOGI(TAG, "press %s: signal seq=%" PRIu32 " queued", code,
                 s_sig.seq);
    } else {
        s_sig.coalesced++;
        ESP_LOGI(TAG, "press %s: coalesced into seq=%" PRIu32, code,
                 s_sig.seq);
    }
    xSemaphoreGive(s_sig.lock);
}

static void sig_button_task(void *arg) {
    (void)arg;
    while (true) {
        int64_t now = esp_timer_get_time() / 1000;
        for (size_t i = 0; i < s_button_count; i++) {
            sig_button_t *b = &s_buttons[i];
            bool pressed = gpio_get_level(b->gpio) == 0;
            if (pressed != b->candidate) {
                b->candidate = pressed;
                b->candidate_since = now;
            } else if (b->candidate != b->stable_pressed &&
                       now - b->candidate_since >= SIG_DEBOUNCE_MS) {
                b->stable_pressed = b->candidate;
                if (b->stable_pressed) sig_on_press(b->code);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(SIG_POLL_MS));
    }
}

/* ---- Button config loading (via the chat: CFGREQ + history) ------- */

static struct {
    int    status;
    char  *buf; /* SIG_HIST_CAP bytes, allocated in PSRAM at load time */
    size_t len;
    bool   done;
} s_cfg_rx;

/* Runs on the session task: copy what arrived, never block. */
static void sig_cfg_on_frame(void *ctx, int status, const uint8_t *data,
                             size_t len, bool end) {
    (void)ctx;
    if (status) s_cfg_rx.status = status;
    if (data && len && s_cfg_rx.buf) {
        size_t room = SIG_HIST_CAP - 1 - s_cfg_rx.len;
        if (len > room) len = room;
        memcpy(s_cfg_rx.buf + s_cfg_rx.len, data, len);
        s_cfg_rx.len += len;
        s_cfg_rx.buf[s_cfg_rx.len] = '\0';
    }
    if (end || status < 0) s_cfg_rx.done = true;
}

/* Minimal JSON string unescape: the [CFG] block arrives as a JSON
 * string value inside the /chat/history reply, so its newlines are
 * "\n" pairs there. Writes the decoded text to out (NUL-terminated).
 * Config lines are ASCII by design; \uXXXX above 0x7F becomes '?'. */
static void sig_json_unescape(const char *in, char *out, size_t cap) {
    size_t o = 0;
    for (size_t i = 0; in[i] && o + 1 < cap; i++) {
        char c = in[i];
        if (c != '\\' || !in[i + 1]) {
            out[o++] = c;
            continue;
        }
        char e = in[++i];
        switch (e) {
            case 'n': out[o++] = '\n'; break;
            case 'r': out[o++] = '\r'; break;
            case 't': out[o++] = '\t'; break;
            case 'u': {
                unsigned v = 0;
                bool hex = true;
                for (int k = 0; k < 4; k++) {
                    char h = in[i + 1 + k];
                    v <<= 4;
                    if (h >= '0' && h <= '9') v |= (unsigned)(h - '0');
                    else if (h >= 'a' && h <= 'f') v |= (unsigned)(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') v |= (unsigned)(h - 'A' + 10);
                    else { hex = false; break; }
                }
                if (hex) i += 4;
                out[o++] = (hex && v < 0x80) ? (char)v : '?';
                break;
            }
            default: out[o++] = e; break; /* \" \\ \/ and anything else */
        }
    }
    out[o] = '\0';
}

/* One GET /chat/history attempt (the newest 2 events: the CFGREQ
 * itself and, once it lands, the agent's [CFG] answer). The reply
 * lands in s_cfg_rx.buf. False on any failure. */
static bool sig_history_fetch(void) {
    s_cfg_rx.status = 0;
    s_cfg_rx.len = 0;
    s_cfg_rx.buf[0] = '\0';
    s_cfg_rx.done = false;
    int64_t id = noise_ctrl_req_open("GET", "/chat/history?limit=2", NULL,
                                     true, sig_cfg_on_frame, NULL);
    if (!id) {
        ESP_LOGW(TAG, "signal config: history open failed");
        return false;
    }
    int64_t deadline = esp_timer_get_time() + 15000 * 1000;
    while (!s_cfg_rx.done && esp_timer_get_time() < deadline) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (!s_cfg_rx.done) {
        ESP_LOGW(TAG, "signal config: history no reply (timeout)");
        noise_ctrl_req_cancel(id);
        return false;
    }
    if (s_cfg_rx.status != 200) {
        ESP_LOGW(TAG, "signal config: history status %d", s_cfg_rx.status);
        return false;
    }
    return true;
}

/* Find a "[CFG] nonce=<nonce>" block in a history reply and decode
 * its lines into out. The block is a JSON string value in the reply,
 * so the raw search works on the escaped form; the segment between
 * the nonce line and "[/CFG]" is unescaped before parsing. Returns
 * false when no block carries this boot's nonce. */
static bool sig_cfg_extract(const char *reply, const char *nonce,
                            char *out, size_t cap) {
    const char *mark = strstr(reply, "[CFG]");
    if (!mark) return false;
    const char *n = strstr(mark, "nonce=");
    if (!n || strncmp(n + 6, nonce, 8) != 0) return false;
    const char *p = n + 6 + 8;
    const char *end = strstr(p, "[/CFG]");
    if (!end || end <= p) return false;
    /* Copy the raw (still escaped) segment, then unescape it. The
     * segment ends at the closing marker or at the string's end
     * quote, whichever the copy hits first after [/CFG]. */
    static char raw[SIG_HIST_CAP];
    size_t r = 0;
    for (; p < end && r + 2 < sizeof(raw); p++) {
        if (*p == '\\' && p[1]) {
            raw[r++] = *p++;
            raw[r++] = *p;
        } else {
            raw[r++] = *p;
        }
    }
    raw[r] = '\0';
    sig_json_unescape(raw, out, cap);
    return true;
}

/* Parse the button list into the button table. The list is one
 * line of "<index>:<GPIO>" pairs separated by commas, e.g.
 * "01:21,02:47,03:48,04:38". The index is the button number and
 * must run in order from 1; the code is built here as "BTN<index>",
 * so no code strings travel. A malformed pair stops the parse and
 * the buttons read so far are kept. Returns the number of
 * buttons. */
static size_t sig_cfg_parse(char *text) {
    size_t n = 0;
    char *p = text;
    while (*p && n < SIG_MAX_BUTTONS) {
        char *end = NULL;
        long idx = strtol(p, &end, 10);
        if (end == p || *end != ':') break;
        p = end + 1;
        long gpio = strtol(p, &end, 10);
        if (end == p || gpio < 0 || gpio > 48) break;
        p = (*end == ',') ? end + 1 : end;
        if (idx != (long)n + 1) break;
        sig_button_t *b = &s_buttons[n++];
        b->gpio = (int)gpio;
        snprintf(b->code, sizeof(b->code), "BTN%d", (int)idx);
        b->stable_pressed = false;
        b->candidate = false;
        b->candidate_since = 0;
    }
    return n;
}

static void sig_start_buttons(void) {
    uint64_t mask = 0;
    for (size_t i = 0; i < s_button_count; i++) {
        mask |= (1ULL << s_buttons[i].gpio);
    }
    gpio_config_t cfg = {
        .pin_bit_mask = mask,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio config failed: %s", esp_err_to_name(err));
        return;
    }
    if (xTaskCreate(sig_button_task, "signal_btn", 2048, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "button task create failed");
        return;
    }
    for (size_t i = 0; i < s_button_count; i++) {
        ESP_LOGI(TAG, "signal button %s ready (GPIO %d, active low)",
                 s_buttons[i].code, s_buttons[i].gpio);
    }
}

/* Ask for the button config through the chat and wait for the
 * agent's [CFG] answer in the history. On success the button table
 * is filled and true is returned. */
static bool sig_cfg_load_via_chat(void) {
    static char text[SIG_CFG_CAP];
    s_cfg_rx.buf = heap_caps_malloc(SIG_HIST_CAP, MALLOC_CAP_SPIRAM |
                                                   MALLOC_CAP_8BIT);
    if (!s_cfg_rx.buf) {
        s_cfg_rx.buf = heap_caps_malloc(SIG_HIST_CAP, MALLOC_CAP_8BIT);
    }
    if (!s_cfg_rx.buf) {
        ESP_LOGE(TAG, "signal config: no memory for history reply");
        return false;
    }

    char nonce[9];
    snprintf(nonce, sizeof(nonce), "%08" PRIx32, esp_random());

    /* The request takes a seq like any other signal, so the agent
     * sees one numbering for the device. */
    xSemaphoreTake(s_sig.lock, portMAX_DELAY);
    uint32_t seq = s_sig.next_seq;
    xSemaphoreGive(s_sig.lock);
    char message[96];
    snprintf(message, sizeof(message),
             "[SIG] code=CFGREQ seq=%" PRIu32 " nonce=%s", seq, nonce);
    bool posted = false;
    for (int try = 0; try < 3 && !posted; try++) {
        posted = sig_post_message(message) == SIG_SEND_ACKED;
        if (!posted) vTaskDelay(pdMS_TO_TICKS(SIG_DOWN_MS));
    }
    if (!posted) {
        ESP_LOGW(TAG, "signal config: CFGREQ not acked");
        heap_caps_free(s_cfg_rx.buf);
        s_cfg_rx.buf = NULL;
        return false;
    }
    xSemaphoreTake(s_sig.lock, portMAX_DELAY);
    if (s_sig.next_seq == seq) s_sig.next_seq = seq + 1;
    xSemaphoreGive(s_sig.lock);

    bool loaded = false;
    int64_t deadline = esp_timer_get_time() + (int64_t)SIG_CFG_TOTAL_MS * 1000;
    while (!loaded && esp_timer_get_time() < deadline) {
        if (sig_history_fetch() &&
            sig_cfg_extract(s_cfg_rx.buf, nonce, text, sizeof(text))) {
            s_button_count = sig_cfg_parse(text);
            loaded = s_button_count > 0;
            if (!loaded) {
                ESP_LOGW(TAG, "signal config: [CFG] carried no valid line");
            }
        }
        if (!loaded) vTaskDelay(pdMS_TO_TICKS(SIG_CFG_POLL_MS));
    }
    heap_caps_free(s_cfg_rx.buf);
    s_cfg_rx.buf = NULL;
    return loaded;
}

/* Loads the button table via the chat, with the compiled-in single
 * button as fallback, then starts the poll task. */
static void sig_config_task(void *arg) {
    (void)arg;
    bool loaded = false;

    int64_t wait_deadline = esp_timer_get_time() + (int64_t)SIG_CFG_WAIT_MS * 1000;
    while (esp_timer_get_time() < wait_deadline) {
        if (noise_ctrl_is_connected()) {
            loaded = sig_cfg_load_via_chat();
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    if (loaded) {
        s_cfg_via_chat = true;
        ESP_LOGI(TAG, "signal buttons loaded via chat (%u buttons)",
                 (unsigned)s_button_count);
    } else {
        s_buttons[0].gpio = CONFIG_HOMEHUB_SIGNAL_GPIO;
        strncpy(s_buttons[0].code, "BTN1", SIG_CODE_CAP - 1);
        s_buttons[0].code[SIG_CODE_CAP - 1] = '\0';
        s_button_count = 1;
        ESP_LOGW(TAG, "signal config not loaded; fallback BTN1 (GPIO %d)",
                 CONFIG_HOMEHUB_SIGNAL_GPIO);
    }
    sig_start_buttons();
    vTaskDelete(NULL);
}

void muse_signal_init(void) {
    memset(&s_sig, 0, sizeof(s_sig));
    s_sig.next_seq = 1;
    s_sig.lock = xSemaphoreCreateMutex();
    if (!s_sig.lock) {
        ESP_LOGE(TAG, "no memory for signal state");
        return;
    }
    if (xTaskCreate(sig_sender_task, "signal_tx", 4096, NULL, 5, NULL) != pdPASS ||
        xTaskCreate(sig_config_task, "signal_cfg", 4096, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "task create failed");
        return;
    }
}

cJSON *muse_signal_last_json(void) {
    cJSON *result = cJSON_CreateObject();
    if (!result) return NULL;
    if (!s_sig.lock) return result;
    xSemaphoreTake(s_sig.lock, portMAX_DELAY);
    cJSON_AddNumberToObject(result, "last_seq", s_sig.last_seq);
    cJSON_AddStringToObject(result, "last_code", s_sig.last_code);
    cJSON_AddBoolToObject(result, "acked", s_sig.last_acked);
    cJSON_AddNumberToObject(result, "last_ack_ms", (double)s_sig.last_ack_ms);
    cJSON_AddBoolToObject(result, "pending", s_sig.pending);
    cJSON_AddNumberToObject(result, "coalesced", s_sig.coalesced);
    cJSON_AddNumberToObject(result, "buttons", (double)s_button_count);
    cJSON_AddStringToObject(result, "config",
                            s_cfg_via_chat ? "chat" : "fallback");
    xSemaphoreGive(s_sig.lock);
    return result;
}

#else /* !CONFIG_HOMEHUB_SIGNAL_BUTTON */

void muse_signal_init(void) {}

cJSON *muse_signal_last_json(void) { return cJSON_CreateObject(); }

#endif
