#include "dashboard.h"

#include "app.h"
#include "stm32f769i_discovery_lcd.h"

#include <stdio.h>
#include <string.h>

#define COLOR_BACKGROUND 0xFF0D1117
#define COLOR_TEXT 0xFFC9D1D9
#define COLOR_MUTED 0xFF8B949E
#define COLOR_ACCENT 0xFF58A6FF
#define COLOR_PANEL 0xFF161B22

static const uint32_t STATE_COLORS[] = {
    [CWIST_LWIP_CONN_FREE] = COLOR_PANEL,     [CWIST_LWIP_CONN_IDLE] = 0xFF1F6FEB,
    [CWIST_LWIP_CONN_RECEIVING] = 0xFFD29922, [CWIST_LWIP_CONN_SENDING] = 0xFFA371F7,
    [CWIST_LWIP_CONN_STREAMING] = 0xFF3FB950,
};

static cwist_lwip_conn_info g_info[FORTRESS_MAX_CONNS] FORTRESS_DTCM;
static int g_previous[FORTRESS_MAX_CONNS] FORTRESS_DTCM;

static void text_at(uint16_t x, uint16_t y, sFONT *font, uint32_t color, char *text) {
    BSP_LCD_SetFont(font);
    BSP_LCD_SetBackColor(COLOR_BACKGROUND);
    BSP_LCD_SetTextColor(color);
    BSP_LCD_DisplayStringAt(x, y, (uint8_t *)text, LEFT_MODE);
}

static void legend(void) {
    static const char *const names[] = {"FREE", "IDLE", "RX", "TX", "STREAM"};
    uint16_t x = 16;
    for (size_t i = 0; i < sizeof(STATE_COLORS) / sizeof(STATE_COLORS[0]); i++) {
        BSP_LCD_SetTextColor(STATE_COLORS[i]);
        BSP_LCD_FillRect(x, 80, 12, 8);
        text_at((uint16_t)(x + 17), 78, &Font8, COLOR_MUTED, (char *)names[i]);
        x += i == 0 ? 76 : (i == 4 ? 0 : 70);
    }
}

int dashboard_init(void) {
    if (BSP_LCD_Init() != LCD_OK) return -1;
    BSP_LCD_LayerDefaultInit(LTDC_ACTIVE_LAYER_BACKGROUND, LCD_FB_START_ADDRESS);
    BSP_LCD_SelectLayer(LTDC_ACTIVE_LAYER_BACKGROUND);
    BSP_LCD_DisplayOn();
    BSP_LCD_SetBackColor(COLOR_BACKGROUND);
    BSP_LCD_Clear(COLOR_BACKGROUND);
    text_at(16, 10, &Font20, COLOR_ACCENT, "CWIST CONNECTION FORTRESS");
    text_at(16, 38, &Font12, COLOR_TEXT, "Starting the 288-slot static connection pool...");
    for (size_t i = 0; i < FORTRESS_MAX_CONNS; i++) g_previous[i] = -1;
    return 0;
}

void dashboard_render(const cwist_lwip_server *server, uint32_t ipv4, uint32_t requests,
                      size_t payload_bytes) {
    cwist_lwip_server_stats stats;
    cwist_lwip_get_server_stats(server, &stats);
    size_t count = cwist_lwip_get_connection_info(server, g_info, FORTRESS_MAX_CONNS);
    char line[128];

    BSP_LCD_SetTextColor(COLOR_BACKGROUND);
    BSP_LCD_FillRect(0, 36, 800, 36);
    snprintf(line, sizeof line, "ACTIVE %u/%u   ACCEPT %lu   REJECT %lu",
             (unsigned)stats.active_connections, (unsigned)stats.max_connections,
             (unsigned long)stats.accepted_connections, (unsigned long)stats.rejected_connections);
    text_at(16, 38, &Font12, COLOR_TEXT, line);
    snprintf(line, sizeof line, "REQUEST %lu   PAYLOAD %u B   NO RTOS / STATIC POOL",
             (unsigned long)requests, (unsigned)payload_bytes);
    text_at(16, 56, &Font12, COLOR_MUTED, line);
    legend();

    for (size_t i = 0; i < count; i++) {
        if (g_previous[i] == (int)g_info[i].state) continue;
        uint16_t x = (uint16_t)(16 + (i % 24) * 32);
        uint16_t y = (uint16_t)(104 + (i / 24) * 24);
        BSP_LCD_SetTextColor(STATE_COLORS[g_info[i].state]);
        BSP_LCD_FillRect(x, y, 28, 20);
        g_previous[i] = (int)g_info[i].state;
    }

    BSP_LCD_SetTextColor(COLOR_BACKGROUND);
    BSP_LCD_FillRect(0, 416, 800, 52);
    text_at(16, 420, &Font12, COLOR_MUTED,
            "RX 512 B / TX 192 B PER SLOT   LARGE RESPONSES STREAM IN CHUNKS");
    if (ipv4) {
        snprintf(line, sizeof line, "http://%u.%u.%u.%u/   OPEN THE LIVE DASHBOARD IN FIREFOX",
                 (unsigned)(ipv4 & 0xff), (unsigned)((ipv4 >> 8) & 0xff),
                 (unsigned)((ipv4 >> 16) & 0xff), (unsigned)((ipv4 >> 24) & 0xff));
    } else {
        snprintf(line, sizeof line, "DHCP: waiting for an IPv4 address...");
    }
    text_at(16, 444, &Font12, COLOR_ACCENT, line);
}
