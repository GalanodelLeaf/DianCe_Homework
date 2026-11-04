#pragma once

// Only hardware/RTOS services are stubbed. Tests use the real LVGL and project code.
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#ifdef _MSC_VER
#include <sys/stat.h>
#ifndef S_ISDIR
#define S_ISDIR(mode) (((mode) & _S_IFMT) == _S_IFDIR)
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define ESP_ERROR_CHECK(x) assert((x) == 0)
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define configASSERT(x) assert(x)
#define MALLOC_CAP_SPIRAM 1
#define pdTRUE 1
#define portMAX_DELAY UINT32_MAX
#define UART_NUM_1 1
#define GPIO_NUM_44 44
#define GPIO_NUM_43 43
#define UART_PIN_NO_CHANGE -1
#define UART_DATA_8_BITS 8
#define UART_PARITY_DISABLE 0
#define UART_STOP_BITS_1 1
#define UART_HW_FLOWCTRL_DISABLE 0
#define UART_SCLK_APB 0

typedef void *SemaphoreHandle_t;
typedef struct {
    int baud_rate, data_bits, parity, stop_bits, flow_ctrl, source_clk;
} uart_config_t;

static inline int uart_param_config(int port, const uart_config_t *config) { return 0; }
static inline int uart_set_pin(int port, int tx, int rx, int rts, int cts) { return 0; }
static inline int uart_driver_install(int port, int rx, int tx, int qsize, void *q, int flags) { return 0; }
int uart_read_bytes(int port, void *buffer, unsigned int size, uint32_t ticks);
static inline void *heap_caps_malloc(size_t bytes, int caps) { return calloc(1, bytes); }
static inline SemaphoreHandle_t xSemaphoreCreateBinary(void) { return (void *)1; }
static inline int xSemaphoreGive(SemaphoreHandle_t sem) { return pdTRUE; }
static inline int xSemaphoreTake(SemaphoreHandle_t sem, uint32_t ticks) { return pdTRUE; }
static inline void vTaskDelay(unsigned int ticks) {}
static inline int xTaskCreatePinnedToCore(void (*task)(void *), const char *name,
                                        unsigned int stack, void *arg, unsigned int priority,
                                        void *handle, int core) { return pdTRUE; }
static unsigned test_lvgl_lock_depth = 0;
static inline bool lvgl_port_lock(uint32_t ticks) {
    assert(ticks == 0); ++test_lvgl_lock_depth; return true;
}
static inline void lvgl_port_unlock(void) {
    assert(test_lvgl_lock_depth > 0); --test_lvgl_lock_depth;
}
bool esp_qmc5883_port_read_heading(float *heading);

// MinGW lacks newlib's strsep. This shim preserves empty fields just as newlib does.
static inline char *test_strsep(char **rest, const char *delimiters)
{
    if (*rest == NULL) return NULL;
    char *start = *rest;
    char *end = strpbrk(start, delimiters);
    if (end != NULL) { *end = '\0'; *rest = end + 1; }
    else *rest = NULL;
    return start;
}
#define strsep test_strsep

#ifdef __cplusplus
}

#endif
