#include <switch.h>
#include "ipc.h"
#include "log.h"

static Service g_usbnet_service;
static Thread g_ipc_thread;
static bool g_ipc_running = false;

static void ipc_server_thread(void* arg) {
    Result rc;
    Handle client;
    while (g_ipc_running) {
        rc = svcAcceptSession(&client, g_usbnet_service.handle);
        if (R_SUCCEEDED(rc)) {
            log_print("Cliente IPC conectado.\n");
            // Aqui você pode tratar comandos via IPC
            svcCloseHandle(client);
        }
    }
}

void ipc_start_server(void) {
    Result rc = smRegisterService(&g_usbnet_service, "usbnet:u", false, 1);
    if (R_FAILED(rc)) {
        log_print("Erro ao registrar serviço usbnet:u: 0x%x\n", rc);
        return;
    }

    g_ipc_running = true;
    threadCreate(&g_ipc_thread, ipc_server_thread, NULL, 0x4000, 0x2C, -2);
    threadStart(&g_ipc_thread);
}

void ipc_stop_server(void) {
    g_ipc_running = false;
    smUnregisterService(&g_usbnet_service);
    threadWaitForExit(&g_ipc_thread);
    threadClose(&g_ipc_thread);
}
