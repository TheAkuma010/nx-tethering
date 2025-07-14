#include <switch.h>
#include <stdio.h>
#include "usbnet.h"
#include "log.h"

void usbnet_init_usb(void) {
    log_print("usbnet: inicializando USB (usbhsfs)...\n");
    usbHsFsInitialize();
}

void usbnet_exit_usb(void) {
    usbHsFsExit();
    log_print("usbnet: finalizando USB (usbhsfs).\n");
}

void usbnet_poll_usb_devices(void) {
    UsbHsFsClient client;
    Result rc = usbHsFsCreateClient(&client);
    if (R_FAILED(rc)) {
        log_print("Erro ao criar cliente usbHsFs: 0x%x\n", rc);
        return;
    }

    s32 device_count = 0;
    UsbHsFsDeviceInfo devices[8];
    rc = usbHsFsQueryAvailableInterfaces(&client, devices, 8, &device_count);
    if (R_FAILED(rc)) {
        log_print("Erro ao enumerar interfaces USB: 0x%x\n", rc);
        usbHsFsDestroyClient(&client);
        return;
    }

    log_print("Dispositivos USB detectados: %d\n", device_count);

    for (int i = 0; i < device_count; i++) {
        UsbHsFsDeviceInfo* dev = &devices[i];

        log_print(
            "Dispositivo %d: VID: %04X PID: %04X | class: 0x%02X, subclass: 0x%02X, proto: 0x%02X\n",
            i,
            dev->descriptor.idVendor,
            dev->descriptor.idProduct,
            dev->descriptor.bDeviceClass,
            dev->descriptor.bDeviceSubClass,
            dev->descriptor.bDeviceProtocol
        );

        for (int j = 0; j < dev->interface_count; j++) {
            UsbHsFsInterfaceDescriptor desc = dev->interfaces[j];

            log_print(
                "  Interface %d: class: 0x%02X, subclass: 0x%02X, protocol: 0x%02X\n",
                j,
                desc.bInterfaceClass,
                desc.bInterfaceSubClass,
                desc.bInterfaceProtocol
            );

            // Detecta possível CDC ECM
            if (desc.bInterfaceClass == 0x02 && desc.bInterfaceSubClass == 0x06) {
                log_print("  ↳ Interface CDC ECM detectada (tethering Linux).\n");
            }

            // Detecta possível RNDIS (Vendor-specific)
            if (desc.bInterfaceClass == 0xFF && desc.bInterfaceSubClass == 0xFF) {
                log_print("  ↳ Interface RNDIS possível (tethering Windows).\n");
            }

            // Detecta NCM
            if (desc.bInterfaceClass == 0x02 && desc.bInterfaceSubClass == 0x0D) {
                log_print("  ↳ Interface NCM detectada.\n");
            }
        }
    }

    usbHsFsDestroyClient(&client);
}
