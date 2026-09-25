# ESP-Music —— 一块 ESP32-S3 做的纯音乐终端

把一块 ESP32-S3 开发板 + 一块 ES8388 音频板 + 一块 1.8 寸小屏做成一台**独立的音乐播放器**：
从 [Navidrome](https://www.navidrome.org/) 拉流播放、屏幕上显示封面/歌词、四个按键操作、
手机连它的热点就能用网页控制台控制、还能把四个键当成蓝牙 HID 遥控器去控制手机/电脑。

**没有用任何播放器框架**（没有 esp_audio_simple_player / GMF / Lua），音频管道是自己写的 ——
原因见下面的「为什么自己写」。

---

## 硬件

| 部件 | 型号 / 说明 |
|---|---|
| 主控 | **ESP32-S3**（N16R8：16MB flash + 8MB Octal PSRAM） |
| 音频 | **ES8388** codec（I2C 控制 + I2S 数据）+ PAM8406 功放 |
| 屏幕 | **ST7735** 160×128 SPI 屏 |
| 按键 | K1–K4（低电平按下，内部上拉） |

### 引脚（按本项目实际接线，改线请同步改 `components/board/include/board.h`）

| 用途 | GPIO |
|---|---|
| ES8388 I2C | SDA=1, SCL=2（**I2C 地址填 0x22 八位形式**，驱动内部会 `>>1`） |
| ES8388 I2S | MCK=42, BCK=41, WS=39, DOUT=40, DIN=38 |
| PAM8406 使能 | 47（高有效） |
| ST7735 | SCLK=4, MOSI=5, RST=6, DC=7, CS=15, BLK=16（SPI2 @40MHz） |
| 按键 | K1=3, K2=8, K3=18, K4=17 |

⚠️ **K1 在 GPIO3 是 strapping 脚**：开机瞬间按住它会进 JTAG 下载模式。正常使用没事，别在上电时按着 K1。

---

## 功能

| 功能 | 说明 |
|---|---|
| **本地播放** | Navidrome 转码拉流 → 解码 → I2S → ES8388。位置由「已交给 DMA 的 PCM 帧数」推导，**不是墙钟**，暂停/卡顿都不漂移 |
| **屏幕 UI** | 播放页（封面 + 歌词 3 行 + 进度）、列表页、菜单、音量页、系统信息、来源二级菜单、蓝牙控制页、搜索三页 |
| **网页控制台** | 局域网内 `http://<设备IP>/`，无鉴权（用户要求）。播放控制 / 选歌 / 搜索 / 收藏 / 最近播放 / 歌词动画 / WiFi 配置（扫描 + 点选，不重启生效）/ Navidrome 配置 |
| **蓝牙 HID 遥控** | K1–K4 当 Consumer Control 遥控器控制手机/电脑（菜单第 1 项「蓝牙控制」进入） |
| **屏幕搜索** | 屏上字母键盘 + **拼音输入法**（打 `yue` 出候选「越月约跃…」），搜中文歌不用手机的输入法 |
| **收藏 / 最近播放** | 与 Navidrome 双向同步，落盘到 cache 分区，重启不丢 |
| **开机续播** | 记住上次在放哪首、放到哪一秒，重启接着放 |

### 按键

| 页面 | K1 | K2 | K3 | K4 |
|---|---|---|---|---|
| 播放页·短按 | 下一首 | 播放/暂停 | 上一首 | 换来源 |
| 播放页·长按 | 收藏 | 音量页 | 列表页 | 菜单 |
| 列表 / 菜单 / 音量 / 搜索页 | 相应的上/左 | 相应的下/右 | 确认 | **短按回上一级 · 长按回播放器** |
| 搜索输入页 | **向右** | **向左** | 输入 / 选字 | 同上 |
| 蓝牙控制页 | 下一首 | 播放/暂停 | 上一首 | 退出（长按 K2 = 遥控对方音量） |

---

## 编译与烧录

需要 **ESP-IDF v5.5.x**（开发用的是 v5.5.4）。

```bash
. $HOME/esp/esp-idf/export.sh
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

首次编译会自动从组件仓库拉依赖（`idf_component.yml` 里都锁了版本）。

### 配置（WiFi / Navidrome）

**凭据不进版本库。** 建一个 `sdkconfig.local`（已被 gitignore）：

```bash
CONFIG_APP_WIFI_SSID="你的WiFi名"
CONFIG_APP_WIFI_PASSWORD="你的WiFi密码"
CONFIG_APP_NAV_HOST="http://192.168.1.100:4533"
CONFIG_APP_NAV_USER="你的用户名"
CONFIG_APP_NAV_PASS="你的密码"
```

```bash
cp sdkconfig.local.template sdkconfig.local   # 然后填进去
idf.py build
```

⚠️ **这里有个坑（本项目专门加了工具对付它）**：IDF 的 `SDKCONFIG_DEFAULTS` 只在
`sdkconfig` **不存在**或某符号**还没被赋值**时才生效。所以改了 `sdkconfig.local` 再
build 是**无效的**（实测：换了地方改了 SSID，设备还是去连旧网络）。
`CMakeLists.txt` 会在每次 configure 前跑 `tools/sync_sdkconfig.py`，把
`sdkconfig.local` / `sdkconfig.forced` 的值**强制覆盖**进 `sdkconfig`。

也可以完全不改代码：设备起来后用网页控制台或屏幕菜单配 WiFi 和 Navidrome
（存 NVS，优先级高于 Kconfig）。

### 素材与分区

预渲染图 / 中文字库 / 网页 / 拼音表都打包在 `assets` 分区（4MB），运行时 mmap **零拷贝**
读取 —— 所以改一张图或一行 HTML 不用重烧固件。它们已随仓库提供（`components/assets/assets.bin`），
克隆下来直接能烧。

想改素材的话：

```bash
python3 tools/gen_ui_png.py     # 改界面底图（构图改这个脚本，别改 C 代码）
python3 tools/gen_pinyin.py     # 生成拼音输入法的表（需要 Rime 的 8105 词典，见脚本注释）
python3 tools/gen_ui_text.py    # UI 文案字符清单（开机自检字库覆盖用）
python3 tools/gen_assets.py     # 打包成 assets.bin
idf.py -p /dev/ttyACM0 flash    # 会自动把 assets.bin 烧到 0x620000
```

---

## 工程结构

```
components/
├── app_core/       # 状态快照（各模块之间唯一的共享点）
├── board/          # I2C/I2S/SPI/GPIO + ES8388 封装 + 混合音量
├── assets/         # assets 分区 mmap + 索引
├── audio_engine/   # 音频管道：fetch → ring_in → 解码 → ring_pcm → I2S
├── subsonic/       # Navidrome 客户端（列表/流/收藏/歌词/封面/搜索）
├── store/          # cache 分区 FATFS + 磨损均衡（最近播放/收藏/封面缓存落盘）
├── settings/       # NVS 设置（带去抖落盘）
├── player/         # 播放命令层：队列 + 自动连播 + 搜索
├── net_mgr/        # WiFi STA+AP、扫描、重连
├── hid_remote/     # NimBLE + esp_hid：K1-K4 当 BLE 遥控器
├── ui/             # LVGL 页面 + 自写显示驱动 + 按键分发 + 拼音输入法
└── web_console/    # esp_http_server：REST API + 网页
main/app_main.c     # 只做编排
```

**分层约定**：`ui / web_console / hid_remote / net_mgr` **互不引用**，全部经
`app_core` 的状态快照 + `player` 的命令队列交互 —— 从结构上保证"一路控制卡住不会拖死 UI"。

---

## 为什么自己写音频管道（而不是用现成播放器）

这个工程是从另一个用 `esp_audio_simple_player` + GMF 的版本重写来的。踩到的坑**全部来自框架**：

- GMF 音频组件的**内存破坏 bug** 导致 FLAC 直传必崩（三次崩溃回溯才定位）
- 播放位置**不可信**（歌词对不上、seek 之后位置飘）
- `managed_components` 里的 4 处手改补丁**一拉依赖就丢**
- app 分区被吃到 93%

自己写之后：解码器直接调 `esp_audio_simple_dec`，位置从 DMA 帧数推导（和声音严格一致），
没有一层框架要把状态从它肚子里掏出来。

---

## 一些反直觉的实测事实（省得后来者再踩）

代码注释里都有详细说明，这里只列结论：

1. **ES8388 的 I2C 写入会静默失败** → 初始化必须"写 → 读回 → 重试"并统计重试次数。
2. **S3 没有硬件 ASRC** → 采样率变了要重配 I2S 时钟，不能指望重采样。
3. **混合音量要自己写 4 个寄存器**（`0x2E–0x31`）：`esp_codec_dev_set_out_vol()` 只动数字音量，
   而且 `esp_codec_dev_open()` 会把模拟音量清掉 —— 每次 open 之后都要重新施加。
4. **Navidrome 只在"真转码"时尊重 `timeOffset`** → seek 必须保证转码（显式带 `format=mp3`），
   否则它静默忽略，表现为"seek 之后从头播"。
5. **中文字库至少 4bpp**：2bpp 只有 4 级灰度，汉字细笔画被量化丢失后就成了"别的字"。
6. **字库缺字形在屏上是【一片空白】不是方框**；更阴的是还有一类"**码点在、字形位图是空的**"
   （`…` 就是这样）→ 开机自检会连 box 尺寸一起查（`ui_font_audit_text()`）。
7. **FATFS 的 `rename` 不覆盖已存在目标**（返回 FR_EXIST）→ 原子写要"先 unlink 再 rename"。
8. **内部 SRAM 是这块板唯一紧张的东西**：LVGL 控件默认会占它（小于 4KB 的 malloc 都走内部），
   加一个新页面就可能把蓝牙挤死 → 见 `sdkconfig.forced` 里 `SPIRAM_MALLOC_ALWAYSINTERNAL` 的说明。
9. **NimBLE 在 S3 上必须自己启动主机任务**（`esp_nimble_enable()`），漏了表现为"能起来但不广播、不报错"。
10. **`lv_font_montserrat_16` 没有启用**，别用它（链不过）。

---

## 许可与素材来源

- 本工程代码：见 `LICENSE`
- 中文字形来自 **Droid Sans Fallback**（Apache License 2.0，Android 系统字体）的子集
- 拼音数据来自 [rime-ice](https://github.com/iDvel/rime-ice) 的 `8105.dict.yaml`（通用规范汉字表）
- 界面图标是脚本画的（`tools/gen_ui_png.py`），字体用 LVGL 内置的 Montserrat
