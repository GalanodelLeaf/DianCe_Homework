# 地图导航主机回归测试

本目录测试真实 LVGL 8.4、地图页面、GPS 解析和指针图片。只替代串口、磁力计与 FreeRTOS 服务，不是 ESP32 整机仿真，不读写设备 SD 卡。

## 本次功能

- 真实 pointer 按下、拖动、松手：左上/反向、多次跨瓦片平移，不使用内部 raw scroll 来冒充触摸输入。
- `+/-` 在可用离线层级间缩放，保持屏幕中心的地理位置；稀疏层级如 12/14/16 可跳层。
- 独立地图视图和定位点；手动平移后 GPS 更新、超时或 V 报文不把地图拉回。第一次有效定位仅在用户未操作地图时居中。
- 定位图标按钮单次回到箭头所在位置，保持当前层级；无定位时使用最后位置，未定位过使用默认位置，不开启持续跟随。
- 同层横/纵一格读三张、复用六张；斜向一格读五张，重复成功 revision 零读取。首次、切层或大跳仍读九张。
- 每瓦片前检查请求是否过期，过期批次不提交，立即取最新请求；单个进行中的同步读取不声称可中断。
- 严格原始瓦片检查：256×256 RGB565、恰好 131072 字节，拒绝短/长文件、读取错误和 PNG/JPEG 内容。正式加载不计算完整诊断统计，也不改变像素颜色。

## CMake

先安装本工程 ESP-IDF 依赖，使 `managed_components/lvgl__lvgl` 存在；或使用 `-DLVGL_ROOT` 指定已有 LVGL 8.4 源码。需要 C/C++ 编译器、CMake 和 Ninja。

```text
cmake -S tests/navigation -B build/navigation-tests -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/navigation-tests
ctest --test-dir build/navigation-tests --output-on-failure
```

包含两项测试：

1. `navigation`：GPS 解析、真实 LVGL 触摸/缩放/回中、地理边界、旧请求、失败瓦片重试和读取量。
2. `map_tile_diagnostics`：快速与完整读取的状态/像素一致性，像素统计、大小和签名检查。

导航测试显式解除 `NDEBUG`，Release 仍执行断言。输入和显示驱动的对象寿命覆盖整个测试过程。

## Windows MSVC

在项目根目录的 Windows 命令提示符运行：

```cmd
tests\navigation\run_msvc.cmd
```

可传入 LVGL 源码目录：

```cmd
tests\navigation\run_msvc.cmd C:\deps\lvgl
```

脚本使用 Visual Studio Installer 的 `vswhere` 查找已安装的 C++ 工具链，以及 Visual Studio 的 CMake/Ninja。测试构建目录是忽略的 `tests/navigation/build-msvc`；TMP/TEMP 仅在该脚本进程中指向此目录，避免 UCRT 临时文件及中文路径问题。

测试的 CMake 为 MSVC 复制未修改的 LVGL/GPS/箭头依赖到构建目录，避免 `lib.exe` 的响应文件路径编码问题。所有测试临时文件均为电脑本地文件，不涉及 SD 卡或设备备份。

## 行为与边界

- 默认位置是天安门 39.9087°N / 116.3975°E，不代表 GPS 已定位。失去定位保留最后位置及用户视图。
- 18 张瓦片缓冲合计 2359296 字节（约 2.25 MiB）PSRAM；复用没有额外增加瓦片缓冲。
- 地图路径保持 `/sdcard/maps/{z}/{y}/{x}`，无文件扩展名。目录存在不证明当前区域瓦片齐全，缺片使用占位棋盘格。
- 缩放使用 SD 中实际层级；没有新增数字过放大，也没有修复 Z16 原始图像的黑底内容。
- 主机的读取次数与字节数不是 ESP32 帧率、SD 时延或实物触控验收；需另行上板验证。
- 不改变磁力计算法、参数存储或 Flash 分区，不包含临时 USB 诊断任务及校准实验。
