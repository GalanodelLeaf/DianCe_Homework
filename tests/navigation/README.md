# 地图定位箭头回归测试

使用项目中的 LVGL 8.4、地图界面源码和 GPS 解析源码，仅替代串口、磁力计与 FreeRTOS 等硬件接口。需要已下载 `managed_components/lvgl__lvgl`，以及本机 C/C++ 编译器、CMake 和 Ninja。

在项目根目录运行（Windows 可使用 MinGW，并将其 `bin` 目录加入 PATH）：

```text
cmake -S tests/navigation -B build/navigation-tests -G Ninja
cmake --build build/navigation-tests
ctest --test-dir build/navigation-tests --output-on-failure
```

覆盖拖动后的定位点重合、四个屏幕边缘的隐藏/恢复、隐藏期间旋转、GPS 更新后保留拖动位移、失去定位及恢复、天安门默认点、报文跨批次接收、校验和、坐标格式转换和缓冲边界。

当前行为：开机使用天安门（39.9087°N，116.3975°E）；RMC 状态 V 立即回退，连续 3 秒没有新的有效定位也回退。仅以定位点中心判断是否位于屏幕内，中心仍在屏内时允许图标边缘被屏幕裁切。

地图使用两组九张 RGB565 瓦片缓冲以同步提交地图和箭头，较原实现额外使用 1,179,648 字节 PSRAM。桌面测试不替代实机触控、SD 卡读取和传感器验证。
