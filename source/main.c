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
#define RNDIS_SET_MSG 0x00000005U
#define RNDIS_SET_CMPLT 0x80000005U
#define RNDIS_STATUS_SUCCESS 0x00000000U
#define RNDIS_OID_802_3_CURRENT_ADDRESS 0x01010102U
#define RNDIS_OID_GEN_CURRENT_PACKET_FILTER 0x0001010EU
#define RNDIS_DEFAULT_PACKET_FILTER 0x0000000FU
#define RNDIS_PACKET_MSG 0x00000001U
#define RNDIS_PACKET_HEADER_SIZE 44U
#define DHCP_PAYLOAD_SIZE 300U

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

typedef struct {
    u32 message_type;
    u32 message_length;
    u32 request_id;
    u32 oid;
    u32 information_buffer_length;
    u32 information_buffer_offset;
    u32 device_vc_handle;
    u32 packet_filter;
} RndisSetPacketFilterMessage;

typedef struct {
    u32 message_type;
    u32 message_length;
    u32 request_id;
    u32 status;
} RndisSetComplete;

typedef struct {
    u8 address[4];
    u8 server[4];
    u8 subnet_mask[4];
    u8 router[4];
    u8 dns[4];
    u32 lease_seconds;
    bool has_server;
    bool has_subnet_mask;
    bool has_router;
    bool has_dns;
} DhcpLease;

static char g_log[LOG_LINES][LOG_LINE_SIZE];
static unsigned int g_log_head;

static void log_result(const char *label, Result rc) {
    snprintf(g_log[g_log_head % LOG_LINES], LOG_LINE_SIZE, "%s: 0x%08X", label, rc);
    g_log_head++;
}

static void draw_header(void) {
    consoleClear();
    printf("NX Tethering Diagnostics 0.9.0 (gateway ICMP probe)\n");
    printf("NRO startup check passed. This app stays open until PLUS.\n");
    printf("A: scan | X: control | Y: endpoints | R: DHCP handshake | B: redraw | PLUS: exit\n");
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

static void put_be16(u8 *out, u16 value) {
    out[0] = (u8)(value >> 8);
    out[1] = (u8)value;
}

static void put_be32(u8 *out, u32 value) {
    out[0] = (u8)(value >> 24);
    out[1] = (u8)(value >> 16);
    out[2] = (u8)(value >> 8);
    out[3] = (u8)value;
}

static void put_le32(u8 *out, u32 value) {
    out[0] = (u8)value;
    out[1] = (u8)(value >> 8);
    out[2] = (u8)(value >> 16);
    out[3] = (u8)(value >> 24);
}

static u16 get_be16(const u8 *in) {
    return ((u16)in[0] << 8) | in[1];
}

static u32 get_le32(const u8 *in) {
    return (u32)in[0] | ((u32)in[1] << 8) | ((u32)in[2] << 16) | ((u32)in[3] << 24);
}

static u16 ipv4_checksum(const u8 *header, size_t length) {
    u32 sum = 0;
    for (size_t i = 0; i < length; i += 2)
        sum += ((u16)header[i] << 8) | header[i + 1];
    while (sum >> 16) sum = (sum & 0xffffU) + (sum >> 16);
    return (u16)~sum;
}

static size_t build_dhcp_discover(u8 *out, const u8 mac[6]) {
    const size_t ethernet_size = 14;
    const size_t ipv4_size = 20;
    const size_t udp_size = 8;
    const size_t frame_size = ethernet_size + ipv4_size + udp_size + DHCP_PAYLOAD_SIZE;
    u8 *ethernet = out + RNDIS_PACKET_HEADER_SIZE;
    u8 *ipv4 = ethernet + ethernet_size;
    u8 *udp = ipv4 + ipv4_size;
    u8 *dhcp = udp + udp_size;
    u8 *options;

    memset(out, 0, 0x1000);
    put_le32(out + 0, RNDIS_PACKET_MSG);
    put_le32(out + 4, RNDIS_PACKET_HEADER_SIZE + frame_size);
    put_le32(out + 8, RNDIS_PACKET_HEADER_SIZE - 8);
    put_le32(out + 12, frame_size);

    memset(ethernet, 0xff, 6);
    memcpy(ethernet + 6, mac, 6);
    put_be16(ethernet + 12, 0x0800);

    ipv4[0] = 0x45;
    put_be16(ipv4 + 2, ipv4_size + udp_size + DHCP_PAYLOAD_SIZE);
    put_be16(ipv4 + 6, 0x8000);
    ipv4[8] = 64;
    ipv4[9] = 17;
    memset(ipv4 + 12, 0, 4);
    memset(ipv4 + 16, 0xff, 4);
    put_be16(ipv4 + 10, ipv4_checksum(ipv4, ipv4_size));

    put_be16(udp, 68);
    put_be16(udp + 2, 67);
    put_be16(udp + 4, udp_size + DHCP_PAYLOAD_SIZE);
    /* A zero UDP checksum is permitted for IPv4. */

    dhcp[0] = 1;  /* BOOTREQUEST */
    dhcp[1] = 1;  /* Ethernet */
    dhcp[2] = 6;
    put_be32(dhcp + 4, 0x4e585431U); /* "NXT1" */
    put_be16(dhcp + 10, 0x8000);     /* request broadcast reply */
    memcpy(dhcp + 28, mac, 6);
    put_be32(dhcp + 236, 0x63825363U);
    options = dhcp + 240;
    *options++ = 53; *options++ = 1; *options++ = 1; /* DHCPDISCOVER */
    *options++ = 61; *options++ = 7; *options++ = 1;
    memcpy(options, mac, 6); options += 6;
    *options++ = 55; *options++ = 4; /* subnet, router, DNS, lease */
    *options++ = 1; *options++ = 3; *options++ = 6; *options++ = 51;
    *options++ = 255;
    return RNDIS_PACKET_HEADER_SIZE + frame_size;
}

static size_t build_dhcp_request(u8 *out, const u8 mac[6], const DhcpLease *lease) {
    const size_t ethernet_size = 14;
    const size_t ipv4_size = 20;
    const size_t udp_size = 8;
    const size_t frame_size = ethernet_size + ipv4_size + udp_size + DHCP_PAYLOAD_SIZE;
    u8 *ethernet = out + RNDIS_PACKET_HEADER_SIZE;
    u8 *ipv4 = ethernet + ethernet_size;
    u8 *udp = ipv4 + ipv4_size;
    u8 *dhcp = udp + udp_size;
    u8 *options;

    memset(out, 0, 0x1000);
    put_le32(out + 0, RNDIS_PACKET_MSG);
    put_le32(out + 4, RNDIS_PACKET_HEADER_SIZE + frame_size);
    put_le32(out + 8, RNDIS_PACKET_HEADER_SIZE - 8);
    put_le32(out + 12, frame_size);

    memset(ethernet, 0xff, 6);
    memcpy(ethernet + 6, mac, 6);
    put_be16(ethernet + 12, 0x0800);

    ipv4[0] = 0x45;
    put_be16(ipv4 + 2, ipv4_size + udp_size + DHCP_PAYLOAD_SIZE);
    put_be16(ipv4 + 6, 0x8000);
    ipv4[8] = 64;
    ipv4[9] = 17;
    memset(ipv4 + 12, 0, 4);
    memset(ipv4 + 16, 0xff, 4);
    put_be16(ipv4 + 10, ipv4_checksum(ipv4, ipv4_size));

    put_be16(udp, 68);
    put_be16(udp + 2, 67);
    put_be16(udp + 4, udp_size + DHCP_PAYLOAD_SIZE);

    dhcp[0] = 1;  /* BOOTREQUEST */
    dhcp[1] = 1;  /* Ethernet */
    dhcp[2] = 6;
    put_be32(dhcp + 4, 0x4e585431U); /* Must match the DISCOVER transaction. */
    put_be16(dhcp + 10, 0x8000);
    memcpy(dhcp + 28, mac, 6);
    put_be32(dhcp + 236, 0x63825363U);
    options = dhcp + 240;
    *options++ = 53; *options++ = 1; *options++ = 3; /* DHCPREQUEST */
    *options++ = 50; *options++ = 4; /* requested address */
    memcpy(options, lease->address, 4); options += 4;
    *options++ = 54; *options++ = 4; /* selected server */
    memcpy(options, lease->server, 4); options += 4;
    *options++ = 61; *options++ = 7; *options++ = 1;
    memcpy(options, mac, 6); options += 6;
    *options++ = 55; *options++ = 4;
    *options++ = 1; *options++ = 3; *options++ = 6; *options++ = 51;
    *options++ = 255;
    return RNDIS_PACKET_HEADER_SIZE + frame_size;
}

static size_t build_arp_request(u8 *out, const u8 mac[6], const DhcpLease *lease) {
    const size_t ethernet_size = 14;
    const size_t arp_size = 28;
    const size_t frame_size = ethernet_size + arp_size;
    u8 *ethernet = out + RNDIS_PACKET_HEADER_SIZE;
    u8 *arp = ethernet + ethernet_size;

    memset(out, 0, 0x1000);
    put_le32(out + 0, RNDIS_PACKET_MSG);
    put_le32(out + 4, RNDIS_PACKET_HEADER_SIZE + frame_size);
    put_le32(out + 8, RNDIS_PACKET_HEADER_SIZE - 8);
    put_le32(out + 12, frame_size);

    memset(ethernet, 0xff, 6);
    memcpy(ethernet + 6, mac, 6);
    put_be16(ethernet + 12, 0x0806);
    put_be16(arp + 0, 1);       /* Ethernet */
    put_be16(arp + 2, 0x0800);  /* IPv4 */
    arp[4] = 6;
    arp[5] = 4;
    put_be16(arp + 6, 1);       /* request */
    memcpy(arp + 8, mac, 6);
    memcpy(arp + 14, lease->address, 4);
    memset(arp + 18, 0, 6);
    memcpy(arp + 24, lease->router, 4);
    return RNDIS_PACKET_HEADER_SIZE + frame_size;
}

static size_t build_icmp_echo_request(u8 *out, const u8 mac[6], const DhcpLease *lease,
                                      const u8 gateway_mac[6]) {
    const size_t ethernet_size = 14;
    const size_t ipv4_size = 20;
    const size_t icmp_size = 24;
    const size_t frame_size = ethernet_size + ipv4_size + icmp_size;
    u8 *ethernet = out + RNDIS_PACKET_HEADER_SIZE;
    u8 *ipv4 = ethernet + ethernet_size;
    u8 *icmp = ipv4 + ipv4_size;

    memset(out, 0, 0x1000);
    put_le32(out + 0, RNDIS_PACKET_MSG);
    put_le32(out + 4, RNDIS_PACKET_HEADER_SIZE + frame_size);
    put_le32(out + 8, RNDIS_PACKET_HEADER_SIZE - 8);
    put_le32(out + 12, frame_size);

    memcpy(ethernet, gateway_mac, 6);
    memcpy(ethernet + 6, mac, 6);
    put_be16(ethernet + 12, 0x0800);
    ipv4[0] = 0x45;
    put_be16(ipv4 + 2, ipv4_size + icmp_size);
    put_be16(ipv4 + 4, 0x4e58);
    ipv4[8] = 64;
    ipv4[9] = 1; /* ICMP */
    memcpy(ipv4 + 12, lease->address, 4);
    memcpy(ipv4 + 16, lease->router, 4);
    put_be16(ipv4 + 10, ipv4_checksum(ipv4, ipv4_size));

    icmp[0] = 8; /* echo request */
    put_be16(icmp + 4, 0x4e58);
    put_be16(icmp + 6, 1);
    memcpy(icmp + 8, "NX-TETHER-ICMP", 14);
    put_be16(icmp + 2, ipv4_checksum(icmp, icmp_size));
    return RNDIS_PACKET_HEADER_SIZE + frame_size;
}

static void print_ipv4(const char *label, const u8 *address) {
    printf("%s%u.%u.%u.%u", label, address[0], address[1], address[2], address[3]);
}

static u8 inspect_dhcp_reply(const u8 *packet, u32 packet_size, DhcpLease *lease) {
    printf("RX raw (%u bytes):", packet_size);
    u32 dump_length = packet_size < 32 ? packet_size : 32;
    for (u32 i = 0; i < dump_length; i++) printf(" %02X", packet[i]);
    printf("\n");
    if (packet_size < RNDIS_PACKET_HEADER_SIZE) {
        printf("Received bulk data is too short for an RNDIS packet.\n");
        return 0;
    }
    u32 data_offset = 8 + get_le32(packet + 8);
    u32 data_length = get_le32(packet + 12);
    if (get_le32(packet) != RNDIS_PACKET_MSG || data_offset > packet_size ||
        data_length > packet_size - data_offset || data_length < 14) {
        printf("Received bulk data is not a complete RNDIS Ethernet packet.\n");
        return 0;
    }
    const u8 *ethernet = packet + data_offset;
    u16 ether_type = get_be16(ethernet + 12);
    printf("RNDIS Ethernet: %02X:%02X:%02X:%02X:%02X:%02X -> %02X:%02X:%02X:%02X:%02X:%02X type=0x%04X\n",
           ethernet[6], ethernet[7], ethernet[8], ethernet[9], ethernet[10], ethernet[11],
           ethernet[0], ethernet[1], ethernet[2], ethernet[3], ethernet[4], ethernet[5], ether_type);
    if (ether_type == 0x0806) {
        if (data_length >= 14 + 28) {
            const u8 *arp = ethernet + 14;
            printf("ARP opcode=%u sender=%u.%u.%u.%u target=%u.%u.%u.%u\n", get_be16(arp + 6),
                   arp[14], arp[15], arp[16], arp[17], arp[24], arp[25], arp[26], arp[27]);
        }
        return 0;
    }
    if (ether_type == 0x86dd) {
        if (data_length >= 14 + 40) {
            const u8 *ipv6 = ethernet + 14;
            u8 next_header = ipv6[6];
            printf("IPv6 next-header=%u", next_header);
            if (next_header == 58 && data_length >= 14 + 40 + 1)
                printf(" ICMPv6 type=%u", ipv6[40]);
            printf(" (not DHCPv4).\n");
        }
        return 0;
    }
    if (ether_type != 0x0800) {
        printf("Received Ethernet type 0x%04X (not IPv4/DHCP).\n", ether_type);
        return 0;
    }
    const u8 *ipv4 = ethernet + 14;
    u8 ihl = (ipv4[0] & 0x0f) * 4;
    if (ihl < 20 || data_length < (u32)(14 + ihl + 8) || ipv4[9] != 17) {
        printf("Received IPv4 packet is not a DHCP UDP candidate.\n");
        return 0;
    }
    const u8 *udp = ipv4 + ihl;
    if (data_length < (u32)(14 + ihl + 8 + 240)) {
        printf("Received UDP %u -> %u, too short for DHCP/BOOTP.\n", get_be16(udp), get_be16(udp + 2));
        return 0;
    }
    if (get_be16(udp) != 67 || get_be16(udp + 2) != 68) {
        printf("Received UDP %u -> %u (not DHCP reply).\n", get_be16(udp), get_be16(udp + 2));
        return 0;
    }
    const u8 *dhcp = udp + 8;
    if (dhcp[0] != 2 || memcmp(dhcp + 236, "\x63\x82\x53\x63", 4) != 0) {
        printf("Received UDP payload is not a DHCP BOOTREPLY.\n");
        return 0;
    }
    memcpy(lease->address, dhcp + 16, sizeof(lease->address));
    lease->has_server = false;
    lease->has_subnet_mask = false;
    lease->has_router = false;
    lease->has_dns = false;
    lease->lease_seconds = 0;
    print_ipv4("DHCP address: ", lease->address);
    printf("\n");
    const u8 *option = dhcp + 240;
    const u8 *end = packet + data_offset + data_length;
    u8 message_type = 0;
    while (option < end && *option != 255) {
        u8 code = *option++;
        if (code == 0) continue;
        if (option >= end) break;
        u8 length = *option++;
        if (length > (size_t)(end - option)) break;
        if (code == 53 && length == 1) {
            message_type = option[0];
            printf("DHCP message type: %u\n", message_type);
        }
        else if (code == 54 && length == 4) {
            memcpy(lease->server, option, sizeof(lease->server));
            lease->has_server = true;
            print_ipv4("DHCP server: ", lease->server);
            printf("\n");
        }
        else if (code == 1 && length == 4) {
            memcpy(lease->subnet_mask, option, sizeof(lease->subnet_mask));
            lease->has_subnet_mask = true;
            print_ipv4("DHCP subnet mask: ", lease->subnet_mask);
            printf("\n");
        }
        else if (code == 3 && length >= 4) {
            memcpy(lease->router, option, sizeof(lease->router));
            lease->has_router = true;
            print_ipv4("DHCP router: ", lease->router);
            printf("\n");
        }
        else if (code == 6 && length >= 4) {
            memcpy(lease->dns, option, sizeof(lease->dns));
            lease->has_dns = true;
            print_ipv4("DHCP DNS: ", lease->dns);
            printf("\n");
        }
        else if (code == 51 && length == 4) {
            lease->lease_seconds = ((u32)option[0] << 24) | ((u32)option[1] << 16) |
                                  ((u32)option[2] << 8) | option[3];
            printf("DHCP lease time: %u seconds\n", lease->lease_seconds);
        }
        option += length;
    }
    return message_type;
}

static bool inspect_arp_reply(const u8 *packet, u32 packet_size, const DhcpLease *lease, u8 gateway_mac[6]) {
    if (packet_size < RNDIS_PACKET_HEADER_SIZE) return false;
    u32 data_offset = 8 + get_le32(packet + 8);
    u32 data_length = get_le32(packet + 12);
    if (get_le32(packet) != RNDIS_PACKET_MSG || data_offset > packet_size ||
        data_length > packet_size - data_offset || data_length < 14 + 28) return false;
    const u8 *ethernet = packet + data_offset;
    if (get_be16(ethernet + 12) != 0x0806) return false;
    const u8 *arp = ethernet + 14;
    if (get_be16(arp) != 1 || get_be16(arp + 2) != 0x0800 || arp[4] != 6 || arp[5] != 4 ||
        get_be16(arp + 6) != 2 || memcmp(arp + 14, lease->router, 4) != 0 ||
        memcmp(arp + 24, lease->address, 4) != 0) return false;
    memcpy(gateway_mac, arp + 8, 6);
    printf("ARP reply: gateway MAC %02X:%02X:%02X:%02X:%02X:%02X\n",
           gateway_mac[0], gateway_mac[1], gateway_mac[2], gateway_mac[3], gateway_mac[4], gateway_mac[5]);
    return true;
}

static bool inspect_icmp_echo_reply(const u8 *packet, u32 packet_size, const DhcpLease *lease) {
    if (packet_size < RNDIS_PACKET_HEADER_SIZE) return false;
    u32 data_offset = 8 + get_le32(packet + 8);
    u32 data_length = get_le32(packet + 12);
    if (get_le32(packet) != RNDIS_PACKET_MSG || data_offset > packet_size ||
        data_length > packet_size - data_offset || data_length < 14 + 20 + 8) return false;
    const u8 *ethernet = packet + data_offset;
    if (get_be16(ethernet + 12) != 0x0800) return false;
    const u8 *ipv4 = ethernet + 14;
    u8 ihl = (ipv4[0] & 0x0f) * 4;
    if (ihl < 20 || data_length < (u32)(14 + ihl + 8) || ipv4[9] != 1 ||
        memcmp(ipv4 + 12, lease->router, 4) != 0 || memcmp(ipv4 + 16, lease->address, 4) != 0) return false;
    const u8 *icmp = ipv4 + ihl;
    if (icmp[0] != 0 || icmp[1] != 0 || get_be16(icmp + 4) != 0x4e58 || get_be16(icmp + 6) != 1) return false;
    printf("ICMP echo reply received from DHCP router.\n");
    return true;
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
    RndisSetPacketFilterMessage enable_filter = {
        .message_type = RNDIS_SET_MSG,
        .message_length = sizeof(RndisSetPacketFilterMessage),
        .request_id = 3,
        .oid = RNDIS_OID_GEN_CURRENT_PACKET_FILTER,
        .information_buffer_length = sizeof(u32),
        .information_buffer_offset = 20,
        .device_vc_handle = 0,
        .packet_filter = RNDIS_DEFAULT_PACKET_FILTER,
    };
    s32 count = 0;
    s32 available_count = 0;
    int control_index = -1;
    Result rc;
    u32 transferred = 0;
    void *buffer = NULL;
    bool packet_filter_enabled = false;

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
            printf("RNDIS MAC query validated; enabling packet filter briefly.\n");
        } else {
            printf("RNDIS MAC response was invalid: type=0x%08X len=%u status=0x%08X data=%u@%u\n",
                   response.message_type, response.message_length, response.status,
                   response.information_buffer_length, response.information_buffer_offset);
        }
    } else if (R_SUCCEEDED(rc)) {
        printf("RNDIS MAC response was too short (%u bytes).\n", transferred);
    }

    if (R_FAILED(rc) || transferred < sizeof(RndisQueryComplete)) goto cleanup;
    RndisQueryComplete mac_response;
    memcpy(&mac_response, buffer, sizeof(mac_response));
    if (mac_response.message_type != RNDIS_QUERY_CMPLT ||
        mac_response.request_id != mac_query.request_id ||
        mac_response.status != RNDIS_STATUS_SUCCESS)
        goto cleanup;

    memcpy(buffer, &enable_filter, sizeof(enable_filter));
    transferred = 0;
    rc = usbHsIfCtrlXfer(&session,
                         USB_ENDPOINT_OUT | USB_REQUEST_TYPE_CLASS | USB_RECIPIENT_INTERFACE,
                         0, 0, control_desc.bInterfaceNumber,
                         sizeof(enable_filter), buffer, &transferred);
    log_transfer_result("RNDIS SET PACKET_FILTER", rc, transferred);
    if (R_FAILED(rc) || transferred != sizeof(enable_filter)) goto cleanup;

    memset(buffer, 0, 0x1000);
    transferred = 0;
    rc = usbHsIfCtrlXfer(&session,
                         USB_ENDPOINT_IN | USB_REQUEST_TYPE_CLASS | USB_RECIPIENT_INTERFACE,
                         1, 0, control_desc.bInterfaceNumber,
                         0x1000, buffer, &transferred);
    log_transfer_result("RNDIS GET FILTER RESPONSE", rc, transferred);
    if (R_FAILED(rc) || transferred < sizeof(RndisSetComplete)) goto cleanup;
    RndisSetComplete filter_response;
    memcpy(&filter_response, buffer, sizeof(filter_response));
    if (filter_response.message_type != RNDIS_SET_CMPLT ||
        filter_response.request_id != enable_filter.request_id ||
        filter_response.status != RNDIS_STATUS_SUCCESS) {
        printf("RNDIS filter enable was not accepted.\n");
        goto cleanup;
    }
    packet_filter_enabled = true;
    printf("RNDIS packet filter enabled (0x%08X); disabling it during cleanup.\n",
           enable_filter.packet_filter);

cleanup:
    if (buffer != NULL) {
        if (packet_filter_enabled) {
            RndisSetPacketFilterMessage disable_filter = enable_filter;
            disable_filter.request_id = 4;
            disable_filter.packet_filter = 0;
            memcpy(buffer, &disable_filter, sizeof(disable_filter));
            transferred = 0;
            rc = usbHsIfCtrlXfer(&session,
                                 USB_ENDPOINT_OUT | USB_REQUEST_TYPE_CLASS | USB_RECIPIENT_INTERFACE,
                                 0, 0, control_desc.bInterfaceNumber,
                                 sizeof(disable_filter), buffer, &transferred);
            log_transfer_result("RNDIS CLEAR PACKET_FILTER", rc, transferred);
            if (R_SUCCEEDED(rc) && transferred == sizeof(disable_filter)) {
                memset(buffer, 0, 0x1000);
                transferred = 0;
                rc = usbHsIfCtrlXfer(&session,
                                     USB_ENDPOINT_IN | USB_REQUEST_TYPE_CLASS | USB_RECIPIENT_INTERFACE,
                                     1, 0, control_desc.bInterfaceNumber,
                                     0x1000, buffer, &transferred);
                log_transfer_result("RNDIS GET CLEAR RESPONSE", rc, transferred);
            }
        }
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

static bool is_bulk_endpoint(const struct usb_endpoint_descriptor *desc) {
    return desc->bLength != 0 &&
           (desc->bmAttributes & USB_TRANSFER_TYPE_MASK) == USB_TRANSFER_TYPE_BULK;
}

static void probe_rndis_bulk_endpoints(void) {
    UsbHsInterfaceFilter discovery_filter;
    UsbHsInterfaceFilter data_filter;
    UsbHsInterface interfaces[MAX_INTERFACES];
    UsbHsInterface available[MAX_INTERFACES];
    UsbHsClientIfSession session;
    UsbHsClientEpSession input_endpoint;
    UsbHsClientEpSession output_endpoint;
    struct usb_device_descriptor device_desc;
    struct usb_interface_descriptor data_desc;
    struct usb_endpoint_descriptor input_desc;
    struct usb_endpoint_descriptor output_desc;
    s32 count = 0;
    s32 available_count = 0;
    int data_index = -1;
    Result rc;
    bool input_open = false;
    bool output_open = false;

    memset(&discovery_filter, 0, sizeof(discovery_filter));
    memset(&data_filter, 0, sizeof(data_filter));
    memset(interfaces, 0, sizeof(interfaces));
    memset(available, 0, sizeof(available));
    memset(&session, 0, sizeof(session));
    memset(&input_endpoint, 0, sizeof(input_endpoint));
    memset(&output_endpoint, 0, sizeof(output_endpoint));
    memset(&input_desc, 0, sizeof(input_desc));
    memset(&output_desc, 0, sizeof(output_desc));

    rc = usbHsQueryAllInterfaces(&discovery_filter, interfaces, sizeof(interfaces), &count);
    log_result("usbHsQueryAllInterfaces", rc);
    if (R_FAILED(rc) || count <= 0) goto unavailable;

    for (int i = 0; i < count && i < MAX_INTERFACES; i++) {
        if (is_rndis_data(&interfaces[i])) {
            data_index = i;
            break;
        }
    }
    if (data_index < 0) goto unavailable;

    memcpy(&device_desc, &interfaces[data_index].device_desc, sizeof(device_desc));
    memcpy(&data_desc, &interfaces[data_index].inf.interface_desc, sizeof(data_desc));
    data_filter.Flags = UsbHsInterfaceFilterFlags_idVendor |
                        UsbHsInterfaceFilterFlags_idProduct |
                        UsbHsInterfaceFilterFlags_bInterfaceClass |
                        UsbHsInterfaceFilterFlags_bInterfaceSubClass |
                        UsbHsInterfaceFilterFlags_bInterfaceProtocol;
    data_filter.idVendor = device_desc.idVendor;
    data_filter.idProduct = device_desc.idProduct;
    data_filter.bInterfaceClass = data_desc.bInterfaceClass;
    data_filter.bInterfaceSubClass = data_desc.bInterfaceSubClass;
    data_filter.bInterfaceProtocol = data_desc.bInterfaceProtocol;
    rc = usbHsQueryAvailableInterfaces(&data_filter, available, sizeof(available), &available_count);
    log_result("usbHsQueryAvailableDataIf", rc);
    if (R_FAILED(rc) || available_count <= 0) goto unavailable;

    data_index = -1;
    for (int i = 0; i < available_count && i < MAX_INTERFACES; i++) {
        if (is_rndis_data(&available[i])) {
            data_index = i;
            break;
        }
    }
    if (data_index < 0) goto unavailable;

    for (int i = 0; i < 15; i++) {
        struct usb_endpoint_descriptor candidate;
        memcpy(&candidate, &available[data_index].inf.input_endpoint_descs[i], sizeof(candidate));
        if (is_bulk_endpoint(&candidate)) {
            input_desc = candidate;
            break;
        }
    }
    for (int i = 0; i < 15; i++) {
        struct usb_endpoint_descriptor candidate;
        memcpy(&candidate, &available[data_index].inf.output_endpoint_descs[i], sizeof(candidate));
        if (is_bulk_endpoint(&candidate)) {
            output_desc = candidate;
            break;
        }
    }
    if (input_desc.bLength == 0 || output_desc.bLength == 0) {
        printf("RNDIS data interface lacks the required bulk IN/OUT endpoints.\n");
        print_recent_results();
        return;
    }

    printf("Acquiring RNDIS data interface %u (ID %d).\n",
           available[data_index].inf.interface_desc.bInterfaceNumber,
           available[data_index].inf.ID);
    rc = usbHsAcquireUsbIf(&session, &available[data_index]);
    log_result("usbHsAcquireDataIf", rc);
    if (R_FAILED(rc)) {
        print_recent_results();
        return;
    }

    rc = usbHsIfOpenUsbEp(&session, &input_endpoint, 1, 0x1000, &input_desc);
    log_result("usbHsIfOpenUsbEp(IN)", rc);
    if (R_FAILED(rc)) goto cleanup;
    input_open = true;

    rc = usbHsIfOpenUsbEp(&session, &output_endpoint, 1, 0x1000, &output_desc);
    log_result("usbHsIfOpenUsbEp(OUT)", rc);
    if (R_FAILED(rc)) goto cleanup;
    output_open = true;

    printf("Bulk endpoints opened: IN 0x%02X, OUT 0x%02X.\n",
           input_desc.bEndpointAddress, output_desc.bEndpointAddress);
    printf("No packets were sent and no RNDIS packet filter was enabled.\n");

cleanup:
    if (output_open) usbHsEpClose(&output_endpoint);
    if (input_open) usbHsEpClose(&input_endpoint);
    usbHsIfClose(&session);
    print_recent_results();
    return;

unavailable:
    printf("RNDIS data interface is not available to this homebrew.\n");
    print_recent_results();
}

static void probe_dhcp_discover(void) {
    UsbHsInterfaceFilter no_filter, control_filter, data_filter;
    UsbHsInterface interfaces[MAX_INTERFACES], available[MAX_INTERFACES];
    UsbHsClientIfSession control_session, data_session;
    UsbHsClientEpSession input_endpoint, output_endpoint;
    struct usb_interface_descriptor control_desc, data_desc;
    struct usb_device_descriptor device_desc;
    struct usb_endpoint_descriptor input_desc = {0}, output_desc = {0};
    RndisInitializeMessage init = { RNDIS_INITIALIZE_MSG, sizeof(RndisInitializeMessage), 10, 1, 0, 0x4000 };
    RndisQueryMessage mac_query = { RNDIS_QUERY_MSG, sizeof(RndisQueryMessage), 11,
                                    RNDIS_OID_802_3_CURRENT_ADDRESS, 0, 20, 0 };
    RndisSetPacketFilterMessage set_filter = { RNDIS_SET_MSG, sizeof(RndisSetPacketFilterMessage), 12,
                                                RNDIS_OID_GEN_CURRENT_PACKET_FILTER, sizeof(u32), 20, 0,
                                                RNDIS_DEFAULT_PACKET_FILTER };
    RndisHaltMessage halt = { RNDIS_HALT_MSG, sizeof(RndisHaltMessage), 10 };
    s32 count = 0, available_count = 0;
    int index = -1;
    Result rc;
    u32 transferred = 0, xfer_id = 0, report_count = 0;
    void *control_buffer = NULL, *rx_buffer = NULL, *tx_buffer = NULL;
    bool control_active = false, data_active = false, input_open = false, output_open = false;
    bool filter_enabled = false;
    u8 mac[6];
    DhcpLease lease = {0};

    memset(&no_filter, 0, sizeof(no_filter));
    memset(&control_filter, 0, sizeof(control_filter));
    memset(&data_filter, 0, sizeof(data_filter));
    memset(interfaces, 0, sizeof(interfaces));
    memset(available, 0, sizeof(available));
    memset(&control_session, 0, sizeof(control_session));
    memset(&data_session, 0, sizeof(data_session));
    memset(&input_endpoint, 0, sizeof(input_endpoint));
    memset(&output_endpoint, 0, sizeof(output_endpoint));

    rc = usbHsQueryAllInterfaces(&no_filter, interfaces, sizeof(interfaces), &count);
    log_result("DHCP query interfaces", rc);
    if (R_FAILED(rc)) goto cleanup;
    for (int i = 0; i < count && i < MAX_INTERFACES; i++) {
        if (is_rndis_control(&interfaces[i])) { index = i; break; }
    }
    if (index < 0) { printf("No RNDIS control interface found.\n"); goto cleanup; }
    memcpy(&control_desc, &interfaces[index].inf.interface_desc, sizeof(control_desc));
    memcpy(&device_desc, &interfaces[index].device_desc, sizeof(device_desc));
    control_filter.Flags = UsbHsInterfaceFilterFlags_idVendor | UsbHsInterfaceFilterFlags_idProduct |
                           UsbHsInterfaceFilterFlags_bInterfaceClass | UsbHsInterfaceFilterFlags_bInterfaceSubClass |
                           UsbHsInterfaceFilterFlags_bInterfaceProtocol;
    control_filter.idVendor = device_desc.idVendor;
    control_filter.idProduct = device_desc.idProduct;
    control_filter.bInterfaceClass = control_desc.bInterfaceClass;
    control_filter.bInterfaceSubClass = control_desc.bInterfaceSubClass;
    control_filter.bInterfaceProtocol = control_desc.bInterfaceProtocol;
    rc = usbHsQueryAvailableInterfaces(&control_filter, available, sizeof(available), &available_count);
    log_result("DHCP query control", rc);
    if (R_FAILED(rc) || available_count <= 0) goto cleanup;
    rc = usbHsAcquireUsbIf(&control_session, &available[0]);
    log_result("DHCP acquire control", rc);
    if (R_FAILED(rc)) goto cleanup;
    control_active = true;

    control_buffer = memalign(0x1000, 0x1000);
    rx_buffer = memalign(0x1000, 0x1000);
    tx_buffer = memalign(0x1000, 0x1000);
    if (!control_buffer || !rx_buffer || !tx_buffer) {
        log_result("DHCP aligned allocation", MAKERESULT(Module_Libnx, LibnxError_OutOfMemory));
        goto cleanup;
    }
    memcpy(control_buffer, &init, sizeof(init));
    rc = usbHsIfCtrlXfer(&control_session, USB_ENDPOINT_OUT | USB_REQUEST_TYPE_CLASS | USB_RECIPIENT_INTERFACE,
                         0, 0, control_desc.bInterfaceNumber, sizeof(init), control_buffer, &transferred);
    log_transfer_result("DHCP RNDIS INIT", rc, transferred);
    if (R_FAILED(rc) || transferred != sizeof(init)) goto cleanup;
    memset(control_buffer, 0, 0x1000); transferred = 0;
    rc = usbHsIfCtrlXfer(&control_session, USB_ENDPOINT_IN | USB_REQUEST_TYPE_CLASS | USB_RECIPIENT_INTERFACE,
                         1, 0, control_desc.bInterfaceNumber, 0x1000, control_buffer, &transferred);
    log_transfer_result("DHCP GET INIT RESPONSE", rc, transferred);
    if (R_FAILED(rc) || transferred < sizeof(RndisInitializeCompletePrefix)) goto cleanup;
    RndisInitializeCompletePrefix init_response;
    memcpy(&init_response, control_buffer, sizeof(init_response));
    if (init_response.message_type != RNDIS_INITIALIZE_CMPLT || init_response.status != RNDIS_STATUS_SUCCESS) goto cleanup;

    memcpy(control_buffer, &mac_query, sizeof(mac_query)); transferred = 0;
    rc = usbHsIfCtrlXfer(&control_session, USB_ENDPOINT_OUT | USB_REQUEST_TYPE_CLASS | USB_RECIPIENT_INTERFACE,
                         0, 0, control_desc.bInterfaceNumber, sizeof(mac_query), control_buffer, &transferred);
    log_transfer_result("DHCP QUERY MAC", rc, transferred);
    if (R_FAILED(rc) || transferred != sizeof(mac_query)) goto cleanup;
    memset(control_buffer, 0, 0x1000); transferred = 0;
    rc = usbHsIfCtrlXfer(&control_session, USB_ENDPOINT_IN | USB_REQUEST_TYPE_CLASS | USB_RECIPIENT_INTERFACE,
                         1, 0, control_desc.bInterfaceNumber, 0x1000, control_buffer, &transferred);
    log_transfer_result("DHCP GET MAC", rc, transferred);
    if (R_FAILED(rc) || transferred < sizeof(RndisQueryComplete)) goto cleanup;
    RndisQueryComplete mac_response;
    memcpy(&mac_response, control_buffer, sizeof(mac_response));
    size_t mac_offset = 8 + mac_response.information_buffer_offset;
    if (mac_response.message_type != RNDIS_QUERY_CMPLT || mac_response.status != RNDIS_STATUS_SUCCESS ||
        mac_response.information_buffer_length != 6 || mac_offset > transferred || 6 > transferred - mac_offset) goto cleanup;
    memcpy(mac, (u8 *)control_buffer + mac_offset, sizeof(mac));
    printf("DHCP using RNDIS MAC %02X:%02X:%02X:%02X:%02X:%02X\n", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    memcpy(control_buffer, &set_filter, sizeof(set_filter)); transferred = 0;
    rc = usbHsIfCtrlXfer(&control_session, USB_ENDPOINT_OUT | USB_REQUEST_TYPE_CLASS | USB_RECIPIENT_INTERFACE,
                         0, 0, control_desc.bInterfaceNumber, sizeof(set_filter), control_buffer, &transferred);
    log_transfer_result("DHCP SET FILTER", rc, transferred);
    if (R_FAILED(rc) || transferred != sizeof(set_filter)) goto cleanup;
    memset(control_buffer, 0, 0x1000); transferred = 0;
    rc = usbHsIfCtrlXfer(&control_session, USB_ENDPOINT_IN | USB_REQUEST_TYPE_CLASS | USB_RECIPIENT_INTERFACE,
                         1, 0, control_desc.bInterfaceNumber, 0x1000, control_buffer, &transferred);
    log_transfer_result("DHCP GET FILTER RESPONSE", rc, transferred);
    if (R_FAILED(rc) || transferred < sizeof(RndisSetComplete)) goto cleanup;
    RndisSetComplete filter_response;
    memcpy(&filter_response, control_buffer, sizeof(filter_response));
    if (filter_response.message_type != RNDIS_SET_CMPLT || filter_response.status != RNDIS_STATUS_SUCCESS) goto cleanup;
    filter_enabled = true;

    memset(interfaces, 0, sizeof(interfaces)); count = 0; index = -1;
    rc = usbHsQueryAllInterfaces(&no_filter, interfaces, sizeof(interfaces), &count);
    log_result("DHCP query data", rc);
    if (R_FAILED(rc)) goto cleanup;
    for (int i = 0; i < count && i < MAX_INTERFACES; i++) {
        if (is_rndis_data(&interfaces[i])) { index = i; break; }
    }
    if (index < 0) goto cleanup;
    memcpy(&data_desc, &interfaces[index].inf.interface_desc, sizeof(data_desc));
    data_filter.Flags = UsbHsInterfaceFilterFlags_idVendor | UsbHsInterfaceFilterFlags_idProduct |
                       UsbHsInterfaceFilterFlags_bInterfaceClass | UsbHsInterfaceFilterFlags_bInterfaceSubClass |
                       UsbHsInterfaceFilterFlags_bInterfaceProtocol;
    data_filter.idVendor = device_desc.idVendor; data_filter.idProduct = device_desc.idProduct;
    data_filter.bInterfaceClass = data_desc.bInterfaceClass;
    data_filter.bInterfaceSubClass = data_desc.bInterfaceSubClass;
    data_filter.bInterfaceProtocol = data_desc.bInterfaceProtocol;
    memset(available, 0, sizeof(available)); available_count = 0;
    rc = usbHsQueryAvailableInterfaces(&data_filter, available, sizeof(available), &available_count);
    log_result("DHCP query available data", rc);
    if (R_FAILED(rc) || available_count <= 0) goto cleanup;
    rc = usbHsAcquireUsbIf(&data_session, &available[0]);
    log_result("DHCP acquire data", rc);
    if (R_FAILED(rc)) goto cleanup;
    data_active = true;
    for (int i = 0; i < 15; i++) {
        struct usb_endpoint_descriptor candidate;
        memcpy(&candidate, &available[0].inf.input_endpoint_descs[i], sizeof(candidate));
        if (is_bulk_endpoint(&candidate)) { input_desc = candidate; break; }
    }
    for (int i = 0; i < 15; i++) {
        struct usb_endpoint_descriptor candidate;
        memcpy(&candidate, &available[0].inf.output_endpoint_descs[i], sizeof(candidate));
        if (is_bulk_endpoint(&candidate)) { output_desc = candidate; break; }
    }
    if (!input_desc.bLength || !output_desc.bLength) goto cleanup;
    rc = usbHsIfOpenUsbEp(&data_session, &input_endpoint, 1, 0x1000, &input_desc);
    log_result("DHCP open IN", rc); if (R_FAILED(rc)) goto cleanup; input_open = true;
    rc = usbHsIfOpenUsbEp(&data_session, &output_endpoint, 1, 0x1000, &output_desc);
    log_result("DHCP open OUT", rc); if (R_FAILED(rc)) goto cleanup; output_open = true;

    rc = usbHsEpPostBufferAsync(&input_endpoint, rx_buffer, 0x1000, 1, &xfer_id);
    log_result("DHCP post receive", rc); if (R_FAILED(rc)) goto cleanup;
    size_t discover_size = build_dhcp_discover(tx_buffer, mac);
    transferred = 0;
    rc = usbHsEpPostBuffer(&output_endpoint, tx_buffer, discover_size, &transferred);
    log_transfer_result("DHCPDISCOVER bulk send", rc, transferred);
    if (R_FAILED(rc) || transferred != discover_size) goto cleanup;
    printf("DHCPDISCOVER sent; waiting up to 12 seconds for DHCPOFFER.\n");
    bool got_offer = false;
    for (int attempt = 0; attempt < 6; attempt++) {
        rc = eventWait(usbHsEpGetXferEvent(&input_endpoint), 2000000000ULL);
        log_result("DHCP receive wait", rc);
        if (R_FAILED(rc)) continue;

        UsbHsXferReport reports[2];
        eventClear(usbHsEpGetXferEvent(&input_endpoint));
        rc = usbHsEpGetXferReport(&input_endpoint, reports, 2, &report_count);
        log_result("DHCP receive report", rc);
        if (R_FAILED(rc) || report_count == 0) continue;

        printf("Bulk receive: result=0x%08X bytes=%u\n", reports[0].res, reports[0].transferredSize);
        if (R_FAILED(reports[0].res)) break;
        u8 message_type = 0;
        if (reports[0].transferredSize != 0)
            message_type = inspect_dhcp_reply(rx_buffer, reports[0].transferredSize, &lease);
        else
            printf("Ignoring zero-length bulk completion; reposting receive.\n");
        if (message_type == 2 && lease.has_server) {
            got_offer = true;
            printf("DHCPOFFER retained; requesting the offered lease.\n");
            break;
        }

        if (attempt == 1 || attempt == 3) {
            transferred = 0;
            rc = usbHsEpPostBuffer(&output_endpoint, tx_buffer, discover_size, &transferred);
            log_transfer_result("DHCPDISCOVER retry send", rc, transferred);
            if (R_FAILED(rc) || transferred != discover_size) goto cleanup;
            printf("Retrying DHCPDISCOVER after unrelated traffic.\n");
        }
        printf("Reposting receive for the next frame.\n");
        rc = usbHsEpPostBufferAsync(&input_endpoint, rx_buffer, 0x1000, (u64)(attempt + 2), &xfer_id);
        log_result("DHCP repost receive", rc);
        if (R_FAILED(rc)) break;
        if (attempt == 5) printf("No DHCPOFFER before timeout.\n");
    }
    if (!got_offer) goto cleanup;

    rc = usbHsEpPostBufferAsync(&input_endpoint, rx_buffer, 0x1000, 16, &xfer_id);
    log_result("DHCP post ACK receive", rc); if (R_FAILED(rc)) goto cleanup;
    size_t request_size = build_dhcp_request(tx_buffer, mac, &lease);
    transferred = 0;
    rc = usbHsEpPostBuffer(&output_endpoint, tx_buffer, request_size, &transferred);
    log_transfer_result("DHCPREQUEST bulk send", rc, transferred);
    if (R_FAILED(rc) || transferred != request_size) goto cleanup;
    printf("DHCPREQUEST sent; waiting up to 8 seconds for DHCPACK.\n");
    bool got_ack = false;
    for (int attempt = 0; attempt < 4; attempt++) {
        rc = eventWait(usbHsEpGetXferEvent(&input_endpoint), 2000000000ULL);
        log_result("DHCP ACK receive wait", rc);
        if (R_FAILED(rc)) continue;

        UsbHsXferReport reports[2];
        eventClear(usbHsEpGetXferEvent(&input_endpoint));
        report_count = 0;
        rc = usbHsEpGetXferReport(&input_endpoint, reports, 2, &report_count);
        log_result("DHCP ACK receive report", rc);
        if (R_FAILED(rc) || report_count == 0) continue;

        printf("ACK bulk receive: result=0x%08X bytes=%u\n", reports[0].res, reports[0].transferredSize);
        if (R_FAILED(reports[0].res)) break;
        u8 message_type = 0;
        if (reports[0].transferredSize != 0)
            message_type = inspect_dhcp_reply(rx_buffer, reports[0].transferredSize, &lease);
        else
            printf("Ignoring zero-length bulk completion; reposting receive.\n");
        if (message_type == 5) {
            got_ack = true;
            print_ipv4("DHCPACK accepted for validation only: ", lease.address);
            printf("\nNo Switch IP, route, or DNS configuration was applied.\n");
            break;
        }
        if (message_type == 6) {
            printf("DHCPNAK received; no lease was accepted.\n");
            break;
        }

        printf("Reposting receive for the next DHCPACK frame.\n");
        rc = usbHsEpPostBufferAsync(&input_endpoint, rx_buffer, 0x1000, (u64)(17 + attempt), &xfer_id);
        log_result("DHCP repost ACK receive", rc);
        if (R_FAILED(rc)) break;
        if (attempt == 3) printf("No DHCPACK before timeout.\n");
    }
    if (!got_ack) printf("Lease was not accepted by this diagnostic.\n");
    if (!got_ack || !lease.has_router) {
        if (got_ack) printf("DHCPACK did not contain a router option; ARP probe skipped.\n");
        goto cleanup;
    }

    u8 gateway_mac[6] = {0};
    rc = usbHsEpPostBufferAsync(&input_endpoint, rx_buffer, 0x1000, 32, &xfer_id);
    log_result("ARP post receive", rc); if (R_FAILED(rc)) goto cleanup;
    size_t arp_size = build_arp_request(tx_buffer, mac, &lease);
    transferred = 0;
    rc = usbHsEpPostBuffer(&output_endpoint, tx_buffer, arp_size, &transferred);
    log_transfer_result("ARP gateway request", rc, transferred);
    if (R_FAILED(rc) || transferred != arp_size) goto cleanup;
    print_ipv4("ARP request sent for DHCP router ", lease.router);
    printf("; waiting up to 4 seconds for its MAC.\n");
    bool got_arp = false;
    for (int attempt = 0; attempt < 2; attempt++) {
        rc = eventWait(usbHsEpGetXferEvent(&input_endpoint), 2000000000ULL);
        log_result("ARP receive wait", rc);
        if (R_FAILED(rc)) continue;

        UsbHsXferReport reports[2];
        eventClear(usbHsEpGetXferEvent(&input_endpoint));
        report_count = 0;
        rc = usbHsEpGetXferReport(&input_endpoint, reports, 2, &report_count);
        log_result("ARP receive report", rc);
        if (R_FAILED(rc) || report_count == 0) continue;
        printf("ARP bulk receive: result=0x%08X bytes=%u\n", reports[0].res, reports[0].transferredSize);
        if (R_FAILED(reports[0].res)) break;
        if (inspect_arp_reply(rx_buffer, reports[0].transferredSize, &lease, gateway_mac)) {
            got_arp = true;
            break;
        }
        printf("Frame was not the requested ARP reply; reposting receive.\n");
        rc = usbHsEpPostBufferAsync(&input_endpoint, rx_buffer, 0x1000, (u64)(33 + attempt), &xfer_id);
        log_result("ARP repost receive", rc);
        if (R_FAILED(rc)) break;
    }
    if (!got_arp) printf("No matching gateway ARP reply before timeout.\n");
    if (!got_arp) goto cleanup;

    rc = usbHsEpPostBufferAsync(&input_endpoint, rx_buffer, 0x1000, 48, &xfer_id);
    log_result("ICMP post receive", rc); if (R_FAILED(rc)) goto cleanup;
    size_t icmp_size = build_icmp_echo_request(tx_buffer, mac, &lease, gateway_mac);
    transferred = 0;
    rc = usbHsEpPostBuffer(&output_endpoint, tx_buffer, icmp_size, &transferred);
    log_transfer_result("ICMP gateway echo send", rc, transferred);
    if (R_FAILED(rc) || transferred != icmp_size) goto cleanup;
    print_ipv4("ICMP echo sent to DHCP router ", lease.router);
    printf("; waiting up to 4 seconds for reply.\n");
    bool got_echo = false;
    for (int attempt = 0; attempt < 2; attempt++) {
        rc = eventWait(usbHsEpGetXferEvent(&input_endpoint), 2000000000ULL);
        log_result("ICMP receive wait", rc);
        if (R_FAILED(rc)) continue;

        UsbHsXferReport reports[2];
        eventClear(usbHsEpGetXferEvent(&input_endpoint));
        report_count = 0;
        rc = usbHsEpGetXferReport(&input_endpoint, reports, 2, &report_count);
        log_result("ICMP receive report", rc);
        if (R_FAILED(rc) || report_count == 0) continue;
        printf("ICMP bulk receive: result=0x%08X bytes=%u\n", reports[0].res, reports[0].transferredSize);
        if (R_FAILED(reports[0].res)) break;
        if (inspect_icmp_echo_reply(rx_buffer, reports[0].transferredSize, &lease)) {
            got_echo = true;
            break;
        }
        printf("Frame was not the requested ICMP echo reply; reposting receive.\n");
        rc = usbHsEpPostBufferAsync(&input_endpoint, rx_buffer, 0x1000, (u64)(49 + attempt), &xfer_id);
        log_result("ICMP repost receive", rc);
        if (R_FAILED(rc)) break;
    }
    if (!got_echo) printf("No matching ICMP echo reply before timeout.\n");

cleanup:
    if (output_open) usbHsEpClose(&output_endpoint);
    if (input_open) usbHsEpClose(&input_endpoint);
    if (data_active) usbHsIfClose(&data_session);
    if (control_active && control_buffer) {
        if (filter_enabled) {
            set_filter.request_id = 13; set_filter.packet_filter = 0;
            memcpy(control_buffer, &set_filter, sizeof(set_filter)); transferred = 0;
            rc = usbHsIfCtrlXfer(&control_session, USB_ENDPOINT_OUT | USB_REQUEST_TYPE_CLASS | USB_RECIPIENT_INTERFACE,
                                 0, 0, control_desc.bInterfaceNumber, sizeof(set_filter), control_buffer, &transferred);
            log_transfer_result("DHCP CLEAR FILTER", rc, transferred);
            if (R_SUCCEEDED(rc) && transferred == sizeof(set_filter)) {
                memset(control_buffer, 0, 0x1000); transferred = 0;
                rc = usbHsIfCtrlXfer(&control_session, USB_ENDPOINT_IN | USB_REQUEST_TYPE_CLASS | USB_RECIPIENT_INTERFACE,
                                     1, 0, control_desc.bInterfaceNumber, 0x1000, control_buffer, &transferred);
                log_transfer_result("DHCP GET CLEAR RESPONSE", rc, transferred);
            }
        }
        memcpy(control_buffer, &halt, sizeof(halt)); transferred = 0;
        rc = usbHsIfCtrlXfer(&control_session, USB_ENDPOINT_OUT | USB_REQUEST_TYPE_CLASS | USB_RECIPIENT_INTERFACE,
                             0, 0, control_desc.bInterfaceNumber, sizeof(halt), control_buffer, &transferred);
        log_transfer_result("DHCP RNDIS HALT", rc, transferred);
    }
    free(tx_buffer); free(rx_buffer); free(control_buffer);
    if (control_active) usbHsIfClose(&control_session);
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
        if (pressed & HidNpadButton_Y) {
            draw_header();
            if (R_SUCCEEDED(usb_rc))
                probe_rndis_bulk_endpoints();
            else {
                printf("usb:hs is unavailable; no bulk endpoint probe was attempted.\n");
                print_recent_results();
            }
        }
        if (pressed & HidNpadButton_R) {
            draw_header();
            if (R_SUCCEEDED(usb_rc))
                probe_dhcp_discover();
            else {
                printf("usb:hs is unavailable; no DHCP probe was attempted.\n");
                print_recent_results();
            }
        }
        consoleUpdate(NULL);
    }

    if (R_SUCCEEDED(usb_rc)) usbHsExit();
    consoleExit(NULL);
    return 0;
}
