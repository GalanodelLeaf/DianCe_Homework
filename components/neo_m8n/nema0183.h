/* 可见卫星信息 */
// struct nema_gsv_data
// {
//     int total_messages;     // GSV语句总数
//     int message_number;     // 当前GSV语句的序号
//     int total_satellites;   // 可见卫星总数，0-12
//     int satellite_id;       // 卫星编号，0-32
//     int elevation;          // 卫星仰角，0-90度
//     int azimuth;            // 卫星方位角，0-359度
//     int snr;                // 信噪比，0-99dB
//     int check;              // 检查位
// };

/* 卫星定位信息 */
// struct nema_gga_data
// {
//     char utc_time[11];          // UTC时间，hhmmss.sss格式
//     double latitude;            // 纬度，单位：度
//     char ns_indicator;          // 北纬/南纬指示符，N或S
//     double longitude;           // 经度，单位：度
//     char ew_indicator;          // 东经/西经指示符，E或W
//     int fix_quality;            // 定位质量，0=无效，1=单点定位，2=差分定位，3=固定解，4=浮点解
//     int num_satellites;         // 使用的卫星数量，0-12
//     double hdop;                // 水平精度因子
//     double altitude;            // 海拔高度，单位：米
//     char altitude_units;        // 海拔高度单位，M=米
//     double geoid_separation;    // 大地水准面分离值，单位：米
//     char geoid_units;           // 大地水准面分离值单位，M=米
// };

/* 推荐定位信息 */
struct nema_rmc_data
{
    char utc_time[11];          // UTC时间，hhmmss.sss格式
    char status;                // 定位状态，A=有效，V=无效
    double latitude;            // 纬度，单位：度
    char ns_indicator;          // 北纬/南纬指示符，N或S
    double longitude;           // 经度，单位：度
    char ew_indicator;          // 东经/西经指示符，E或W
    double speed_over_ground;   // 地面速率，单位：节（1节=1海里/小时）
    double course_over_ground;  // 地面航向，单位：度
    char date[7];               // 日期，ddmmyy格式
    double magnetic_variation;  // 磁偏角，单位：度
    char mv_ew_indicator;       // 磁偏角方向指示符，E或W
};

/* 地面速度信息 */
// struct nema_vtg_data
// {
//     double true_track;                      // 真航向，单位：度
//     char true_track_indicator;              // 真航向指示符，T=真航向
//     double magnetic_track;                  // 磁航向，单位：度
//     char magnetic_track_indicator;          // 磁航向指示符，M=磁航向
//     double speed_over_ground_knots;         // 地面速率，单位：节（1节=1海里/小时）
//     char speed_over_ground_knots_indicator; // 地面速率指示符，N=节
//     double speed_over_ground_kmh;           // 地面速率，单位：公里/小时
//     char speed_over_ground_kmh_indicator;   // 地面速率指示符，K=公里/小时
// };