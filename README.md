# lcder2

`lcder2` 是一个基于 `ESP8266_RTOS_SDK` 和 `LVGL 8.3.6` 的天气小屏项目，运行在 ESP8266 上，提供本地时间显示、心知天气信息展示、网页配置入口，以及 WiFi 配网与恢复逻辑。

当前界面以天气为主，顶部固定显示城市、时间、日期，下方天气区域会自动横向切换 3 个页面：

- 当前天气
- 今日天气
- 明天天气

每个天气页面显示天气图标、天气现象、温度，以及风向、风速。项目同时包含一个二维码页面，用于快速打开设备的本地配置页面。

## 功能概览

- 基于 `LVGL` 的单色小屏界面
- 使用 `SNTP` 同步时间，显示本地日期与时间
- 接入心知天气 API
- 显示当前天气、今日天气、明天天气
- 风速自动转换为风力等级显示
- 启动后自动轮播天气页面
- 按键切换到二维码页面
- 内置本地网页配置页
- 支持修改天气城市
- 支持修改 WiFi 凭据
- 支持保存并恢复天气城市与 WiFi 配置
- WiFi 不可用时可回退到重新配网流程

## 软件架构

项目主要由以下几部分组成：

- [`main/lcder_main.c`](/g:/08-Embedded/06-esp/msys32/home/huanghui/myPrj/lcder2/main/lcder_main.c)
  应用入口、LVGL 任务、页面自动切换、时间刷新、二维码页面控制。
- [`main/http_seniverse.c`](/g:/08-Embedded/06-esp/msys32/home/huanghui/myPrj/lcder2/main/http_seniverse.c)
  心知天气接口访问、城市查询、天气数据解析、界面回填。
- [`main/config_portal.c`](/g:/08-Embedded/06-esp/msys32/home/huanghui/myPrj/lcder2/main/config_portal.c)
  本地 HTTP 配置页面，提供城市和 WiFi 修改入口。
- [`main/wifi_sta.c`](/g:/08-Embedded/06-esp/msys32/home/huanghui/myPrj/lcder2/main/wifi_sta.c)
  WiFi 连接、恢复、切换与配网流程。
- [`main/startup_ui.c`](/g:/08-Embedded/06-esp/msys32/home/huanghui/myPrj/lcder2/main/startup_ui.c)
  启动画面与状态提示。
- [`main/screens/ui_Screen1.c`](/g:/08-Embedded/06-esp/msys32/home/huanghui/myPrj/lcder2/main/screens/ui_Screen1.c)
  主天气界面布局。
- [`GUI/`](/g:/08-Embedded/06-esp/msys32/home/huanghui/myPrj/lcder2/GUI)
  LVGL 源码及相关组件。

## 目录结构

```text
lcder2/
├─ GUI/                 LVGL 及扩展组件
├─ main/                主应用代码
│  ├─ config_portal.c   网页配置入口
│  ├─ http_seniverse.c  天气接口与解析
│  ├─ lcder_main.c      主循环与界面切换
│  ├─ startup_ui.c      启动状态界面
│  ├─ wifi_sta.c        WiFi 管理
│  ├─ screens/          LVGL 页面布局
│  └─ fonts/            字库文件
├─ Makefile
├─ CMakeLists.txt
└─ README.md
```

## 开发环境

本项目基于 `ESP8266_RTOS_SDK` 的传统 `make` 构建方式。

建议环境：

- Windows + MSYS2
- `ESP8266_RTOS_SDK`
- `xtensa-lx106-elf` 工具链
- `make`
- Python（满足 SDK 依赖）

需要确保已经正确设置：

- `IDF_PATH`
- 工具链路径
- `make` 可执行路径

## 构建方式

在项目根目录执行：

```bash
make -j4 app
```

如果需要烧录并打开串口监视器：

```bash
make -j4 flash monitor
```

退出串口监视器可使用：

```text
Ctrl-]
```

## 配置项

如需修改串口、Flash 参数等，可先执行：

```bash
make menuconfig
```

常见需要关注的配置：

- 串口号
- Flash 参数
- WiFi 相关配置

## 使用说明

### 1. 上电启动

设备启动后会初始化 LCD、LVGL、WiFi、天气模块和 SNTP。

### 2. WiFi 连接

系统会优先尝试使用已保存的 WiFi 凭据连接网络。

如果保存的 WiFi 不可用，则会进入重新配网或恢复流程。启动界面会显示当前状态。

### 3. 时间同步

WiFi 联网成功后会启动 `SNTP` 同步时间，并在界面顶部显示日期和时间。

### 4. 天气刷新

联网后会立即触发一次天气刷新，之后按固定周期刷新天气数据。

天气界面会在以下 3 页之间自动横向切换：

- 当前天气
- 今日天气
- 明天天气

### 5. 网页配置入口

联网成功后设备会启动本地配置页面。

配置页面地址格式为：

```text
http://设备IP/
```

配置页可以完成：

- 修改天气城市
- 修改 WiFi SSID 和密码
- 查看当前配置状态

### 6. 二维码页面

按键触发后可切换到二维码页面，二维码内容为当前配置页面 URL，方便手机直接访问。

## 天气数据说明

项目目前使用心知天气接口：

- `weather/now`
- `weather/daily`
- `location/search`

当前实现中会显示：

- 当前天气现象
- 当前温度
- 今日天气现象
- 今日最高/最低温
- 明日天气现象
- 明日最高/最低温
- 风向
- 风速等级

说明：

- 当前页面使用实况天气接口
- 今日/明日页面使用日天气预报接口
- 风速会由原始数值换算成风力等级显示

## 按键与界面行为

- 默认显示天气主界面
- 天气内容区自动轮播
- 按下按键可切换到二维码页面
- 二维码页面会在超时后自动返回天气界面

## 字库说明

项目使用了自定义中文字库。若修改了界面文案、天气字段或城市名称范围，通常需要同步更新字库内容，否则可能出现方框或乱码。

字库相关文件位于：

- [`main/fonts/`](/g:/08-Embedded/06-esp/msys32/home/huanghui/myPrj/lcder2/main/fonts)

## 注意事项

- 本项目依赖网络，未联网时天气和时间同步功能不可用
- 若心知天气免费接口字段受限，界面中应仅使用免费可访问字段
- 修改显示文本后，建议检查字库是否包含对应字符
- 项目中部分 UI 文件带有 SquareLine Studio 生成痕迹，手工修改后如重新导出 UI，需要注意覆盖关系

## 后续可扩展方向

- 增加更多天气指标
- 增加空气质量页面
- 优化城市搜索与配置交互
- 增加更多按键交互
- 支持更完整的低功耗策略

## License

本仓库暂未单独补充项目许可证说明。如需开源发布，建议补充明确的 License 文件。
