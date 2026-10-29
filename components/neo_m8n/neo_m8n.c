#include "neo_m8n.h"

static const char *GPS_TAG = "NEO_M8N";

const uart_config_t gps_uart_config =
{
    .baud_rate = 9600,
    .data_bits = UART_DATA_8_BITS,
    .parity = UART_PARITY_DISABLE,
    .stop_bits = UART_STOP_BITS_1,
    .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
    .source_clk = UART_SCLK_APB
};

static char neo_buffer[NEO_BUF_MAX];

void fill_empty(char* line);

void neo_port_init(void)
{
    ESP_ERROR_CHECK(uart_param_config(UART_NUM_0, &gps_uart_config));
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM_0, NEO_BUF_MAX, NEO_BUF_MAX, 0, NULL, 0));
    ESP_LOGI(GPS_TAG, "GPS module init");
}

int neo_port_listen(void)
{
    return uart_read_bytes(UART_NUM_0, neo_buffer, NEO_BUF_MAX, 50);
}

void neo_decode(int len, struct nema_rmc_data *rmc)
{
    char neo_line[NEO_LINE_MAX];

    if (len <= 0) return;
    if (len > NEO_BUF_MAX) len = NEO_BUF_MAX;   // 防御
    neo_buffer[len] = '\0';                     // 需要 neo_buffer[NEO_BUF_MAX + 1]

    char *rest = neo_buffer;
    char *token;

    while ((token = strsep(&rest, "\n")) != NULL) {
        size_t tlen = strlen(token);
        if (tlen && token[tlen - 1] == '\r') token[--tlen] = '\0';
        if (tlen == 0 || token[0] != '$') continue;
        if (tlen >= NEO_LINE_MAX) continue;      // 超长丢弃

        memcpy(neo_line, token, tlen + 1);
        fill_empty(neo_line);

        if (strncmp(neo_line, "$GNRMC", 6) == 0)
        {
            int n = sscanf(neo_line,
                "$G%*[^,],%[^,],%c,%lf,%c,%lf,%c,%lf,%lf,%[^,],%lf,%c",
                rmc->utc_time, &rmc->status,
                &rmc->latitude, &rmc->ns_indicator,
                &rmc->longitude, &rmc->ew_indicator,
                &rmc->speed_over_ground, &rmc->course_over_ground,
                rmc->date, &rmc->magnetic_variation,
                &rmc->mv_ew_indicator);
            if (n != 11) {
                memset(rmc, 0, sizeof (struct nema_rmc_data));   // 或标记无效
            }
        }
    }
    printf("%s\n", neo_buffer);
}

// 空数据时会有连续逗号串，没法直接格式化读取，先给空数据填充0，方便格式化读取
// new_line取得足够长，应该不用担心溢出的问题
void fill_empty(char *line)
{
    char new_line[NEO_LINE_MAX];
    char *dst = new_line;
    char *dst_end = new_line + sizeof(new_line) - 1;
    char *rest = line;
    char *token;

    while ((token = strsep(&rest, ",")) != NULL) {
        size_t len = strlen(token);
        size_t need = (len == 0 ? 1 : len) + 1;   // 字段 + 逗号
        if (dst + need > dst_end) {               // 空间不够，放弃
            *line = '\0';
            return;
        }
        if (len == 0) {
            *dst++ = '0';
        } else {
            memcpy(dst, token, len);
            dst += len;
        }
        *dst++ = ',';
    }
    *dst = '\0';
    strcpy(line, new_line);   // new_line 已保证以 '\0' 结尾且长度 < NEO_LINE_MAX
}

int lon_to_x(double lon, int z)
{
    return (int)((lon + 180.0) / 360.0 * pow(2, 1.0 * z));
}

#define PI 3.1415926535

int lat_to_y(double lat, int z)
{
    double phi = lat * PI / 180.0;
    return (int)((1 - asinh(tan(phi)) / PI) * pow(2, 1.0 * (z - 1)));
}