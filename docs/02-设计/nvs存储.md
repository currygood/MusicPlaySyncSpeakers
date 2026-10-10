# NVS 存储设计

> **文档范围**：主节点（Play_Master）、从节点（Play_Slave）、灯控节点三个固件的 NVS 存储统一设计。
> **存储原则**：配置统一以「整体式 blob」存储，由各节点对应模块统一读写；其余业务模块只读不写 NVS；字段变更通过 `version` 结构版本号管理。

------

## 1. 主节点（Play_Master）

### 1.1 存储方式

| 项目 | 内容 |
| :--- | :--- |
| NVS 分区 | 默认 `nvs` 分区（`nvs_flash_init()` 之后可用） |
| 命名空间 | `node_role` |
| Key | `cfg`（单 blob） |
| 结构 | `node_role_cfg_t` + `version` 头 |
| 管理模块 | `node_role`：`node_role_init(defaults)` 首次写入、`node_role_get()/set()` 整体式读写（原子提交） |

- `wifi_manager`、`ota_manager`、`MusicPlay` 等模块一律通过 `node_role_get()` 读取，不直接操作 NVS；
- 写入时机：首次启动（defaults）、触摸屏配网、UI 修改音量/播放模式、OTA 服务器地址配置时。

### 1.2 字段表

| 字段 | 类型 | 默认值 | 说明 |
| --- | --- | --- | --- |
| `version` | uint16_t | 1 | blob 结构版本号，结构变更时递增用于迁移 |
| `role` | uint8_t（`node_role_t`） | `NODE_ROLE_MASTER`（0） | 节点角色：主 / 从 |
| `wifi_ssid` | char[32] | 空 | WiFi 名称（触摸屏配网填写） |
| `wifi_password` | char[64] | 空 | WiFi 密码（触摸屏配网填写） |
| `ota_server_url` | char[256] | `192.168.4.16:5000/ota/check` | OTA 服务器地址，`ota_manager` 启动按 `cfg->check_url → node_role 配置 → 默认宏` 读取；默认值不带协议头，使用时自行补 `http://` |
| `volume` | uint8_t | 80 | 软件音量 0~100，掉电恢复 |
| `play_mode` | uint8_t（`music_play_mode_t`） | 0（`MUSIC_PLAY_MODE_SEQUENTIAL`） | MusicPlay 播放模式：0 顺序播放、1 单曲循环，UI 修改时写入 |
| `sync_delay_ms` | uint32_t | 200 | 组播同步延迟 D（ms），掉电恢复 |
| `audio_sample_rate` | uint32_t | 44100 | 音频采样率，全链路固定 44.1kHz，保留字段 |
| `multicast_group` | char[16] | `239.0.0.1` | 音频组播地址；固定默认，不提供 UI 修改入口 |

### 1.3 说明

- `play_mode`、`version` 字段已并入主文档 §3.4.3 的 `node_role_cfg_t`（blob 结构带 `version` 头），字段顺序与本表一致；
- 端口号（音频 5678/5679、灯控 8889）为代码常量，不入 NVS；
- `device_name`（蓝牙广播名）不入 NVS，按 `PLAY_MASTER_BT` 自动生成；
- 开发期默认 WiFi（`HW666` / `ADajLP691TY.`）由 `node_role` 内置默认宏（`NODE_ROLE_DEFAULT_SSID`/`NODE_ROLE_DEFAULT_PASSWORD`）提供并首次落盘，生产默认留空、由触摸屏配网填写；
- SD 续播（`last_track_*`）本期不实现，预留后续版本扩展；播放源与播放状态属运行态，不持久化；
- 配置统一为整体 blob 存储（命名空间 `node_role`、Key `cfg`），NVS 中不单独建键（含 `ota_server_url`）；各业务模块一律经 `node_role_get()` 读取，不直接操作 NVS。

------

## 2. 从节点（Play_Slave）

### 2.1 存储方式

- 与主节点共用同一份 blob 结构（`node_role_cfg_t`），仅 `role = NODE_ROLE_SLAVE`，命名空间 / Key 相同；
- 从节点无触摸屏，默认参数经由固件 `defaults` 写入。

### 2.2 字段差异说明

| 字段 | 说明 |
| --- | --- |
| `role` | 固定 `NODE_ROLE_SLAVE` |
| `wifi_ssid` / `wifi_password` | 保存；从节点需接入同一局域网（同主节点配网）接收组播 |
| `ota_server_url` | 保存；从节点同样支持 OTA 升级 |
| `volume` | 运行期音量由主节点 `SYNC_VOLUME` 同步；该字段仍保留，作为开机初始音量（默认 80） |
| `sync_delay_ms` | 保存（默认 200），从节点同步算法使用 |
| `play_mode` | 从节点无本地播放源，不使用；共享结构保留，写默认 0 |
| `audio_sample_rate` / `multicast_group` | 与主节点一致（44100 / `239.0.0.1`） |

------

## 3. 灯控节点

### 3.1 存储方式

| 项目 | 内容 |
| :--- | :--- |
| NVS 分区 | 默认 `nvs` 分区 |
| 命名空间 | `light` |
| Key | `cfg`（单 blob） |
| 结构 | `light_node_cfg_t`（整体式读写，风格与 `node_role` 一致） |

### 3.2 字段表

| 字段 | 类型 | 默认值 | 说明 |
| --- | --- | --- | --- |
| `wifi_ssid` | char[32] | 空 | WiFi 名称 |
| `wifi_password` | char[64] | 空 | WiFi 密码 |
| `room_name` | char[16] | `bedroom` | 房间名，与灯控 JSON 指令 `light` 字段一致，作为组播指令路由键 |

- blob 版本机制与主节点一致（`version` 默认 1），字段表不单列。
