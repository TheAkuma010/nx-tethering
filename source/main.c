#include <switch.h>
#include <stdio.h>
#include "usbnet.h"
#include "log.h"

int main(int argc, char* argv[]) {
    consoleInit(NULL);
    log_init();

    printf("usbnet homebrew iniciado.\n");
    log_print("usbnet homebrew iniciado.\n");

    usbnet_init_usb();

    while (appletMainLoop()) {
        hidScanInput();
        u64 kDown = hidKeysDown(CONTROLLER_P1_AUTO);
        if (kDown & KEY_PLUS) break;

        // TODO: Detectar dispositivos USB
        usbnet_poll_usb_devices();

        consoleUpdate(NULL);
        svcSleepThread(1e8); // 100ms
    }

    usbnet_exit_usb();
    log_exit();
    consoleExit(NULL);
    return 0;
}
