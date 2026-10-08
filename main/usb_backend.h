#ifndef USB_BACKEND_H
#define USB_BACKEND_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "usb/usb_types_ch9.h"

#include "usbip_protocol.h"

typedef struct {
    uint8_t interface_class;
    uint8_t interface_subclass;
    uint8_t interface_protocol;
} usbip_backend_interface_t;

#define USBIP_MAX_ENDPOINTS 16

typedef struct {
    uint8_t address;
    uint8_t attributes;
    uint16_t max_packet_size;
    uint8_t interval;
} usbip_backend_endpoint_t;

typedef struct {
    bool present;
    char path[256];
    char busid[32];
    uint32_t busnum;
    uint32_t devnum;
    uint32_t speed;
    uint16_t id_vendor;
    uint16_t id_product;
    uint16_t bcd_device;
    uint8_t device_class;
    uint8_t device_subclass;
    uint8_t device_protocol;
    uint8_t configuration_value;
    uint8_t num_configurations;
    uint8_t num_interfaces;
    usbip_backend_interface_t interfaces[USBIP_MAX_INTERFACES];
    uint8_t num_endpoints;
    usbip_backend_endpoint_t endpoints[USBIP_MAX_ENDPOINTS];

    /* Topology and descriptor details (zero/empty for virtual devices) */
    uint8_t dev_addr;               /* USB device address */
    uint8_t parent_hub_addr;        /* 0 when on the root port */
    uint8_t parent_port;            /* port number on the parent hub, 0 on the root port */
    uint16_t max_power_ma;          /* from bMaxPower of the active configuration */
    char manufacturer[64];
    char product[64];
    char serial[64];
} usbip_backend_device_t;

#define USB_BACKEND_MAX_HUBS 8

/* An external hub managed by the USB Host Library (hubs are not exported). */
typedef struct {
    uint8_t addr;
    uint8_t parent_hub_addr;        /* 0 when on the root port */
    uint8_t parent_port;
    char path[32];                  /* Linux style port path, e.g. "1-1" or "1-1.4" */
    uint16_t id_vendor;
    uint16_t id_product;
    char manufacturer[64];
    char product[64];
} usb_backend_hub_t;

esp_err_t usb_backend_start(void);
size_t usb_backend_get_devices(usbip_backend_device_t *out_devices, size_t max_devices);
bool usb_backend_get_device_by_busid(const char busid[32], usbip_backend_device_t *out_device);

/* An asynchronous transfer request.  The caller fills the public fields and
   submits it; the backend task calls done() exactly once, when the transfer
   has completed, failed, or been cancelled.  The request and its buffers
   must stay valid until then.  No task waits on a transfer, so a request the
   device does not answer (a pending read) costs only this struct and its
   buffer, like a pending URB on a Linux host. */
typedef struct usb_backend_req usb_backend_req_t;
typedef void (*usb_backend_done_cb_t)(usb_backend_req_t *req, int status, size_t in_len);

struct usb_backend_req {
    /* Filled by the caller */
    char busid[32];
    uint8_t endpoint_addr;          /* 0 = control (setup is used), 0x8N IN, 0x0N OUT */
    usb_setup_packet_t setup;
    const uint8_t *out_data;
    size_t out_len;
    uint8_t *in_data;
    size_t in_capacity;
    usb_backend_done_cb_t done;     /* called from the backend task: status is 0 or a negative errno */
    void *ctx;                      /* for the caller */

    /* Owned by the backend from usb_backend_submit() until done() */
    volatile bool cancel;           /* set by usb_backend_cancel() */
    struct usb_backend_req *next;
    uint32_t seq;
    int status;
};

/* Queue a transfer.  Never blocks; done() follows from the backend task,
   also for immediate failures (-ENODEV, -ENOMEM, ...). */
void usb_backend_submit(usb_backend_req_t *req);

/* Retire a queued or in-flight request: done() is called with -ECONNRESET,
   or with the real result if the transfer completed first.  Safe to call
   more than once, and after done(). */
void usb_backend_cancel(usb_backend_req_t *req);

bool usb_backend_is_interrupt_endpoint(const char busid[32], uint8_t ep_num, uint8_t direction);

/* The client's USB/IP session for a device has ended: release its claimed
   interfaces (and their host channels) until the next session uses them. */
void usb_backend_session_ended(const char busid[32]);

/* Snapshot of the external hubs currently attached. */
size_t usb_backend_get_hubs(usb_backend_hub_t *out_hubs, size_t max_hubs);

#endif
