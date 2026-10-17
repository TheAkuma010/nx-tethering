#include <stdio.h>
#include <string.h>
#include <malloc.h>

#include <switch.h>

#define MAX_INTERFACES 8
#define LOG_LINES 9
#define LOG_LINE_SIZE 96
#define RNDIS_INITIALIZE_MSG 0x00000002U
#define RNDIS_INITIALIZE_CMPLT 0x80000002U
#define RNDIS_HALT_MSG 0x00000003U
#define RNDIS_QUERY_MSG 0x00000004U
#define RNDIS_QUERY_CMPLT 0x80000004U
#define RNDIS_STATUS_SUCCESS 0x00000000U
#define RNDIS_OID_802_3_CURRENT_ADDRESS 0x01010102U

typedef struct {
    u32 message_type;
    u32 message_length;
    u32 request_id;
    u32 major_version;
    u32 minor_version;
    u32 max_transfer_size;
} RndisInitializeMessage;

typedef struct {
    u32 message_type;
    u32 message_length;
    u32 request_id;
    u32 status;
} RndisInitializeCompletePrefix;

typedef struct {
    u32 message_type;
    u32 message_length;
    u32 request_id;
} RndisHaltMessage;

typedef struct {
    u32 message_type;
    u32 message_length;
    u32 request_id;
    u32 oid;
    u32 information_buffer_length;
    u32 information_buffer_offset;
    u32 device_vc_handle;
} RndisQueryMessage;

typedef struct {
    u32 message_type;
    u32 message_length;
    u32 request_id;
    u32 status;
    u32 information_buffer_length;
    u32 information_buffer_offset;
} RndisQueryComplete;

static char g_log[LOG_LINES][LOG_LINE_SIZE];
static unsigned int g_log_head;

static void log_result(const char *label, Result rc) {
    snprintf(g_log[g_log_head % LOG_LINES], LOG_LINE_SIZE, "%s: 0x%08X", label, rc);
    g_log_head++;
}

static void draw_header(void) {
    consoleClear();
    printf("NX Tethering Diagnostics 0.3.0\n");
    printf("NRO startup check passed. This app stays open until PLUS.\n");
    printf("A: scan | X: RNDIS control + MAC probe | B: redraw | PLUS: exit\n");
    printf("USB detection is not proof of network connectivity.\n\n");
}

static void print_recent_results(void) {
    printf("Results:\n");
    unsigned int first = g_log_head > LOG_LINES ? g_log_head - LOG_LINES : 0;
    for (unsigned int i = first; i < g_log_head; i++)
        printf("  %s\n", g_log[i % LOG_LINES]);
}

static void print_interface(const UsbHsInterface *interface, int index) {
    struct usb_device_descriptor device;
    struct usb_interface_descriptor desc;
    memcpy(&device, &interface->device_desc, sizeof(device));
    memcpy(&desc, &interface->inf.interface_desc, sizeof(desc));
    printf("[%d] VID:PID %04X:%04X class %02X/%02X/%02X\n", index,
           device.idVendor, device.idProduct, desc.bInterfaceClass,
           desc.bInterfaceSubClass, desc.bInterfaceProtocol);
    printf("    if=%u alt=%u endpoints=%u path=%s\n", desc.bInterfaceNumber,
           desc.bAlternateSetting, desc.bNumEndpoints, interface->pathstr);

    for (unsigned int endpoint = 0; endpoint < 15; endpoint++) {
        const struct usb_endpoint_descriptor *in = &interface->inf.input_endpoint_descs[endpoint];
        const struct usb_endpoint_descriptor *out = &interface->inf.output_endpoint_descs[endpoint];
        if (in->bLength != 0)
            printf("    IN  ep=0x%02X attr=0x%02X mps=%u\n", in->bEndpointAddress, in->bmAttributes, in->wMaxPacketSize);
        if (out->bLength != 0)
            printf("    OUT ep=0x%02X attr=0x%02X mps=%u\n", out->bEndpointAddress, out->bmAttributes, out->wMaxPacketSize);
    }
}

static bool is_rndis_control(const UsbHsInterface *interface) {
    const struct usb_interface_descriptor *desc = &interface->inf.interface_desc;
    return (desc->bInterfaceClass == USB_CLASS_COMM &&
            desc->bInterfaceSubClass == 2 && desc->bInterfaceProtocol == 0xff) ||
           (desc->bInterfaceClass == USB_CLASS_WIRELESS &&
            desc->bInterfaceSubClass == 1 && desc->bInterfaceProtocol == 3);
}

static bool is_rndis_data(const UsbHsInterface *interface) {
    const struct usb_interface_descriptor *desc = &interface->inf.interface_desc;
    return desc->bInterfaceClass == USB_CLASS_DATA &&
           desc->bInterfaceSubClass == 0 && desc->bInterfaceProtocol == 0 &&
           desc->bNumEndpoints >= 2;
}

static bool has_rndis_pair(const UsbHsInterface *interfaces, int count) {
    for (int control = 0; control < count; control++) {
        if (!is_rndis_control(&interfaces[control])) continue;
        for (int data = 0; data < count; data++) {
            if (interfaces[control].device_desc.idVendor == interfaces[data].device_desc.idVendor &&
                interfaces[control].device_desc.idProduct == interfaces[data].device_desc.idProduct &&
                is_rndis_data(&interfaces[data])) return true;
        }
    }
    return false;
}

static void log_transfer_result(const char *label, Result rc, u32 transferred) {
    snprintf(g_log[g_log_head % LOG_LINES], LOG_LINE_SIZE, "%s: 0x%08X (%u bytes)",
             label, rc, transferred);
    g_log_head++;
}

static void probe_rndis(void) {
    UsbHsInterfaceFilter filter;
    UsbHsInterfaceFilter query_filter;
    UsbHsInterface interfaces[MAX_INTERFACES];
    UsbHsInterface available[MAX_INTERFACES];
    UsbHsClientIfSession session;
    RndisInitializeMessage init = {
        .message_type = RNDIS_INITIALIZE_MSG,
        .message_length = sizeof(RndisInitializeMessage),
        .request_id = 1,
        .major_version = 1,
        .minor_version = 0,
        .max_transfer_size = 0x4000,
    };
    RndisHaltMessage halt = {
        .message_type = RNDIS_HALT_MSG,
        .message_length = sizeof(RndisHaltMessage),
        .request_id = 1,
    };
    RndisQueryMessage mac_query = {
        .message_type = RNDIS_QUERY_MSG,
        .message_length = sizeof(RndisQueryMessage),
        .request_id = 2,
        .oid = RNDIS_OID_802_3_CURRENT_ADDRESS,
        .information_buffer_length = 0,
        .information_buffer_offset = 20,
        .device_vc_handle = 0,
    };
    s32 count = 0;
    s32 available_count = 0;
    int control_index = -1;
    Result rc;
    u32 transferred = 0;
    void *buffer = NULL;

    memset(&filter, 0, sizeof(filter));
    memset(&query_filter, 0, sizeof(query_filter));
    memset(interfaces, 0, sizeof(interfaces));
    memset(available, 0, sizeof(available));
    memset(&session, 0, sizeof(session));

    /* usbHsQueryAllInterfaces results cannot be acquired. Discover the
       control signature first, then query available with that exact, unique
       filter. This supports both CDC-ACM and wireless E0/01/03 RNDIS. */
    rc = usbHsQueryAllInterfaces(&query_filter, interfaces, sizeof(interfaces), &count);
    log_result("usbHsQueryAllInterfaces", rc);
    if (R_FAILED(rc) || count <= 0) {
        printf("No USB interfaces found. Enable USB tethering first.\n");
        print_recent_results();
        return;
    }
    for (int i = 0; i < count && i < MAX_INTERFACES; i++) {
        if (is_rndis_control(&interfaces[i])) {
            control_index = i;
            break;
        }
    }
    if (control_index < 0) {
        printf("No RNDIS control signature was found.\n");
        print_recent_results();
        return;
    }

    struct usb_interface_descriptor control_desc;
    struct usb_device_descriptor device_desc;
    memcpy(&control_desc, &interfaces[control_index].inf.interface_desc, sizeof(control_desc));
    memcpy(&device_desc, &interfaces[control_index].device_desc, sizeof(device_desc));
    filter.Flags = UsbHsInterfaceFilterFlags_idVendor |
                   UsbHsInterfaceFilterFlags_idProduct |
                   UsbHsInterfaceFilterFlags_bInterfaceClass |
                   UsbHsInterfaceFilterFlags_bInterfaceSubClass |
                   UsbHsInterfaceFilterFlags_bInterfaceProtocol;
    filter.idVendor = device_desc.idVendor;
    filter.idProduct = device_desc.idProduct;
    filter.bInterfaceClass = control_desc.bInterfaceClass;
    filter.bInterfaceSubClass = control_desc.bInterfaceSubClass;
    filter.bInterfaceProtocol = control_desc.bInterfaceProtocol;
    rc = usbHsQueryAvailableInterfaces(&filter, available, sizeof(available), &available_count);
    log_result("usbHsQueryAvailableInterfaces", rc);
    if (R_FAILED(rc) || available_count <= 0) {
        printf("RNDIS control interface is present but unavailable to this homebrew.\n");
        print_recent_results();
        return;
    }
    control_index = -1;
    for (int i = 0; i < available_count && i < MAX_INTERFACES; i++) {
        if (is_rndis_control(&available[i])) {
            control_index = i;
            break;
        }
    }
    if (control_index < 0) {
        printf("Available interface did not match the expected RNDIS control signature.\n");
        print_recent_results();
        return;
    }

    printf("Acquiring RNDIS control interface %u (ID %d).\n",
           available[control_index].inf.interface_desc.bInterfaceNumber,
           available[control_index].inf.ID);

    /* Only entries returned by usbHsQueryAvailableInterfaces carry an ID
       acceptable to usbHsAcquireUsbIf. QueryAll is discovery-only. */
    rc = usbHsAcquireUsbIf(&session, &available[control_index]);
    log_result("usbHsAcquireUsbIf", rc);
    if (R_FAILED(rc)) {
        print_recent_results();
        return;
    }

    buffer = memalign(0x1000, 0x1000);
    if (buffer == NULL) {
        log_result("aligned buffer allocation", MAKERESULT(Module_Libnx, LibnxError_OutOfMemory));
        goto cleanup;
    }

    memcpy(buffer, &init, sizeof(init));
    rc = usbHsIfCtrlXfer(&session,
                         USB_ENDPOINT_OUT | USB_REQUEST_TYPE_CLASS | USB_RECIPIENT_INTERFACE,
                         0, 0, control_desc.bInterfaceNumber,
                         sizeof(init), buffer, &transferred);
    log_transfer_result("RNDIS SEND_ENCAPSULATED_COMMAND", rc, transferred);
    if (R_FAILED(rc) || transferred != sizeof(init)) goto cleanup;

    memset(buffer, 0, 0x1000);
    transferred = 0;
    rc = usbHsIfCtrlXfer(&session,
                         USB_ENDPOINT_IN | USB_REQUEST_TYPE_CLASS | USB_RECIPIENT_INTERFACE,
                         1, 0, control_desc.bInterfaceNumber,
                         0x1000, buffer, &transferred);
    log_transfer_result("RNDIS GET_ENCAPSULATED_RESPONSE", rc, transferred);
    if (R_SUCCEEDED(rc) && transferred >= sizeof(RndisInitializeCompletePrefix)) {
        RndisInitializeCompletePrefix response;
        memcpy(&response, buffer, sizeof(response));
        printf("RNDIS response: type=0x%08X len=%u request=%u status=0x%08X\n",
               response.message_type, response.message_length, response.request_id, response.status);
        if (response.message_type == RNDIS_INITIALIZE_CMPLT &&
            response.request_id == init.request_id && response.status == RNDIS_STATUS_SUCCESS)
            printf("RNDIS initialization validated; querying the adapter MAC.\n");
        else
            printf("RNDIS response did not validate initialization. Preserve this screen.\n");
    } else if (R_SUCCEEDED(rc)) {
        printf("RNDIS initialization response was too short (%u bytes).\n", transferred);
    }

    if (R_FAILED(rc) || transferred < sizeof(RndisInitializeCompletePrefix)) goto cleanup;
    RndisInitializeCompletePrefix init_response;
    memcpy(&init_response, buffer, sizeof(init_response));
    if (init_response.message_type != RNDIS_INITIALIZE_CMPLT ||
        init_response.request_id != init.request_id || init_response.status != RNDIS_STATUS_SUCCESS)
        goto cleanup;

    memcpy(buffer, &mac_query, sizeof(mac_query));
    transferred = 0;
    rc = usbHsIfCtrlXfer(&session,
                         USB_ENDPOINT_OUT | USB_REQUEST_TYPE_CLASS | USB_RECIPIENT_INTERFACE,
                         0, 0, control_desc.bInterfaceNumber,
                         sizeof(mac_query), buffer, &transferred);
    log_transfer_result("RNDIS QUERY CURRENT_ADDRESS", rc, transferred);
    if (R_FAILED(rc) || transferred != sizeof(mac_query)) goto cleanup;

    memset(buffer, 0, 0x1000);
    transferred = 0;
    rc = usbHsIfCtrlXfer(&session,
                         USB_ENDPOINT_IN | USB_REQUEST_TYPE_CLASS | USB_RECIPIENT_INTERFACE,
                         1, 0, control_desc.bInterfaceNumber,
                         0x1000, buffer, &transferred);
    log_transfer_result("RNDIS GET MAC RESPONSE", rc, transferred);
    if (R_SUCCEEDED(rc) && transferred >= sizeof(RndisQueryComplete)) {
        RndisQueryComplete response;
        memcpy(&response, buffer, sizeof(response));
        size_t data_offset = sizeof(u32) * 2 + response.information_buffer_offset;
        if (response.message_type == RNDIS_QUERY_CMPLT &&
            response.request_id == mac_query.request_id &&
            response.status == RNDIS_STATUS_SUCCESS &&
            response.information_buffer_length == 6 &&
            data_offset <= transferred && 6 <= transferred - data_offset) {
            const u8 *mac = (const u8 *)buffer + data_offset;
            printf("RNDIS adapter MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
                   mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
            printf("Control plane validated. Bulk data transport is next.\n");
        } else {
            printf("RNDIS MAC response was invalid: type=0x%08X len=%u status=0x%08X data=%u@%u\n",
                   response.message_type, response.message_length, response.status,
                   response.information_buffer_length, response.information_buffer_offset);
        }
    } else if (R_SUCCEEDED(rc)) {
        printf("RNDIS MAC response was too short (%u bytes).\n", transferred);
    }

cleanup:
    if (buffer != NULL) {
        memcpy(buffer, &halt, sizeof(halt));
        transferred = 0;
        rc = usbHsIfCtrlXfer(&session,
                             USB_ENDPOINT_OUT | USB_REQUEST_TYPE_CLASS | USB_RECIPIENT_INTERFACE,
                             0, 0, control_desc.bInterfaceNumber,
                             sizeof(halt), buffer, &transferred);
        log_transfer_result("RNDIS HALT", rc, transferred);
        free(buffer);
    }
    usbHsIfClose(&session);
    print_recent_results();
}

static void scan_usb(void) {
    UsbHsInterfaceFilter filter;
    UsbHsInterface interfaces[MAX_INTERFACES];
    s32 count = 0;
    memset(&filter, 0, sizeof(filter));
    memset(interfaces, 0, sizeof(interfaces));

    Result rc = usbHsQueryAllInterfaces(&filter, interfaces, sizeof(interfaces), &count);
    log_result("usbHsQueryAllInterfaces", rc);
    if (R_FAILED(rc)) {
        print_recent_results();
        return;
    }

    printf("USB: %d interface(s) reported (showing up to %d)\n", count, MAX_INTERFACES);
    int displayed = count < MAX_INTERFACES ? count : MAX_INTERFACES;
    for (int i = 0; i < displayed; i++) print_interface(&interfaces[i], i);
    if (has_rndis_pair(interfaces, displayed))
        printf("RNDIS signature found: control + Ethernet data interface.\n");
    else if (count == 0)
        printf("No USB interfaces available to this homebrew.\n");
    else
        printf("No supported RNDIS pair found in the displayed interfaces.\n");
    print_recent_results();
}

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    consoleInit(NULL);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);

    Result usb_rc = usbHsInitialize();
    log_result("usbHsInitialize", usb_rc);
    draw_header();
    print_recent_results();

    while (appletMainLoop()) {
        padUpdate(&pad);
        u64 pressed = padGetButtonsDown(&pad);
        if (pressed & HidNpadButton_Plus) break;
        if (pressed & HidNpadButton_B) {
            draw_header();
            print_recent_results();
        }
        if (pressed & HidNpadButton_A) {
            draw_header();
            if (R_SUCCEEDED(usb_rc))
                scan_usb();
            else {
                printf("usb:hs is unavailable; no USB scan was attempted.\n");
                print_recent_results();
            }
        }
        if (pressed & HidNpadButton_X) {
            draw_header();
            if (R_SUCCEEDED(usb_rc))
                probe_rndis();
            else {
                printf("usb:hs is unavailable; no RNDIS probe was attempted.\n");
                print_recent_results();
            }
        }
        consoleUpdate(NULL);
    }

    if (R_SUCCEEDED(usb_rc)) usbHsExit();
    consoleExit(NULL);
    return 0;
}
