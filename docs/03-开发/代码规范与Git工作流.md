## 代码规范

1. 每个模块代码都需要有这个注释在开头：

这个是.c文件的

```c
/**
 * @file OTA_Update.c
 * @brief OTA 远程升级模块实现（局域网 Pull-style OTA）
 *
 * 流程：
 *   1. GET /ota/check 获取 JSON {version, sha256, url}
 *   2. 解析并语义化比较版本号
 *   3. 远端版本更新时，通过 esp_https_ota_begin/perform/finish 流式写入备用分区
 *   4. 下载期间通过 esp_http_client 事件回调计算数据流 SHA256
 *   5. 校验通过后切换启动分区并 esp_restart()
 *
 * 模块不直接依赖 WiFi/Audio/AI Cloud，只通过事件回调通知上层，
 * 上层可在 DOWNLOAD_STARTED 时暂停高内存/高带宽任务。
 */
```

这是.h文件的

```c
/**
 * @file OTA_Update.h
 * @brief OTA 远程升级模块接口（局域网 Pull-style OTA）
 *
 * 本模块与局域网内的 Python OTA 服务器配合，完成：
 *   1. 访问 /ota/check 获取最新固件的版本号、SHA256 和下载 URL
 *   2. 与本地运行固件版本做语义化比较（自动忽略 v / esp_v 等前缀）
 *   3. 使用 esp_https_ota 流式下载并写入备用 app 分区
 *   4. 下载完成后校验 SHA256，校验通过后切换启动分区并重启
 *
 * 设计原则：
 *   - 模块不依赖 WiFi、音频、AI Cloud 等具体业务模块，只暴露事件回调。
 *   - main.c 可在事件回调中暂停/恢复音频采集与 WebSocket，实现并发控制。
 *   - 支持一次性阻塞检测 OTA_Update_CheckAndUpgrade()，也支持后台任务 OTA_Update_Start()。
 *
 * 典型用法（在 app_main 中，WiFi 就绪后）：
 *   OTA_Update_Init(NULL);
 *   OTA_Update_Start();
 *
 * 如需自定义服务器地址和本地版本：
 *   ota_update_config_t cfg = {
 *       .check_url = "http://192.168.1.100:5000/ota/check",
 *       .local_version = NULL,   // NULL 时自动读取当前运行固件的版本号
 *       .event_cb = my_ota_event_handler,
 *   };
 *   OTA_Update_Init(&cfg);
 *
 * check_url 解析优先级：cfg->check_url → NVS（node_role 持久化的 ota_server_url）→ OTA_SERVER_CHECK_URL；
 * 因此 OTA_Update_Init(NULL) 也会从 NVS 读到已配置的服务器地址。
 */
```

2. 还需要有很多合理的注释，例如函数前面的注释

```c
/**
 * @brief 查询 OTA 模块是否正在执行检测/升级
 *
 * @return true  忙
 *         false 空闲
 */
 bool OTA_Update_IsBusy(void);
```

3. 模块对外api函数使用驼峰式命名，而且名字前面的单词应该是该模块的功能简要，例如前面的 `OTA_Update`

4. #define  宏全部单词大写

## git工作流

每个节点开发的每个阶段都需要进行一次git保存