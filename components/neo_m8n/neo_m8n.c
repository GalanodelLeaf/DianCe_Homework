#include "neo_m8n.h"
#include <stdbool.h>
#include <stdlib.h>

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
static char neo_line[NEO_LINE_MAX];
static size_t neo_line_length;

void neo_port_init(void)
{
    ESP_ERROR_CHECK(uart_param_config(NEO_UART_NUM, &gps_uart_config));
    // 把UART1映射到GPS接线的引脚，不使用流控引脚
    ESP_ERROR_CHECK(uart_set_pin(NEO_UART_NUM, NEO_UART_TX_PIN, NEO_UART_RX_PIN,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(NEO_UART_NUM, NEO_BUF_MAX, NEO_BUF_MAX, 0, NULL, 0));
    ESP_LOGI(GPS_TAG, "GPS module init on UART%d (RX=GPIO%d, TX=GPIO%d)",
             NEO_UART_NUM, NEO_UART_RX_PIN, NEO_UART_TX_PIN);
}

int neo_port_listen(void)
{
    // LVGL 定时器调用此函数：不等待 UART，以免拖动地图时卡顿。
    return uart_read_bytes(NEO_UART_NUM, neo_buffer, NEO_BUF_MAX, 0);
}

// RMC 坐标为 ddmm.mmmm / dddmm.mmmm，地图投影需要带符号的十进制度。
static bool parse_coordinate(const char *text, char hemisphere, bool latitude, double *degrees)
{
    char *end;
    const double raw = strtod(text, &end);
    if (end == text || *end != '\0' || !isfinite(raw) || raw < 0) return false;
    const double whole_degrees = floor(raw / 100.0);
    const double minutes = raw - whole_degrees * 100.0;
    const double value = whole_degrees + minutes / 60.0;
    if (minutes >= 60.0 || value > (latitude ? 90.0 : 180.0)) return false;
    if (latitude ? (hemisphere != 'N' && hemisphere != 'S')
                 : (hemisphere != 'E' && hemisphere != 'W')) return false;
    *degrees = (hemisphere == 'S' || hemisphere == 'W') ? -value : value;
    return true;
}

static bool decode_rmc_line(char *line, struct nema_rmc_data *rmc)
{
    // 同时接受 GNRMC、GPRMC 等 talker；其它报文不改变定位状态。
    if (strlen(line) < 7 || line[0] != '$' || strncmp(line + 3, "RMC,", 4) != 0) return false;
    char *checksum = strchr(line, '*');
    if (checksum == NULL || strlen(checksum + 1) != 2) return false;
    char *end;
    const unsigned long expected = strtoul(checksum + 1, &end, 16);
    if (end != checksum + 3 || expected > 0xff) return false;
    unsigned char actual = 0;
    for (char *p = line + 1; p < checksum; ++p) actual ^= (unsigned char)*p;
    if (actual != expected) return false;
    *checksum = '\0';

    char *fields[13];
    size_t count = 0;
    char *rest = line;
    while (count < 13 && (fields[count] = strsep(&rest, ",")) != NULL) ++count;
    if (count < 3 || strlen(fields[2]) != 1) return false;
    struct nema_rmc_data sample = {0};
    sample.status = fields[2][0];
    // 无定位时经纬度字段可以为空，仍须向上层传递 V。
    if (sample.status == 'V') {
        *rmc = sample;
        return true;
    }
    if (sample.status != 'A' || count < 10 ||
        strlen(fields[1]) >= sizeof(sample.utc_time) ||
        strlen(fields[9]) >= sizeof(sample.date) ||
        strlen(fields[4]) != 1 || strlen(fields[6]) != 1) return false;
    sample.ns_indicator = fields[4][0];
    sample.ew_indicator = fields[6][0];
    if (!parse_coordinate(fields[3], sample.ns_indicator, true, &sample.latitude) ||
        !parse_coordinate(fields[5], sample.ew_indicator, false, &sample.longitude)) return false;
    // Web Mercator 的有效纬度范围，避免极区投影产生无穷大。
    if (fabs(sample.latitude) > 85.05112878) return false;
    strcpy(sample.utc_time, fields[1]);
    strcpy(sample.date, fields[9]);
    sample.speed_over_ground = strtod(fields[7], NULL);
    sample.course_over_ground = strtod(fields[8], NULL);
    if (count > 10) sample.magnetic_variation = strtod(fields[10], NULL);
    if (count > 11) sample.mv_ew_indicator = fields[11][0];
    *rmc = sample;
    return true;
}

void neo_decode(int len, struct nema_rmc_data *rmc)
{
    if (rmc == NULL) return;
    // status == 0 表示本批次没有新的完整有效报文，而不是复用旧定位。
    memset(rmc, 0, sizeof(*rmc));
    if (len > NEO_BUF_MAX) len = NEO_BUF_MAX;
    for (int i = 0; i < len; ++i) {
        const char ch = neo_buffer[i];
        if (ch == '$') {
            neo_line_length = 0;
            neo_line[neo_line_length++] = ch;
        } else if (ch == '\r' || ch == '\n') {
            if (neo_line_length > 0) {
                neo_line[neo_line_length] = '\0';
                decode_rmc_line(neo_line, rmc);
                neo_line_length = 0;
            }
        } else if (neo_line_length > 0) {
            if (neo_line_length < sizeof(neo_line) - 1) {
                neo_line[neo_line_length++] = ch;
            } else {
                neo_line_length = 0; // 超长报文丢弃，等待下一个 $。
            }
        }
    }
}

double lon_to_x(double lon, int z)
{
    return ((lon + 180.0) / 360.0 * pow(2, 1.0 * z));
}

#define PI 3.1415926535

double lat_to_y(double lat, int z)
{
    double phi = lat * PI / 180.0;
    return ((1 - asinh(tan(phi)) / PI) * pow(2, 1.0 * (z - 1)));
}
