#include "http_server.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#include "driver/gpio.h"

#include "board_pins.h"
#include "cJSON.h"
#include "controller_api.h"
#include "device_naming.h"
#include "discovery_service.h"
#include "usb_backend.h"
#include "virtual_device.h"
#include "harness_io_expander.h"
#include "harness_dut.h"

#include "neopixel.h"

/* Auto-generated firmware version (from git describe + build timestamp). */
#include "version_autogen.h"

/* Browser-side JS for the status page, embedded at build time via
   EMBED_TXTFILES (web_app.js).  ESP-IDF null-terminates TEXT embeds, so
   the start symbol is safe to use as a C string / stream with strlen. */
extern const char _binary_web_app_js_start[];

#define HTTP_PORT              80
#define HTTP_MAX_CLIENTS       4
#define HTTP_BUF_SIZE          2048
#define HTTP_TASK_STACK        16384
#define HTTP_TASK_PRIORITY     3

static const char *TAG = "http_server";

/* ─────────────────────────────────────────────
 *  Streaming chunked writer — writes directly
 *  to the socket with HTTP chunked encoding.
 *  No fixed-size output buffer needed.
 * ───────────────────────────────────────────── */

/* Send all len bytes, looping over short writes.  Sets *err to the
   errno on failure so callers can stop stream output cleanly. */
static void send_all(int fd, const char *data, size_t len, int *err)
{
    if (err && *err != 0) return;  /* already failed; skip further sends */
    size_t off = 0;
    while (off < len) {
        int n = send(fd, data + off, len - off, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (err) *err = errno;
            return;
        }
        if (n == 0) {  /* peer closed */
            if (err) *err = ECONNRESET;
            return;
        }
        off += (size_t)n;
    }
}

typedef struct {
    int   fd;
    char  buf[1024];
    int   pos;
    int   err;   /* sticky errno — once set, all further sends are skipped */
} stream_t;

static void stream_begin(stream_t *s, int fd, const char *ct)
{
    s->fd  = fd;
    s->pos = 0;
    s->err = 0;
    char hdr[512];
    int n = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: %s\r\n"
        "Transfer-Encoding: chunked\r\n"
        "Connection: close\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "\r\n", ct);
    send_all(fd, hdr, n, &s->err);
}

/* Flush buffered data as a chunk. */
static void stream_flush(stream_t *s)
{
    if (s->pos == 0) return;
    char hdr[16];
    int hn = snprintf(hdr, sizeof(hdr), "%x\r\n", s->pos);
    send_all(s->fd, hdr, hn, &s->err);
    send_all(s->fd, s->buf, s->pos, &s->err);
    send_all(s->fd, "\r\n", 2, &s->err);
    s->pos = 0;
}

/* Write len bytes (may flush and/or directly chunk large data). */
static void stream_write(stream_t *s, const char *data, int len)
{
    if (len < 0) len = (int)strlen(data);  /* -1 means NUL-terminated */
    if (len <= 0) return;
    if (s->err != 0) return;  /* connection already broken */

    /* If it fits in the buffer, just copy. */
    if (s->pos + len <= (int)sizeof(s->buf)) {
        memcpy(s->buf + s->pos, data, len);
        s->pos += len;
        return;
    }

    /* Flush what we have first. */
    stream_flush(s);

    /* If still too big, send as its own chunk. */
    if (len > (int)sizeof(s->buf)) {
        char hdr[16];
        int hn = snprintf(hdr, sizeof(hdr), "%x\r\n", len);
        send_all(s->fd, hdr, hn, &s->err);
        send_all(s->fd, data, len, &s->err);
        send_all(s->fd, "\r\n", 2, &s->err);
        return;
    }

    memcpy(s->buf + s->pos, data, len);
    s->pos += len;
}

/* Format and write (via the buffer). */
static void stream_printf(stream_t *s, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    char tmp[2048];
    (void)vsnprintf(tmp, sizeof(tmp), fmt, args);
    va_end(args);
    size_t n = strlen(tmp);
    if (n > 0) stream_write(s, tmp, n);
}

/* Finish chunked response. */
static void stream_end(stream_t *s)
{
    stream_flush(s);
    send_all(s->fd, "0\r\n\r\n", 5, &s->err);
    if (s->err != 0) {
        ESP_LOGW(TAG, "stream response truncated (send err=%d)", s->err);
    }
}

/* ─────────────────────────────────────────────
 *  GPIO / IO-expander helpers
 * ───────────────────────────────────────────── */

typedef struct {
    const char *label;
    int8_t      gpio;      /* >=0 native GPIO; -1 plain expander pin; -2 pull-only header pin (no GPIO) */
    const char *pull_exp;  /* expander label ("E<n>_<p>") that drives this pin's pull resistor, or NULL */
    const char *desc;      /* optional human-readable role shown next to the label (e.g. "LED"), or NULL */
    char dir;              /* runtime-tracked direction: 'i' = input/hi-Z, 'o' = output/driving */
} pin_entry_t;

#define MAX_DUT_PINS 87
static pin_entry_t s_dut_pins[MAX_DUT_PINS];
static int s_dut_pin_count;

/* ─────────────────────────────────────────────
 *  DUT-header → IO-expander pull resistor map.
 *
 *  On the P4HIL board each DUT header pin (T and B) has a 2.2k series
 *  resistor to an IO-expander output.  Driving that expander pin high pulls
 *  the header line up, low pulls it down, and putting it in hi-Z (input)
 *  disables the pull.  The expander pins are laid out in order across the
 *  three expanders, skipping each expander's pin 15 (which are the special
 *  lines: E0_15/E2_15 = LEDs, E1_15 = USB host VBUS power, and E0_0..E0_5 =
 *  the external connector).
 *
 *  Labeling note: the board silkscreen labels the header's T row T2-T20 and
 *  the B row B1-B20.  The KiCad netlist/schematic, however, labels those same
 *  T nets T1-T19 (off by one); the B labels match on both.  The map below was
 *  extracted from the p4hil KiCad netlist and the T labels have been
 *  renumbered +1 to the board silkscreen so they line up with board_pins.c
 *  (this keeps each header row's pull resistor wired to its own expander and
 *  avoids a phantom T1 row).  All of T2-T20 and B1-B20 have a pull resistor.
 * ───────────────────────────────────────────── */
static const struct { const char *hdr; const char *exp; } k_pull_map[] = {
    {"B1","E0_6"}, {"B2","E0_7"}, {"T2","E0_8"}, {"B3","E0_9"},
    {"T3","E0_10"}, {"B4","E0_11"}, {"T4","E0_12"}, {"B5","E0_13"}, {"T5","E0_14"},
    {"B6","E1_0"}, {"T6","E1_1"}, {"B7","E1_2"}, {"T7","E1_3"},
    {"B8","E1_4"}, {"T8","E1_5"}, {"B9","E1_6"}, {"T9","E1_7"},
    {"B10","E1_8"}, {"T10","E1_9"}, {"B11","E1_10"}, {"T11","E1_11"},
    {"B12","E1_12"}, {"T12","E1_13"}, {"B13","E1_14"},
    {"T13","E2_0"}, {"B14","E2_1"}, {"T14","E2_2"}, {"B15","E2_3"},
    {"T15","E2_4"}, {"B16","E2_5"}, {"T16","E2_6"}, {"B17","E2_7"},
    {"T17","E2_8"}, {"B18","E2_9"}, {"T18","E2_10"}, {"B19","E2_11"},
    {"T19","E2_12"}, {"B20","E2_13"}, {"T20","E2_14"},
};
#define PULL_MAP_COUNT  (int)(sizeof(k_pull_map)/sizeof(k_pull_map[0]))

static const char *pull_exp_for_header(const char *hdr)
{
    for (int i = 0; i < PULL_MAP_COUNT; i++)
        if (strcasecmp(k_pull_map[i].hdr, hdr) == 0) return k_pull_map[i].exp;
    return NULL;
}

static int dut_pin_index_with_label(const char *label)
{
    for (int i = 0; i < s_dut_pin_count; i++)
        if (strcasecmp(s_dut_pins[i].label, label) == 0) return i;
    return -1;
}

/* IO expander I2C bus pins (board-specific, p4hil: SDA=35, SCL=36) */
#define IO_EXPANDER_SDA  35
#define IO_EXPANDER_SCL  36

/* I2C addresses for the three PI4IOE5V9535 expanders (A2=A1=A0 pins). */
#define IO_EXPANDER_ADDR_0  0x20
#define IO_EXPANDER_ADDR_1  0x21
#define IO_EXPANDER_ADDR_2  0x22

/* Expander state */
static bool s_expander_inited = false;

static void init_io_expander(void)
{
    /* main.c initialises the IO expander before usb_backend_start(), so by
       the time the HTTP task runs it should already be set up.  If not, we
       just mark it unavailable — the expander init in main.c will complete
       shortly and the next HTTP request will pick it up. */
    if (harness_io_expander_is_initialized()) {
        s_expander_inited = true;
        ESP_LOGI(TAG, "IO expander already initialised by main, reusing");
    } else {
        s_expander_inited = false;
        ESP_LOGW(TAG, "IO expander not yet initialised — will be ready shortly");
    }
}

/*
 * Parse an expander label of the form "E<exp_idx>_<pin>" e.g. "E0_5".
 * Returns the expander index via *exp_idx and the pin number via *pin.
 * Returns true on success, false on parse failure.
 */
static bool expander_label_parse(const char *label, int *exp_idx, int *pin)
{
    if (!label || (label[0] != 'E' && label[0] != 'e')) return false;
    int e, p;
    if (sscanf(label + 1, "%d_%d", &e, &p) == 2 &&
        e >= 0 && e < IO_EXPANDER_MAX_COUNT &&
        p >= 0 && p < IO_EXPANDER_PIN_COUNT) {
        *exp_idx = e;
        *pin = p;
        return true;
    }
    return false;
}

/* Read a pin: native GPIO if gpio>=0, otherwise expander */
static int read_gpio_safe(int8_t gpio, const char *label)
{
    int val;
    if (gpio >= 0) {
        val = gpio_get_level(gpio);
        ESP_LOGI(TAG, "read pin %s (GPIO%d) = %d", label, gpio, val);
    } else {
        int exp_idx, epin;
        if (!expander_label_parse(label, &exp_idx, &epin) || !s_expander_inited)
            return -1;
        val = harness_io_expander_read_pin(exp_idx, (uint8_t)epin);
        ESP_LOGI(TAG, "read pin %s (Exp%d[%d]) = %d", label, exp_idx, epin, val);
    }
    return val;
}

/* Current configuration of a pin as a short string for the JSON live feed:
   "in" (input / hi-Z) or "out" (driving).  Pull-only header pins (gpio
   == -2) have no native pin and always report "in".  For native GPIO the
   direction is the runtime-tracked value in pin->dir (ESP-IDF's public
   gpio driver has no get_direction accessor); for expander pins we read
   the direction register directly so the reported state stays accurate
   even when another component (e.g. usb_backend) owns the pin. */
static const char *pin_direction_str(const pin_entry_t *pin)
{
    if (pin->gpio == -2)
        return "in";
    if (pin->gpio < 0) {
        /* Expander pin: bit set in the direction register == input. */
        int exp_idx, epin;
        if (!expander_label_parse(pin->label, &exp_idx, &epin) || !s_expander_inited)
            return "in";
        uint16_t cfg = 0;
        if (harness_io_expander_get_dir_all(exp_idx, &cfg) != ESP_OK)
            return "in";
        return (cfg & (1u << epin)) ? "in" : "out";
    }
    return (pin->dir == 'o') ? "out" : "in";
}

static bool config_pin_output(pin_entry_t *pin)
{
    if (pin->gpio >= 0) {
        gpio_config_t cfg = {
            .pin_bit_mask = 1ULL << pin->gpio,
            .mode = GPIO_MODE_INPUT_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        esp_err_t err = gpio_config(&cfg);
        if (err == ESP_OK) {
            pin->dir = 'o';
            ESP_LOGI(TAG, "GPIO %s -> output", pin->label);
        }
        return err == ESP_OK;
    }
    int exp_idx, epin;
    if (!expander_label_parse(pin->label, &exp_idx, &epin) || !s_expander_inited)
        return false;
    esp_err_t err = harness_io_expander_set_dir(exp_idx, (uint8_t)epin, false);
    if (err == ESP_OK) {
        pin->dir = 'o';
        ESP_LOGI(TAG, "Expander %s -> output", pin->label);
    }
    return err == ESP_OK;
}

static bool config_pin_input(pin_entry_t *pin)
{
    if (pin->gpio >= 0) {
        gpio_config_t cfg = {
            .pin_bit_mask = 1ULL << pin->gpio,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        esp_err_t err = gpio_config(&cfg);
        if (err == ESP_OK) {
            pin->dir = 'i';
            ESP_LOGI(TAG, "GPIO %s -> input", pin->label);
        }
        return err == ESP_OK;
    }
    int exp_idx, epin;
    if (!expander_label_parse(pin->label, &exp_idx, &epin) || !s_expander_inited)
        return false;
    esp_err_t err = harness_io_expander_set_dir(exp_idx, (uint8_t)epin, true);
    if (err == ESP_OK) {
        pin->dir = 'i';
        ESP_LOGI(TAG, "Expander %s -> input", pin->label);
    }
    return err == ESP_OK;
}

static bool set_pin_level(pin_entry_t *pin, int level)
{
    if (pin->gpio >= 0) {
        gpio_set_level(pin->gpio, level);
        ESP_LOGI(TAG, "GPIO %s = %d", pin->label, level);
        return true;
    }
    int exp_idx, epin;
    if (!expander_label_parse(pin->label, &exp_idx, &epin) || !s_expander_inited)
        return false;
    esp_err_t err = harness_io_expander_write_pin(exp_idx, (uint8_t)epin, level != 0);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Expander %s = %d", pin->label, level);
    }
    return err == ESP_OK;
}

/* Current pull state of an expander-driven pull resistor:
   input(hi-Z) -> "none", output high -> "up", output low -> "down".
   Used to render the pull dropdown selection and for the live JSON feed. */
static const char *expander_pull_state(const char *exp)
{
    int e, p;
    if (!exp || !s_expander_inited || !expander_label_parse(exp, &e, &p))
        return "none";
    uint16_t cfg = 0;
    if (harness_io_expander_get_dir_all(e, &cfg) != ESP_OK) return "none";
    if (cfg & (1u << p)) return "none";          /* configured as input -> no pull */
    int v = harness_io_expander_read_pin(e, (uint8_t)p);
    return (v > 0) ? "up" : "down";
}

/* ─────────────────────────────────────────────
 *  Pin table construction
 * ───────────────────────────────────────────── */

static void collect_dut_pins(void)
{
    size_t count;
    const board_pin_t *pins = board_get_pins(&count);
    s_dut_pin_count = 0;

    /* Native DUT header pins (T and B) from the board pin table.  Each gets a
       pull resistor if it appears in k_pull_map. */
    for (size_t i = 0; i < count && s_dut_pin_count < MAX_DUT_PINS; i++) {
        const char *l = pins[i].label;
        if ((l[0] == 'T' || l[0] == 'B') && l[1] >= '1' && l[1] <= '9') {
            s_dut_pins[s_dut_pin_count].label    = pins[i].label;
            s_dut_pins[s_dut_pin_count].gpio     = pins[i].gpio;
            s_dut_pins[s_dut_pin_count].pull_exp = pull_exp_for_header(l);
            s_dut_pins[s_dut_pin_count].desc     = NULL;
            s_dut_pins[s_dut_pin_count].dir      = 'i';
            s_dut_pin_count++;
        }
    }

    /* Header pins that have a pull but NO native GPIO.  They still need a row
       so their pull dropdown can be shown.  (Currently none on P4HIL, since
       every B1-B20 and T2-T20 header pin has a native GPIO; kept as a
       fallback in case a future board has a pull-only header line.) */
    if (s_expander_inited) {
        for (int i = 0; i < PULL_MAP_COUNT && s_dut_pin_count < MAX_DUT_PINS; i++) {
            if (dut_pin_index_with_label(k_pull_map[i].hdr) >= 0) continue;
            s_dut_pins[s_dut_pin_count].label    = k_pull_map[i].hdr;
            s_dut_pins[s_dut_pin_count].gpio     = -2;  /* pull-only header pin */
            s_dut_pins[s_dut_pin_count].pull_exp = k_pull_map[i].exp;
            s_dut_pins[s_dut_pin_count].desc     = NULL;
            s_dut_pins[s_dut_pin_count].dir      = 'i';
            s_dut_pin_count++;
        }
    }

    /* Only the special IO-expander lines get their own rows.  Everything
       else on the expanders is a header pull resistor and is exposed through
       the pull dropdown on the matching T/B row instead. */
    if (s_expander_inited) {
        static const struct { const char *label; const char *desc; } k_special[] = {
            {"E0_0", NULL},  {"E0_1", NULL},  {"E0_2", NULL},  /* connector */
            {"E0_3", NULL},  {"E0_4", NULL},  {"E0_5", NULL},
            {"E0_15", "LED"},  {"E1_15", "USB power"},  {"E2_15", "LED"},
        };
        for (size_t i = 0; i < sizeof(k_special)/sizeof(k_special[0])
             && s_dut_pin_count < MAX_DUT_PINS; i++) {
            s_dut_pins[s_dut_pin_count].label    = k_special[i].label;
            s_dut_pins[s_dut_pin_count].gpio     = -1;
            s_dut_pins[s_dut_pin_count].pull_exp = NULL;
            s_dut_pins[s_dut_pin_count].desc     = k_special[i].desc;
            s_dut_pins[s_dut_pin_count].dir      = 'i';
            s_dut_pin_count++;
        }
    }
}

/* Configure every DUT pin as an input on startup, so gpio_get_level()
   reads the external pin level rather than a stale output latch from
   a previous SCPI or UI session. */
static void init_dut_pin_directions(void)
{
    for (int i = 0; i < s_dut_pin_count; i++) {
        if (s_dut_pins[i].gpio >= 0) {
            gpio_config_t cfg = {
                .pin_bit_mask = 1ULL << s_dut_pins[i].gpio,
                .mode = GPIO_MODE_INPUT,
                .pull_up_en = GPIO_PULLUP_DISABLE,
                .pull_down_en = GPIO_PULLDOWN_DISABLE,
                .intr_type = GPIO_INTR_DISABLE,
            };
            gpio_config(&cfg);
            s_dut_pins[i].dir = 'i';
        } else if (s_dut_pins[i].gpio == -1) {
            /* Expander pin — set as input via the expander driver.
               Skip the USB-host VBUS power line: usb_backend owns that pin and
               drives it high on boot; resetting it to input here would cut
               USB power.  Pull-only header rows (gpio == -2) have no native
               pin to configure and are left at the expander power-up default
               (input == no pull). */
            if (s_dut_pins[i].desc && strcmp(s_dut_pins[i].desc, "USB power") == 0)
                continue;
            int exp_idx, epin;
            if (expander_label_parse(s_dut_pins[i].label, &exp_idx, &epin) && s_expander_inited) {
                harness_io_expander_set_dir(exp_idx, (uint8_t)epin, true);
            }
        }
    }
    ESP_LOGI(TAG, "Configured all %d DUT pins as inputs", s_dut_pin_count);
}

static pin_entry_t *find_pin_by_label(const char *label)
{
    for (int i = 0; i < s_dut_pin_count; i++) {
        if (strcasecmp(s_dut_pins[i].label, label) == 0)
            return &s_dut_pins[i];
    }
    /* Pull-resistor expander pins are not rows but are still controllable
       via the pull dropdown; synthesise a transient entry for them so the
       normal read/write/toggle handlers work. */
    if (s_expander_inited) {
        int exp_idx, epin;
        if (expander_label_parse(label, &exp_idx, &epin)) {
            static pin_entry_t syn;
            syn.label    = label;
            syn.gpio     = -1;
            syn.pull_exp = NULL;
            syn.desc     = NULL;
            return &syn;
        }
    }
    return NULL;
}

/* ─────────────────────────────────────────────
 *  JSON helpers
 * ───────────────────────────────────────────── */

typedef struct {
    char method[16];
    char url[256];
    int  content_length;
    char auth[160];             /* Authorization header value, "" if absent */
} http_request_t;

/* Find a header (case-insensitive name) in the request head and copy its
   value.  Returns false if absent. */
static bool header_value(const char *buf, size_t len, const char *name, char *out, size_t out_size)
{
    size_t nlen = strlen(name);
    const char *end = buf + len;
    const char *line = memchr(buf, '\n', len);   /* skip the request line */
    while (line != NULL && line + 1 < end) {
        line++;
        if (*line == '\r' || *line == '\n') {
            break;                                /* end of headers */
        }
        if ((size_t)(end - line) > nlen && strncasecmp(line, name, nlen) == 0 && line[nlen] == ':') {
            const char *v = line + nlen + 1;
            while (v < end && *v == ' ') v++;
            size_t o = 0;
            while (v < end && *v != '\r' && *v != '\n' && o + 1 < out_size) {
                out[o++] = *v++;
            }
            out[o] = '\0';
            return true;
        }
        line = memchr(line, '\n', end - line);
    }
    return false;
}

static bool parse_request(const char *buf, size_t len, http_request_t *req)
{
    memset(req, 0, sizeof(*req));
    const char *eol = (const char *)memchr(buf, '\n', len);
    if (!eol) return false;
    if (sscanf(buf, "%15s %255s", req->method, req->url) < 2) return false;
    char cl[16];
    if (header_value(buf, len, "Content-Length", cl, sizeof(cl))) {
        req->content_length = atoi(cl);
    }
    header_value(buf, len, "Authorization", req->auth, sizeof(req->auth));
    return true;
}

static const char *path_after(const char *url, const char *prefix)
{
    size_t plen = strlen(prefix);
    return (strncmp(url, prefix, plen) == 0) ? url + plen : NULL;
}

/* Locate a JSON string value for `key` starting at `from`.  On success
   returns a pointer to the first char inside the value's quotes and writes
   the value length (excluding the surrounding quotes) to *vlen.  Returns
   NULL if the key is not found or its value is not a quoted string.  Used
   for the small config import format (ad-hoc, like the rest of this
   file's JSON parsing). */
static const char *json_str_after(const char *from, const char *key, size_t *vlen)
{
    const char *k = strstr(from, key);
    if (!k) return NULL;
    const char *c = strchr(k + strlen(key), ':');
    if (!c) return NULL;
    const char *q1 = strchr(c, '"');
    if (!q1) return NULL;
    const char *q2 = strchr(q1 + 1, '"');
    if (!q2) return NULL;
    *vlen = (size_t)(q2 - q1 - 1);
    return q1 + 1;
}

static void build_pins_json(char *buf, size_t buf_size)
{
    int off = 0;
    off += snprintf(buf + off, buf_size - off, "{\"pins\":[");
    for (int i = 0; i < s_dut_pin_count; i++) {
        int val = read_gpio_safe(s_dut_pins[i].gpio, s_dut_pins[i].label);
        char wire_name[HARNESS_DUT_MAX_LABEL_LEN + 1];
        if (!harness_dut_get_wire(s_dut_pins[i].label, wire_name, sizeof(wire_name))) {
            wire_name[0] = '\0';
        }
        off += snprintf(buf + off, buf_size - off,
            "%c{\"name\":\"%s\",\"gpio\":%d,\"value\":%d,\"dir\":\"%s\",\"wire\":\"%s\"}",
            (i > 0) ? ',' : ' ',
            s_dut_pins[i].label, s_dut_pins[i].gpio, val,
            pin_direction_str(&s_dut_pins[i]), wire_name);
        if ((size_t)off >= buf_size) break;
    }
    /* Pull-resistor state for every header pin that has one, so the page's
       pull dropdowns can be kept in sync. */
    off += snprintf(buf + off, buf_size - off, "],\"pulls\":[");
    if (s_expander_inited) {
        for (int i = 0; i < PULL_MAP_COUNT; i++) {
            const char *st = expander_pull_state(k_pull_map[i].exp);
            off += snprintf(buf + off, buf_size - off,
                "%c{\"header\":\"%s\",\"state\":\"%s\"}",
                (i > 0) ? ',' : ' ',
                k_pull_map[i].hdr, st);
            if ((size_t)off >= buf_size) break;
        }
    }
    off += snprintf(buf + off, buf_size - off, "]}");
}

static void build_devices_json(char *buf, size_t buf_size)
{
    size_t devs_max = CONFIG_USBIP_MAX_DEVICES + VIRTUAL_DEVICE_MAX;
    usbip_backend_device_t *devs = malloc(devs_max * sizeof(usbip_backend_device_t));
    size_t count = 0;
    int off = 0;
    off += snprintf(buf + off, buf_size - off, "{\"devices\":[");
    if (devs) {
        count = usb_backend_get_devices(devs, devs_max);
        for (size_t i = 0; i < count; i++) {
            /* Look up friendly name for this device */
            char friendly_name[DEVICE_NAME_MAX_LEN] = "";
            esp_err_t name_err = device_naming_get_device_name(devs[i].busid,
                                    friendly_name, sizeof(friendly_name));
            const char *name = (name_err == ESP_OK && strlen(friendly_name) > 0)
                               ? friendly_name : "";
            off += snprintf(buf + off, buf_size - off,
                "%c{\"busid\":\"%s\",\"vendor\":\"0x%04x\",\"product\":\"0x%04x\","
                "\"bcdDevice\":\"0x%04x\",\"speed\":%lu,\"class\":\"0x%02x\","
                "\"name\":\"%s\"}",
                (i > 0) ? ',' : ' ',
                devs[i].busid, devs[i].id_vendor, devs[i].id_product,
                devs[i].bcd_device, (unsigned long)devs[i].speed, devs[i].device_class,
                name);
            if ((size_t)off >= buf_size) break;
        }
        free(devs);
    }
    off += snprintf(buf + off, buf_size - off, "]}");
}

static const char *k_json_ct = "application/json; charset=utf-8";

static void send_json_ok(int fd, const char *body, size_t len)
{
    int err = 0;
    char hdr[512];
    int n = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %u\r\n"
        "Connection: close\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "\r\n", k_json_ct, (unsigned)len);
    send_all(fd, hdr, n, &err);
    send_all(fd, body, len, &err);
}

/* Send a controller API response with its real status code. */
static void send_api_response(int fd, const api_response_t *r)
{
    int err = 0;
    char hdr[512];
    int n = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %u\r\n"
        "Connection: close\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Access-Control-Allow-Headers: Authorization, Content-Type, Mcp-Protocol-Version\r\n"
        "Access-Control-Allow-Methods: GET, POST, PUT, DELETE, OPTIONS\r\n"
        "\r\n",
        r->status, controller_api_status_text(r->status),
        r->content_type ? r->content_type : k_json_ct, (unsigned)r->len);
    send_all(fd, hdr, n, &err);
    if (r->body && r->len) {
        send_all(fd, r->body, r->len, &err);
    }
}

static void http_401(int fd)
{
    static const char body[] = "{\"error\":\"unauthorized\"}";
    api_response_t r = { .status = 401, .body = (char *)body, .len = sizeof(body) - 1 };
    send_api_response(fd, &r);
}

static void http_404(int fd)
{
    send_json_ok(fd, "{\"error\":\"Not Found\"}", 20);
}

static void http_400(int fd, const char *msg)
{
    char body[256];
    int n = snprintf(body, sizeof(body), "{\"error\":\"%s\"}", msg);
    send_json_ok(fd, body, n);
}

static void http_405(int fd)
{
    send_json_ok(fd, "{\"error\":\"Method Not Allowed\"}", 28);
}

static void http_500(int fd)
{
    send_json_ok(fd, "{\"error\":\"Internal Server Error\"}", 30);
}

/* ─────────────────────────────────────────────
 *  Streaming HTML page builder
 * ───────────────────────────────────────────── */

/* Escape a string for safe use inside an HTML attribute value (e.g.
   value="...").  Writes to out (always NUL-terminated) and returns out.
   out must be large enough for the worst case (~6x input length). */
static char *html_escape_attr(const char *in, char *out, size_t out_size)
{
    size_t o = 0;
    for (size_t i = 0; in[i] && o + 1 < out_size; i++) {
        char c = in[i];
        const char *esc;
        switch (c) {
            case '"':  esc = "&quot;"; break;
            case '&':  esc = "&amp;";  break;
            case '<':  esc = "&lt;";   break;
            case '>':  esc = "&gt;";   break;
            default:   out[o++] = c; continue;
        }
        size_t el = strlen(esc);
        if (o + el + 1 >= out_size) break;
        memcpy(out + o, esc, el);
        o += el;
    }
    out[o] = '\0';
    return out;
}

static void stream_html_page(stream_t *s)
{
    stream_printf(s,
        "<!DOCTYPE html>\n"
        "<html lang=\"en\">\n"
        "<head>\n"
        "<meta charset=\"utf-8\">\n"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n"
        "<title>USB/IP Bridge &mdash; Status</title>\n"
        "<style>\n");

    stream_printf(s,
        "*{box-sizing:border-box;margin:0;padding:0}\n"
        "body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;"
        "background:#1a1a2e;color:#eee;padding:16px;max-width:1200px;margin:0 auto}\n"
        "h1{color:#e94560;margin-bottom:4px;font-size:1.4rem}\n"
        "h2{background:#e94560;color:#0f3460;display:inline-block;"
        "padding:3px 12px;border-radius:4px;margin:12px 0 6px;font-size:0.95rem}\n"
        "p.sub{color:#888;margin-bottom:12px;font-size:0.8rem}\n"
        "table{width:100%%;border-collapse:collapse;margin-bottom:10px;font-size:0.8rem}\n"
        "th{background:#16213e;padding:6px 8px;text-align:left;font-weight:600;color:#e94560}\n"
        "td{padding:5px 8px;border-bottom:1px solid #16213e}\n"
        "tr:hover{background:#16213e}\n"
        ".hi{color:#4ade80;font-weight:bold}\n"
        ".lo{color:#f87171}\n");

    stream_printf(s,
        "button{background:#0f3460;color:#eee;border:1px solid #e94560;"
        "padding:2px 8px;border-radius:3px;cursor:pointer;font-size:0.75rem}\n"
        "button:hover{background:#e94560}\n"
        "select{background:#0f3460;color:#eee;border:1px solid #555;"
        "padding:1px 4px;border-radius:3px;font-size:0.75rem}\n"
        ".tabs{display:flex;gap:3px;margin-bottom:10px}\n"
        ".tabs button{flex:1;padding:6px;background:#16213e;"
        "border:1px solid #0f3460;border-radius:4px 4px 0 0}\n"
        ".tabs button.on{background:#e94560;border-color:#e94560}\n"
        ".sec{display:none}\n"
        ".sec.on{display:block}\n"
        "input.dut-wire{background:#0f3460;color:#eee;border:1px solid #555;"
        "padding:2px 4px;border-radius:3px;font-size:0.75rem;width:80px}\n"
        "input.txt,textarea{background:#0f3460;color:#eee;border:1px solid #555;"
        "padding:2px 6px;border-radius:3px;font-size:0.75rem}\n"
        "textarea{width:100%%;height:160px;font-family:monospace}\n"
        ".hub{color:#aaa;font-size:0.8rem;margin:8px 0 2px}\n"
        ".warn{color:#fbbf24}\n"
        ".muted{color:#888}\n"
        "button.act{border-color:#4ade80}\n"
        "footer{text-align:center;color:#555;margin-top:20px;font-size:0.7rem}\n"
        "@media(max-width:600px){th,td{padding:3px 5px;font-size:0.7rem}}\n"
        "</style>\n"
        "</head>\n"
        "<body>\n");

    /* Read hostname once to embed directly in the page (avoids a
       client-side XHR and any JS parse-order issues). */
    char hostname_str[DEVICE_NAME_MAX_LEN];
    device_naming_get_hostname(hostname_str, sizeof(hostname_str));

    char board_id_str[DEVICE_NAME_MAX_LEN];
    device_naming_get_board_id(board_id_str, sizeof(board_id_str));

    stream_printf(s,
        "<h1>\U0001f50c USB/IP Bridge</h1>\n"
        "<p class=\"sub\">Board: %s &mdash; %d DUT pins &mdash; FW: %s</p>\n"
        "<p class=\"sub\">Hostname: <span id=\"hostname\">%s</span>",
        board_get_name(), s_dut_pin_count, HARNESS_FW_VERSION, hostname_str);
    stream_write(s,
        " <input id=\"hn-input\" type=\"text\" style=\"display:none;background:#0f3460;color:#eee;border:1px solid #555;padding:2px 6px;border-radius:3px;font-size:0.75rem\">"
        " <button id=\"hn-edit\" onclick=\"he()\" style=\"font-size:0.7rem\">Edit</button>"
        " <button id=\"hn-save\" onclick=\"hs()\" style=\"display:none;font-size:0.7rem\">Save</button>"
        " <button id=\"hn-cancel\" onclick=\"hc()\" style=\"display:none;font-size:0.7rem\">Cancel</button>"
        "</p>\n", -1);
    stream_printf(s,
        "<p class=\"sub\">DUT Board ID: <span id=\"board_id\">%s</span>",
        board_id_str);
    stream_write(s,
        " <input id=\"bi-input\" type=\"text\" style=\"display:none;background:#0f3460;color:#eee;border:1px solid #555;padding:2px 6px;border-radius:3px;font-size:0.75rem\">"
        " <button id=\"bi-edit\" onclick=\"be()\" style=\"font-size:0.7rem\">Edit</button>"
        " <button id=\"bi-save\" onclick=\"bs()\" style=\"display:none;font-size:0.7rem\">Save</button>"
        " <button id=\"bi-cancel\" onclick=\"bc()\" style=\"display:none;font-size:0.7rem\">Cancel</button>"
        "</p>\n"
        "<p class=\"sub\">"
        "<button onclick=\"cfgExport()\" style=\"font-size:0.7rem\">Export Config</button>"
        " <button id=\"cfgClearBtn\" onmousedown=\"cfgClearStart(event)\""
        " onmouseup=\"cfgClearCancel()\" onmouseleave=\"cfgClearCancel()\""
        " ontouchstart=\"cfgClearStart(event)\" ontouchend=\"cfgClearCancel()\""
        " ontouchcancel=\"cfgClearCancel()\""
        " style=\"font-size:0.7rem;background-image:linear-gradient(to right,#e94560,#e94560);"
        "background-repeat:no-repeat;background-size:0% 100%;background-color:#0f3460\">"
        "Clear Config</button>"
        " <button onclick=\"cfgImport()\" style=\"font-size:0.7rem\">Import Config</button>"
        "</p>\n"
        "<p class=\"sub\">API token: <input id=\"tok\" class=\"txt\" type=\"password\" size=\"16\">"
        " <button onclick=\"tokSave()\" style=\"font-size:0.7rem\">Use</button>"
        " <button onclick=\"tokSet()\" style=\"font-size:0.7rem\">Set on bridge</button>"
        " <span id=\"authst\" class=\"muted\"></span>"
        " &mdash; <a href=\"/api/schema\" style=\"color:#888\">API</a>"
        " &middot; MCP at <code>/mcp</code></p>\n"
        "<div class=\"tabs\">\n"
        "<button id=\"tp\" class=\"on\" onclick=\"st('p')\">GPIO Pins</button>\n"
        "<button id=\"tu\" onclick=\"st('u')\">USB Devices</button>\n"
        "<button id=\"th\" onclick=\"st('h')\">Hubs &amp; Power</button>\n"
        "<button id=\"tm\" onclick=\"st('m')\">I2C Mux</button>\n"
        "</div>\n"
        "<div id=\"sp\" class=\"sec on\">\n"
        "<h2>DUT Header Pins</h2>\n"
        "<table><thead><tr><th>Pin</th><th>GPIO</th><th>Value</th>"
        "<th>DUT Name</th><th>Dir</th><th>Action</th><th>Pull</th>"
        "</tr></thead><tbody>\n", -1);

    /* Stream each pin row. */
    for (int i = 0; i < s_dut_pin_count; i++) {
        const pin_entry_t *pin = &s_dut_pins[i];
        int val = read_gpio_safe(pin->gpio, pin->label);
        if (val < 0) val = 0;
        const char *cls = val ? "hi" : "lo";
        const char *pst = (pin->pull_exp && s_expander_inited)
                          ? expander_pull_state(pin->pull_exp) : "none";
        const char *selNone = (strcmp(pst, "none") == 0) ? " selected" : "";
        const char *selUp   = (strcmp(pst, "up")   == 0) ? " selected" : "";
        const char *selDown = (strcmp(pst, "down") == 0) ? " selected" : "";
        const char *dirstr   = pin_direction_str(pin);
        const char *selDirIn  = (strcmp(dirstr, "in")  == 0) ? " selected" : "";
        const char *selDirOut = (strcmp(dirstr, "out") == 0) ? " selected" : "";
        /* Pre-fill the saved DUT wire name into the input's value attribute
           so names appear immediately on page load, not only after the
           client-side refresh polls /api/pins. */
        char wire_raw[HARNESS_DUT_MAX_LABEL_LEN + 1];
        char wire_esc[HARNESS_DUT_MAX_LABEL_LEN * 6 + 1];
        if (!harness_dut_get_wire(pin->label, wire_raw, sizeof(wire_raw)))
            wire_raw[0] = '\0';
        html_escape_attr(wire_raw, wire_esc, sizeof(wire_esc));
        /* Label cell, optionally annotated with the pin's role. */
        char lblcell[80];
        if (pin->desc)
            snprintf(lblcell, sizeof(lblcell),
                "<strong>%s</strong> <span style=\"color:#888\">(%s)</span>",
                pin->label, pin->desc);
        else
            snprintf(lblcell, sizeof(lblcell), "<strong>%s</strong>", pin->label);

        if (pin->gpio == -2) {
            /* Pull-only header pin (no native GPIO) — name + DUT wire + pull. */
            stream_printf(s,
                "<tr><td>%s</td><td>\u2014</td>"
                "<td>\u2014</td>"
                "<td><input id=\"w%s\" type=\"text\" class=\"dut-wire\" size=\"6\" value=\"%s\"></td>"
                "<td>\u2014</td><td>\u2014</td>"
                "<td><select id=\"pl%s\" data-exp=\"%s\" onchange=\"spul(this)\">"
                "<option value=\"none\"%s>None</option>"
                "<option value=\"up\"%s>Up</option>"
                "<option value=\"down\"%s>Down</option>"
                "</select></td></tr>\n",
                lblcell,
                pin->label, wire_esc,
                pin->label, pin->pull_exp ? pin->pull_exp : "",
                selNone, selUp, selDown);
        } else if (pin->gpio < 0) {
            /* Special expander line (connector / LED / USB power). */
            stream_printf(s,
                "<tr><td>%s</td><td>Exp</td>"
                "<td class=\"%s\" id=\"v%s\">%s</td>"
                "<td><input id=\"w%s\" type=\"text\" class=\"dut-wire\" size=\"6\" value=\"%s\"></td>"
                "<td><select id=\"d%s\" onchange=\"sd('%s')\">"
                "<option value=\"in\"%s>IN</option><option value=\"out\"%s>OUT</option>"
                "</select></td>"
                "<td><button onclick=\"rr('%s')\">Read</button>"
                "<button onclick=\"tg('%s')\">Toggle</button></td>"
                "<td>\u2014</td></tr>\n",
                lblcell,
                cls, pin->label, val ? "HIGH" : "LOW",
                pin->label, wire_esc, pin->label, pin->label,
                selDirIn, selDirOut,
                pin->label, pin->label);
        } else {
            /* Native GPIO header pin — optionally with a pull dropdown. */
            stream_printf(s,
                "<tr><td>%s</td><td>GPIO%d</td>"
                "<td class=\"%s\" id=\"v%s\">%s</td>"
                "<td><input id=\"w%s\" type=\"text\" class=\"dut-wire\" size=\"6\" value=\"%s\"></td>"
                "<td><select id=\"d%s\" onchange=\"sd('%s')\">"
                "<option value=\"in\"%s>IN</option><option value=\"out\"%s>OUT</option>"
                "</select></td>"
                "<td><button onclick=\"rr('%s')\">Read</button>"
                "<button onclick=\"tg('%s')\">Toggle</button></td>",
                lblcell, pin->gpio,
                cls, pin->label, val ? "HIGH" : "LOW",
                pin->label, wire_esc, pin->label, pin->label,
                selDirIn, selDirOut,
                pin->label, pin->label);
            if (pin->pull_exp && s_expander_inited) {
                stream_printf(s,
                    "<td><select id=\"pl%s\" data-exp=\"%s\" onchange=\"spul(this)\">"
                    "<option value=\"none\"%s>None</option>"
                    "<option value=\"up\"%s>Up</option>"
                    "<option value=\"down\"%s>Down</option>"
                    "</select></td></tr>\n",
                    pin->label, pin->pull_exp,
                    selNone, selUp, selDown);
            } else {
                stream_write(s, "<td>\u2014</td></tr>\n", -1);
            }
        }
    }

    /* USB devices, hubs and the mux are rendered client-side from the
       controller API (see web_app.js) so they stay live. */
    stream_write(s,
        "</tbody></table></div>\n"
        "<div id=\"su\" class=\"sec\">\n"
        "<h2>USB Devices</h2>\n"
        "<p class=\"sub\">Attach from Linux: <code>usbip attach -r &lt;bridge&gt; -b &lt;busid&gt;</code>."
        " <label><input type=\"checkbox\" id=\"force\"> force (ganged / non-switching hubs)</label></p>\n"
        "<table><thead><tr><th>BusID</th><th>Name</th><th>VID:PID</th><th>Device</th>"
        "<th>Serial</th><th>Speed</th><th>mA</th><th>Port power</th></tr></thead>"
        "<tbody id=\"ud\"><tr><td colspan=\"8\" class=\"muted\">Loading&hellip;</td></tr>\n", -1);

    /* Close the USB-devices section, then emit the page's <script> from
       the embedded web_app.js (see EMBED_TXTFILES in main/CMakeLists.txt).
       Keeping the JS in a separate file lets us syntax-check it with
       `node --check main/web_app.js` instead of maintaining it inline as
       escaped C string literals that the compiler can't validate. */
    stream_write(s,
        "</tbody></table></div>\n"
        "<div id=\"sh\" class=\"sec\">\n"
        "<h2>Hubs &amp; Port Power</h2>\n"
        "<p class=\"sub\"><label><input type=\"checkbox\" id=\"setEnf\" onchange=\"saveSettings()\">"
        " Enforce per-port switching</label> <span class=\"muted\">(refuse ganged / non-switching hubs"
        " unless forced; ganged hubs switch all ports or none)</span><br>"
        "<label><input type=\"checkbox\" id=\"setRst\" onchange=\"saveSettings()\">"
        " Restore port power on hub reset</label> <span class=\"muted\">(ports switched off stay off when"
        " their hub re-enumerates: hub reset, power loss, bridge reboot)</span><br>"
        "Enumeration timeout <input id=\"setTo\" class=\"txt\" type=\"number\" min=\"0\" max=\"60000\""
        " style=\"width:80px\" onchange=\"saveTimeout(null, this.value)\"> ms"
        " <span class=\"muted\">(0 = none; a device that does not answer in time has its port disabled"
        " so others can enumerate; hubs and ports can override it below)</span></p>\n"
        "<div id=\"hubs\" class=\"muted\">Loading&hellip;</div>"
        "<p class=\"sub\"><button onclick=\"allPorts('off')\">All ports off</button>"
        " <button onclick=\"allPorts('on')\">All ports on</button>"
        " <button onclick=\"restorePorts()\">Restore saved states</button>"
        " <span id=\"rstst\" class=\"muted\"></span></p></div>\n"
        "<div id=\"sm\" class=\"sec\">\n"
        "<h2>I2C Strand Mux</h2>\n"
        "<p class=\"sub\">Active: <strong id=\"mact\">&hellip;</strong> <span id=\"mman\"></span>"
        " <button onclick=\"muxIso()\">Isolate all</button>"
        " <button onclick=\"muxProbe()\">Probe bus</button> <span id=\"mprobe\" class=\"muted\"></span></p>\n"
        "<div id=\"mduts\"></div><div id=\"mgroups\"></div>\n"
        "<h2>Topology</h2>\n"
        "<textarea id=\"mtopo\"></textarea>\n"
        "<p class=\"sub\"><button onclick=\"topoLoad()\">Reload</button>"
        " <button onclick=\"topoSave()\">Validate &amp; save</button> <span id=\"mtst\" class=\"muted\"></span></p>"
        "</div>\n"
        "<footer>USB/IP Bridge &mdash; <a href=\"/\" style=\"color:#555\">refresh</a></footer>\n"
        "<script>\n", -1);
    stream_write(s, _binary_web_app_js_start, -1);
    stream_write(s,
        "</script>\n"
        "</body></html>\n", -1);
}

/* ─────────────────────────────────────────────
 *  Neopixel helpers
 * ───────────────────────────────────────────── */

/* Timer handle to turn the neopixel off after 10 seconds. */
static esp_timer_handle_t s_neopixel_timer = NULL;

static void neopixel_off_cb(void *arg)
{
    (void)arg;
    neopixel_off();
}

static void neopixel_show_white_10s(void)
{
    neopixel_set_rgb(255, 255, 255);

    /* Start or restart the 10-second off timer. */
    if (s_neopixel_timer) {
        esp_timer_stop(s_neopixel_timer);
        esp_timer_start_once(s_neopixel_timer, 10 * 1000000); /* 10 seconds in µs */
    }
}

/* ─────────────────────────────────────────────
 *  Request routing
 * ───────────────────────────────────────────── */

static void handle_get_pin(int fd, const char *pin_name)
{
    pin_entry_t *pin = find_pin_by_label(pin_name);
    if (!pin) { http_404(fd); return; }
    int level = read_gpio_safe(pin->gpio, pin->label);
    if (level < 0) level = 0;
    char wire_name[HARNESS_DUT_MAX_LABEL_LEN + 1];
    if (!harness_dut_get_wire(pin->label, wire_name, sizeof(wire_name))) {
        wire_name[0] = '\0';
    }
    char resp[256];
    int rlen = snprintf(resp, sizeof(resp),
        "{\"name\":\"%s\",\"gpio\":%d,\"value\":%d,\"wire\":\"%s\"}",
        pin->label, pin->gpio, level, wire_name);
    send_json_ok(fd, resp, rlen);
}

static void handle_post_pin(int fd, const char *pin_name,
                            const char *body, size_t body_len)
{
    (void)body_len;
    pin_entry_t *pin = find_pin_by_label(pin_name);
    if (!pin) { http_404(fd); return; }

    int new_val = -1;
    char direction[8] = {0};
    char wire[HARNESS_DUT_MAX_LABEL_LEN + 1] = {0};
    bool has_wire = false;

    const char *vk = strstr(body, "\"value\"");
    if (vk) {
        const char *c = strchr(vk, ':');
        if (c) {
            while (*c == ':' || *c == ' ') c++;
            if (*c == '0') new_val = 0;
            else if (*c == '1') new_val = 1;
        }
    }

    const char *dk = strstr(body, "\"direction\"");
    if (dk) {
        const char *c = strchr(dk, ':');
        if (c) {
            const char *q1 = strchr(c, '"');
            if (q1) {
                const char *q2 = strchr(q1 + 1, '"');
                if (q2 && (size_t)(q2 - q1 - 1) < sizeof(direction)) {
                    memcpy(direction, q1 + 1, q2 - q1 - 1);
                    direction[q2 - q1 - 1] = '\0';
                }
            }
        }
    }

    const char *wk = strstr(body, "\"wire\"");
    if (wk) {
        const char *c = strchr(wk, ':');
        if (c) {
            const char *q1 = strchr(c, '"');
            if (q1) {
                const char *q2 = strchr(q1 + 1, '"');
                if (q2 && (size_t)(q2 - q1 - 1) < sizeof(wire)) {
                    memcpy(wire, q1 + 1, q2 - q1 - 1);
                    wire[q2 - q1 - 1] = '\0';
                    has_wire = true;
                }
            }
        }
    }

    if (direction[0]) {
        if (strcmp(direction, "in") == 0) config_pin_input(pin);
        else if (strcmp(direction, "out") == 0) config_pin_output(pin);
    }
    if (new_val >= 0) {
        /* Writing a value implies the pin should drive, so make sure it is
           actually configured as an output.  Without this, a pad that is
           still in GPIO_MODE_INPUT (the startup default) will accept the
           gpio_set_level() call (it only updates the output latch) but the
           real pin stays hi-Z and reads back 0. */
        if (!(direction[0] && strcmp(direction, "in") == 0))
            config_pin_output(pin);
        set_pin_level(pin, new_val);
    }

    if (has_wire) {
        harness_dut_set_wire(pin->label, wire);
    }

    int level = read_gpio_safe(pin->gpio, pin->label);
    if (level < 0) level = 0;
    char wire_name[HARNESS_DUT_MAX_LABEL_LEN + 1];
    if (!harness_dut_get_wire(pin->label, wire_name, sizeof(wire_name))) {
        wire_name[0] = '\0';
    }
    char resp[256];
    int rlen = snprintf(resp, sizeof(resp),
        "{\"name\":\"%s\",\"gpio\":%d,\"value\":%d,\"wire\":\"%s\"}",
        pin->label, pin->gpio, level, wire_name);
    send_json_ok(fd, resp, rlen);
}

/* ─────────────────────────────────────────────
 *  Config export / import
 *
 *  Exports the bridge hostname plus the DUT pin→wire name mappings as a
 *  single JSON document that can be re-imported on the same board (a
 *  backup) or on another harness (moving a DUT between boards — only the
 *  pin labels that exist on the target board are applied).
 *
 *  The exported document carries a "version" field so future format
 *  changes can be detected and handled gracefully on import.
 * ───────────────────────────────────────────── */

#define CONFIG_EXPORT_VERSION  1

/* Parse a JSON number value following `key` in `from`.  Returns true and
   writes the parsed integer to *out on success, false otherwise. */
static bool json_int_after(const char *from, const char *key, int *out)
{
    const char *k = strstr(from, key);
    if (!k) return false;
    const char *c = strchr(k + strlen(key), ':');
    if (!c) return false;
    while (*c == ':' || *c == ' ' || *c == '\t') c++;
    char *end = NULL;
    long v = strtol(c, &end, 10);
    if (end == c) return false;
    *out = (int)v;
    return true;
}

static void handle_get_config(int fd)
{
    char hostname[DEVICE_NAME_MAX_LEN];
    device_naming_get_hostname(hostname, sizeof(hostname));
    char board_id[DEVICE_NAME_MAX_LEN];
    device_naming_get_board_id(board_id, sizeof(board_id));

    stream_t s;
    stream_begin(&s, fd, "application/json; charset=utf-8");
    stream_printf(&s, "{\"version\":%d,\"hostname\":\"%s\",\"board_id\":\"%s\",\"board\":\"%s\",\"wires\":[",
                  CONFIG_EXPORT_VERSION, hostname, board_id, board_get_name());
    bool first = true;
    for (int i = 0; i < s_dut_pin_count; i++) {
        char wire[HARNESS_DUT_MAX_LABEL_LEN + 1];
        if (!harness_dut_get_wire(s_dut_pins[i].label, wire, sizeof(wire)))
            continue;
        if (wire[0] == '\0')
            continue;
        stream_printf(&s, "%s{\"pin\":\"%s\",\"wire\":\"%s\"}",
                      first ? "" : ",", s_dut_pins[i].label, wire);
        first = false;
    }
    stream_write(&s, "]}", -1);
    stream_end(&s);
}

static void handle_post_config(int fd, const char *body, size_t body_len)
{
    (void)body_len;

    /* Reject an unknown format version up front rather than guessing.  A
       missing version field is tolerated as version 1 for backwards
       compatibility with exports made before the field existed. */
    int version = 1;
    if (json_int_after(body, "\"version\"", &version) &&
        version != CONFIG_EXPORT_VERSION) {
        char resp[128];
        int rlen = snprintf(resp, sizeof(resp),
            "{\"error\":\"unsupported config version %d\"}", version);
        send_json_ok(fd, resp, rlen);
        return;
    }

    /* Optional hostname. */
    size_t hn_len;
    const char *hn = json_str_after(body, "\"hostname\"", &hn_len);
    if (hn && hn_len > 0 && hn_len < DEVICE_NAME_MAX_LEN) {
        char hostname[DEVICE_NAME_MAX_LEN];
        memcpy(hostname, hn, hn_len);
        hostname[hn_len] = '\0';
        device_naming_set_hostname(hostname);
    }

    /* Optional board_id. */
    size_t bi_len;
    const char *bi = json_str_after(body, "\"board_id\"", &bi_len);
    if (bi && bi_len < DEVICE_NAME_MAX_LEN) {
        char board_id[DEVICE_NAME_MAX_LEN];
        if (bi_len > 0) {
            memcpy(board_id, bi, bi_len);
            board_id[bi_len] = '\0';
            device_naming_set_board_id(board_id);
        } else {
            device_naming_set_board_id("");
        }
    }

    /* Notify mDNS after hostname/board_id changes above. */
    discovery_service_notify_config_changed();

    /* Replace strategy: clear every known pin's wire, then apply the ones
       from the import.  Pins not listed in the file end up cleared, which
       makes import a true restore. */
    for (int i = 0; i < s_dut_pin_count; i++)
        harness_dut_set_wire(s_dut_pins[i].label, "");

    int applied = 0, skipped = 0;
    const char *wkey = strstr(body, "\"wires\"");
    if (wkey) {
        const char *lb = strchr(wkey, '[');
        const char *p = lb ? lb + 1 : (wkey + 7);
        for (;;) {
            size_t plen;
            const char *pin = json_str_after(p, "\"pin\"", &plen);
            if (!pin) break;
            const char *next_pin = strstr(pin + plen, "\"pin\"");
            size_t wlen;
            const char *wire = json_str_after(pin + plen + 1, "\"wire\"", &wlen);
            if (!wire || (next_pin && wire > next_pin)) {
                /* No wire in this object (or it ran into the next one). */
                skipped++;
                p = next_pin ? next_pin : (pin + plen + 1);
                continue;
            }
            if (plen < HARNESS_DUT_MAX_LABEL_LEN && wlen < HARNESS_DUT_MAX_LABEL_LEN) {
                char pb[HARNESS_DUT_MAX_LABEL_LEN + 1];
                char wb[HARNESS_DUT_MAX_LABEL_LEN + 1];
                memcpy(pb, pin, plen);   pb[plen] = '\0';
                memcpy(wb, wire, wlen);   wb[wlen] = '\0';
                if (dut_pin_index_with_label(pb) >= 0 &&
                    harness_dut_set_wire(pb, wb) == ESP_OK)
                    applied++;
                else
                    skipped++;
            } else {
                skipped++;
            }
            p = wire + wlen + 1;
        }
    }

    char resp[128];
    int rlen = snprintf(resp, sizeof(resp),
        "{\"status\":\"ok\",\"applied\":%d,\"skipped\":%d}",
        applied, skipped);
    send_json_ok(fd, resp, rlen);
}

static void handle_connection(int fd)
{
    char *buf = malloc(HTTP_BUF_SIZE);
    if (!buf) { close(fd); return; }
    size_t total = 0;

    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    while (total < HTTP_BUF_SIZE) {
        int n = read(fd, buf + total, HTTP_BUF_SIZE - total);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            free(buf);
            return;
        }
        if (n == 0) break;
        total += (size_t)n;
        /* Stop at the end of the headers.  Clients usually send the body in
           the same segment, so the terminator need not be at the end of what
           was read; waiting for more would block until SO_RCVTIMEO (2 s). */
        if (memmem(buf, total, "\r\n\r\n", 4) != NULL) break;
    }
    if (total == 0) { free(buf); return; }

    http_request_t req;
    if (!parse_request(buf, total, &req)) { free(buf); http_400(fd, "Bad request"); return; }

    /* Read body if present.  Grows up to 8 KiB so the config import
       endpoint (which may carry the full pin→wire mapping) is not
       truncated. */
    size_t body_cap = 512;
    if (req.content_length > 0 && (size_t)req.content_length + 1 > body_cap)
        body_cap = (size_t)req.content_length + 1;   /* + NUL terminator */
    if (body_cap > 8192) body_cap = 8192;
    char *body_buf = malloc(body_cap);
    size_t body_read = 0;
    if (!body_buf) { free(buf); http_500(fd); return; }
    if (req.content_length > 0) {
        const char *start = memmem(buf, total, "\r\n\r\n", 4);   /* buf is not NUL-terminated */
        if (start) {
            start += 4;
            size_t already = buf + total - start;
            if (already > 0) {
                size_t copy = (already < body_cap - 1) ? already : body_cap - 1;
                memcpy(body_buf, start, copy);
                body_read = copy;
            }
            while (body_read < (size_t)req.content_length && body_read < body_cap - 1) {
                int n = read(fd, body_buf + body_read, body_cap - 1 - body_read);
                if (n <= 0) break;
                body_read += (size_t)n;
            }
        }
    }
    body_buf[body_read] = '\0';  /* NUL-terminate for strstr-based parsers */

    const char *url = req.url;

    /* Controller API: devices, hub port power, analog mux, MCP */
    api_response_t ar;
    if (controller_api_handle(req.method, url, req.auth[0] ? req.auth : NULL,
                              body_buf, body_read, &ar)) {
        send_api_response(fd, &ar);
        cJSON_free(ar.body);
        free(body_buf); free(buf);
        return;
    }

    /* With an API token set, every other mutating request needs it too */
    if (strcmp(req.method, "GET") != 0 && strcmp(req.method, "HEAD") != 0 &&
            !controller_api_authorized(req.auth[0] ? req.auth : NULL)) {
        free(body_buf); free(buf);
        http_401(fd);
        return;
    }

    /* GET / — stream the HTML page */
    if (strcmp(url, "/") == 0 && strcmp(req.method, "GET") == 0) {
        neopixel_show_white_10s();
        stream_t s;
        stream_begin(&s, fd, "text/html; charset=utf-8");
        stream_html_page(&s);
        stream_end(&s);
        free(body_buf); free(buf);
        return;
    }

    /* GET /api/pins */
    if (strcmp(url, "/api/pins") == 0 && strcmp(req.method, "GET") == 0) {
        char *resp = malloc(8192);
        if (!resp) { free(body_buf); free(buf); http_500(fd); return; }
        build_pins_json(resp, 8192);
        send_json_ok(fd, resp, strlen(resp));
        free(resp);
        free(body_buf); free(buf);
        return;
    }

    /* GET /api/devices */
    if (strcmp(url, "/api/devices") == 0 && strcmp(req.method, "GET") == 0) {
        char *resp = malloc(4096);
        if (!resp) { free(body_buf); free(buf); http_500(fd); return; }
        build_devices_json(resp, 4096);
        send_json_ok(fd, resp, strlen(resp));
        free(resp);
        free(body_buf); free(buf);
        return;
    }

    /* GET /api/hostname - return the current bridge hostname */
    if (strcmp(url, "/api/hostname") == 0 && strcmp(req.method, "GET") == 0) {
        char hostname[DEVICE_NAME_MAX_LEN];
        device_naming_get_hostname(hostname, sizeof(hostname));
        char resp[256];
        int rlen = snprintf(resp, sizeof(resp),
            "{\"hostname\":\"%s\"}", hostname);
        send_json_ok(fd, resp, rlen);
        free(body_buf); free(buf);
        return;
    }

    /* POST /api/hostname - set the bridge hostname */
    if (strcmp(url, "/api/hostname") == 0 && strcmp(req.method, "POST") == 0) {
        char hostname[DEVICE_NAME_MAX_LEN] = {0};
        const char *hk = strstr(body_buf, "\"hostname\"");
        if (hk) {
            const char *c = strchr(hk, ':');
            if (c) {
                const char *q1 = strchr(c, '"');
                if (q1) {
                    const char *q2 = strchr(q1 + 1, '"');
                    if (q2 && (size_t)(q2 - q1 - 1) < sizeof(hostname)) {
                        memcpy(hostname, q1 + 1, q2 - q1 - 1);
                        hostname[q2 - q1 - 1] = '\0';
                    }
                }
            }
        }
        esp_err_t err = device_naming_set_hostname(hostname);
        if (err == ESP_OK) {
            discovery_service_notify_config_changed();
            char resp[128];
            int rlen = snprintf(resp, sizeof(resp),
                "{\"status\":\"ok\",\"hostname\":\"%s\"}", hostname);
            send_json_ok(fd, resp, rlen);
        } else {
            http_500(fd);
        }
        free(body_buf); free(buf);
        return;
    }

    /* GET /api/board_id - return the current DUT board ID */
    if (strcmp(url, "/api/board_id") == 0 && strcmp(req.method, "GET") == 0) {
        char board_id[DEVICE_NAME_MAX_LEN];
        device_naming_get_board_id(board_id, sizeof(board_id));
        char resp[256];
        int rlen = snprintf(resp, sizeof(resp),
            "{\"board_id\":\"%s\"}", board_id);
        send_json_ok(fd, resp, rlen);
        free(body_buf); free(buf);
        return;
    }

    /* POST /api/board_id - set the DUT board ID */
    if (strcmp(url, "/api/board_id") == 0 && strcmp(req.method, "POST") == 0) {
        char board_id[DEVICE_NAME_MAX_LEN] = {0};
        const char *bk = strstr(body_buf, "\"board_id\"");
        if (bk) {
            const char *c = strchr(bk, ':');
            if (c) {
                const char *q1 = strchr(c, '"');
                if (q1) {
                    const char *q2 = strchr(q1 + 1, '"');
                    if (q2 && (size_t)(q2 - q1 - 1) < sizeof(board_id)) {
                        memcpy(board_id, q1 + 1, q2 - q1 - 1);
                        board_id[q2 - q1 - 1] = '\0';
                    }
                }
            }
        }
        esp_err_t err = device_naming_set_board_id(board_id);
        if (err == ESP_OK) {
            discovery_service_notify_config_changed();
            char resp[128];
            int rlen = snprintf(resp, sizeof(resp),
                "{\"status\":\"ok\",\"board_id\":\"%s\"}", board_id);
            send_json_ok(fd, resp, rlen);
        } else {
            http_500(fd);
        }
        free(body_buf); free(buf);
        return;
    }

    /* GET /api/config - export hostname + DUT pin/wire mapping as JSON */
    if (strcmp(url, "/api/config") == 0 && strcmp(req.method, "GET") == 0) {
        handle_get_config(fd);
        free(body_buf); free(buf);
        return;
    }

    /* POST /api/config - import (replace) hostname + DUT pin/wire mapping */
    if (strcmp(url, "/api/config") == 0 && strcmp(req.method, "POST") == 0) {
        handle_post_config(fd, body_buf, body_read);
        free(body_buf); free(buf);
        return;
    }

    /* POST /api/config/clear - clear all wire names and reset hostname */
    if (strcmp(url, "/api/config/clear") == 0 && strcmp(req.method, "POST") == 0) {
        neopixel_show_white_10s();
        /* Clear every known pin's wire name. */
        for (int i = 0; i < s_dut_pin_count; i++)
            harness_dut_set_wire(s_dut_pins[i].label, "");
        /* Reset hostname to default (empty string = MAC-based fallback). */
        device_naming_set_hostname("");
        /* Clear the DUT board ID. */
        device_naming_set_board_id("");
        discovery_service_notify_config_changed();
        char resp[128];
        int rlen = snprintf(resp, sizeof(resp),
            "{\"status\":\"ok\",\"message\":\"Configuration cleared\"}");
        send_json_ok(fd, resp, rlen);
        free(body_buf); free(buf);
        return;
    }

    /* POST /api/devices/<busid>/name - set a friendly name for a device */
    const char *dev_rest = path_after(url, "/api/devices/");
    if (dev_rest && strcmp(req.method, "POST") == 0) {
        /* Parse busid until /name */
        const char *name_slash = strstr(dev_rest, "/name");
        if (name_slash && (name_slash[5] == '\0' || name_slash[5] == '/')) {
            size_t busid_len = name_slash - dev_rest;
            if (busid_len > 0 && busid_len < 32) {
                char busid[32] = {0};
                memcpy(busid, dev_rest, busid_len);
                busid[busid_len] = '\0';

                /* Parse name from body */
                char device_name[DEVICE_NAME_MAX_LEN] = {0};
                const char *nk = strstr(body_buf, "\"name\"");
                if (nk) {
                    const char *c = strchr(nk, ':');
                    if (c) {
                        const char *q1 = strchr(c, '"');
                        if (q1) {
                            const char *q2 = strchr(q1 + 1, '"');
                            if (q2 && (size_t)(q2 - q1 - 1) < sizeof(device_name)) {
                                memcpy(device_name, q1 + 1, q2 - q1 - 1);
                                device_name[q2 - q1 - 1] = '\0';
                            }
                        }
                    }
                }
                esp_err_t err = device_naming_set_device_name(busid, device_name);
                if (err == ESP_OK) {
                    char resp[256];
                    int rlen = snprintf(resp, sizeof(resp),
                        "{\"status\":\"ok\",\"busid\":\"%s\",\"name\":\"%s\"}",
                        busid, device_name);
                    send_json_ok(fd, resp, rlen);
                } else {
                    http_500(fd);
                }
                free(body_buf); free(buf);
                return;
            }
        }
    }

    /* GET or POST /api/pins/<name>[/toggle] */
    const char *rest = path_after(url, "/api/pins/");
    if (rest) {
        if (strcmp(req.method, "GET") == 0) {
            handle_get_pin(fd, rest);
            free(body_buf); free(buf);
            return;
        }
        if (strcmp(req.method, "POST") != 0) { free(body_buf); free(buf); http_405(fd); return; }

        const char *slash = strchr(rest, '/');
        if (slash && strcmp(slash, "/toggle") == 0) {
            size_t nlen = slash - rest;
            if (nlen >= 32) { free(body_buf); free(buf); http_400(fd, "Name too long"); return; }
            char pname[32];
            memcpy(pname, rest, nlen);
            pname[nlen] = '\0';

            pin_entry_t *pin = find_pin_by_label(pname);
            if (!pin) { free(body_buf); free(buf); http_404(fd); return; }

            int level = read_gpio_safe(pin->gpio, pin->label);
            if (level < 0) level = 0;
            /* Toggle drives the pin, so switch it to output first; otherwise
               the read-back simply reflects the pull/external level and
               toggling the latch does nothing. */
            config_pin_output(pin);
            set_pin_level(pin, level ? 0 : 1);
            level = read_gpio_safe(pin->gpio, pin->label);
            if (level < 0) level = 0;

            char resp[128];
            int rlen = snprintf(resp, sizeof(resp),
                "{\"name\":\"%s\",\"value\":%d}", pname, level);
            send_json_ok(fd, resp, rlen);
            free(body_buf); free(buf);
            return;
        }

        if (slash == NULL || *slash == '\0') {
            handle_post_pin(fd, rest, body_buf, body_read);
            free(body_buf); free(buf);
            return;
        }
    }

    free(body_buf); free(buf);
    http_404(fd);
}

/* ─────────────────────────────────────────────
 *  Server task
 * ───────────────────────────────────────────── */

static void http_server_task(void *arg)
{
    (void)arg;

    init_io_expander();
    collect_dut_pins();
    init_dut_pin_directions();
    ESP_LOGI(TAG, "Found %d DUT pins on board \"%s\"",
             s_dut_pin_count, board_get_name());

    /* Initialise neopixel (if configured). */
    {
        int np_gpio = CONFIG_USBIP_NEOPIXEL_GPIO;
        if (np_gpio >= 0) {
            esp_err_t err = neopixel_init(np_gpio);
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "Neopixel on GPIO %d", np_gpio);
                /* Create a one-shot timer to turn neopixel off after 10 s. */
                esp_timer_create_args_t tmr = {
                    .callback = neopixel_off_cb,
                    .name = "neopxl_off",
                };
                esp_timer_create(&tmr, &s_neopixel_timer);
            } else {
                ESP_LOGW(TAG, "Neopixel init failed: %s", esp_err_to_name(err));
            }
        } else {
            ESP_LOGI(TAG, "Neopixel disabled");
        }
    }

    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (fd < 0) {
        ESP_LOGE(TAG, "socket(): errno=%d", errno);
        vTaskDelete(NULL);
        return;
    }

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port   = htons(HTTP_PORT),
        .sin_addr   = { htonl(INADDR_ANY) },
    };

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "bind(%d): errno=%d", HTTP_PORT, errno);
        close(fd);
        vTaskDelete(NULL);
        return;
    }

    if (listen(fd, HTTP_MAX_CLIENTS) < 0) {
        ESP_LOGE(TAG, "listen(): errno=%d", errno);
        close(fd);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "HTTP server on port %d", HTTP_PORT);

    struct timeval tv = { .tv_sec = 0, .tv_usec = 100000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    while (true) {
        struct sockaddr_in peer;
        socklen_t plen = sizeof(peer);
        int cf = accept(fd, (struct sockaddr *)&peer, &plen);
        if (cf < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            ESP_LOGW(TAG, "accept(): errno=%d", errno);
            continue;
        }
        handle_connection(cf);
        close(cf);
    }
}

/* ─────────────────────────────────────────────
 *  Public API
 * ───────────────────────────────────────────── */

esp_err_t http_server_start(void)
{
    if (xTaskCreate(http_server_task, "http_server",
                    HTTP_TASK_STACK, NULL,
                    HTTP_TASK_PRIORITY, NULL) != pdPASS) {
        return ESP_FAIL;
    }
    return ESP_OK;
}