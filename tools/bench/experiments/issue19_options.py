# Experiments for bridge issue #19: the two rejected options, measured against the shipped
# fix (see docs/S31-BRIDGE-WORKLOG.md section 12 and the #19 comment). They patch the
# usbip_server.c / usb_backend.c of the commit BEFORE the #19 fix, i.e. run
#   git checkout <fix-commit>~1 -- main/usbip_server.c main/usb_backend.c main/usb_backend.h main/Kconfig.projbuild
#   python tools/bench/experiments/issue19_options.py 1   (or 2)
# then "reconfigure build", flash, and run regression.sh open idle timeout.

"""Apply an experiment to the ORIGINAL (HEAD) usbip_server.c / usb_backend.c.

  exp_patch.py 1   option 1: raise the limits (48 URBs per connection, 64 pool
                   slots, worker stacks in PSRAM); 5 s timeout kept
  exp_patch.py 2   option 2: reserve slots for OUT/control (reader never blocks
                   on IN; pool 16 with IN limited to 12); 5 s timeout kept
"""
import sys
which = sys.argv[1]
srv = 'main/usbip_server.c'
be = 'main/usb_backend.c'
s = open(srv, encoding='utf-8').read()
b = open(be, encoding='utf-8').read()
assert '#define URB_STREAM_MAX_INFLIGHT 8\n' in s
assert '#define USB_BACKEND_NUM_PIPES CONFIG_USBIP_NUM_PIPES\n' in b

if which == '1':
    s = s.replace('#define URB_STREAM_MAX_INFLIGHT 8\n', '#define URB_STREAM_MAX_INFLIGHT 48 /* EXPERIMENT 1 */\n')
    s = s.replace('#include "freertos/task.h"\n', '#include "freertos/task.h"\n#include "freertos/idf_additions.h"\n#include "esp_heap_caps.h"\n')
    old = '''            if (xTaskCreate(urb_worker_task, "urb_wrk",
                            CONFIG_USBIP_SERVER_TASK_STACK, item,
                            CONFIG_USBIP_SERVER_TASK_PRIORITY, NULL) != pdPASS) {'''
    new = '''            if (xTaskCreateWithCaps(urb_worker_task, "urb_wrk",
                            CONFIG_USBIP_SERVER_TASK_STACK, item,
                            CONFIG_USBIP_SERVER_TASK_PRIORITY, NULL,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) { /* EXPERIMENT 1 */'''
    assert old in s
    s = s.replace(old, new)
    b = b.replace('#define USB_BACKEND_NUM_PIPES CONFIG_USBIP_NUM_PIPES\n', '#define USB_BACKEND_NUM_PIPES 64 /* EXPERIMENT 1 */\n')
elif which == '2':
    # Reader never blocks on IN URBs: the in-flight array is big enough for all of
    # cdc-acm's reads, and only OUT/control URBs count toward the old cap of 8.
    s = s.replace('#define URB_STREAM_MAX_INFLIGHT 8\n',
                  '#define URB_STREAM_MAX_INFLIGHT 48 /* EXPERIMENT 2: array */\n#define URB_STREAM_MAX_OUTCTRL 8 /* EXPERIMENT 2: cap for OUT+control */\n')
    old = '''    atomic_int inflight_count;              /* active worker tasks */'''
    new = '''    atomic_int inflight_count;              /* active worker tasks */
    atomic_int outctrl_count;               /* EXPERIMENT 2 */'''
    assert old in s; s = s.replace(old, new)
    old = '''    int slot;                               /* index in stream->in_flight[] */
} urb_work_item_t;'''
    new = '''    int slot;                               /* index in stream->in_flight[] */
    bool outctrl;                           /* EXPERIMENT 2 */
} urb_work_item_t;'''
    assert old in s; s = s.replace(old, new)
    old = '''static void urb_stream_free_slot(urb_stream_ctx_t *ctx, int slot)
{
    ctx->in_flight[slot].item = NULL;'''
    new = '''static void urb_stream_free_slot(urb_stream_ctx_t *ctx, int slot)
{
    if (ctx->in_flight[slot].item != NULL && ctx->in_flight[slot].item->outctrl) {
        atomic_fetch_sub(&ctx->outctrl_count, 1);   /* EXPERIMENT 2 */
    }
    ctx->in_flight[slot].item = NULL;'''
    assert old in s; s = s.replace(old, new)
    old = '''    int slot = -1;
    xSemaphoreTake(ctx->write_mutex, portMAX_DELAY);
    for (int i = 0; i < URB_STREAM_MAX_INFLIGHT; i++) {
        if (ctx->in_flight[i].item == NULL) {'''
    new = '''    int slot = -1;
    xSemaphoreTake(ctx->write_mutex, portMAX_DELAY);
    if (item->outctrl && atomic_load(&ctx->outctrl_count) >= URB_STREAM_MAX_OUTCTRL) {
        xSemaphoreGive(ctx->write_mutex);   /* EXPERIMENT 2 */
        return -1;
    }
    for (int i = 0; i < URB_STREAM_MAX_INFLIGHT; i++) {
        if (ctx->in_flight[i].item == NULL) {
            if (item->outctrl) {
                atomic_fetch_add(&ctx->outctrl_count, 1);
            }'''
    assert old in s; s = s.replace(old, new)
    old = '''        item->slot = urb_stream_alloc_slot(ctx, seqnum, item);
        if (item->slot < 0) {'''
    new = '''        item->outctrl = (direction == USBIP_DIR_OUT || endpoint == 0);   /* EXPERIMENT 2 */
        item->slot = urb_stream_alloc_slot(ctx, seqnum, item);
        if (item->slot < 0) {'''
    assert old in s; s = s.replace(old, new)
    # Backend: IN transfers may hold at most 12 of the 16 pool slots.
    old = '''    SemaphoreHandle_t pipe_avail_sem;           /* counting sem: tracks free pipe slots */'''
    new = '''    SemaphoreHandle_t pipe_avail_sem;           /* counting sem: tracks free pipe slots */
    SemaphoreHandle_t in_avail_sem;             /* EXPERIMENT 2: IN share of the pool */'''
    assert old in b; b = b.replace(old, new)
    old = '''    /* Release all slots into the pool. */'''
    new = '''    s_state.in_avail_sem = xSemaphoreCreateCounting(USB_BACKEND_NUM_PIPES - 4, USB_BACKEND_NUM_PIPES - 4); /* EXPERIMENT 2 */
    /* Release all slots into the pool. */'''
    assert old in b; b = b.replace(old, new)
    old = '''    /* Wait for a free host channel from the shared pool. */
    xSemaphoreTake(s_state.pipe_avail_sem, portMAX_DELAY);'''
    new = '''    /* Wait for a free host channel from the shared pool. */
    const bool exp_in = endpoint_addr != 0 && (endpoint_addr & 0x80);   /* EXPERIMENT 2 */
    if (exp_in) {
        xSemaphoreTake(s_state.in_avail_sem, portMAX_DELAY);
    }
    xSemaphoreTake(s_state.pipe_avail_sem, portMAX_DELAY);'''
    assert old in b; b = b.replace(old, new)
    # every early return / final give of pipe_avail_sem also gives in_avail_sem
    b = b.replace('''        xSemaphoreGive(s_state.pipe_avail_sem);
        return -ENODEV;''', '''        xSemaphoreGive(s_state.pipe_avail_sem);
        if (exp_in) xSemaphoreGive(s_state.in_avail_sem);
        return -ENODEV;''')
    b = b.replace('''            xSemaphoreGive(s_state.pipe_avail_sem);
            ESP_LOGD(TAG, "Unknown endpoint 0x%02x on %s", endpoint_addr, busid);''', '''            xSemaphoreGive(s_state.pipe_avail_sem);
            if (exp_in) xSemaphoreGive(s_state.in_avail_sem);
            ESP_LOGD(TAG, "Unknown endpoint 0x%02x on %s", endpoint_addr, busid);''')
    b = b.replace('''        xSemaphoreGive(s_state.pipe_avail_sem);
        ESP_LOGE(TAG, "No free pipe slot (semaphore accounting error)");''', '''        xSemaphoreGive(s_state.pipe_avail_sem);
        if (exp_in) xSemaphoreGive(s_state.in_avail_sem);
        ESP_LOGE(TAG, "No free pipe slot (semaphore accounting error)");''')
    old = '''    if (give) {
        xSemaphoreGive(s_state.pipe_avail_sem);
    }

    return status;'''
    new = '''    if (give) {
        xSemaphoreGive(s_state.pipe_avail_sem);
    }
    if (exp_in) {
        xSemaphoreGive(s_state.in_avail_sem);   /* EXPERIMENT 2 (orphans keep only the pool slot) */
    }

    return status;'''
    assert old in b; b = b.replace(old, new)
else:
    raise SystemExit("which experiment?")
open(srv, 'w', encoding='utf-8', newline='\n').write(s)
open(be, 'w', encoding='utf-8', newline='\n').write(b)
print("experiment", which, "applied")
