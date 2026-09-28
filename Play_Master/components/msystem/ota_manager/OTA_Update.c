// /**
//  * @file OTA_Update.c
//  * @brief OTA 远程升级模块实现（局域网 Pull-style OTA）
//  *
//  * 流程：
//  *   1. GET /ota/check 获取 JSON {version, sha256, url}
//  *   2. 解析并语义化比较版本号
//  *   3. 远端版本更新时，通过 esp_https_ota_begin/perform/finish 流式写入备用分区
//  *   4. 下载期间通过 esp_http_client 事件回调计算数据流 SHA256
//  *   5. 校验通过后切换启动分区并 esp_restart()
//  *
//  * 模块不直接依赖 WiFi/Audio/AI Cloud，只通过事件回调通知上层，
//  * 上层可在 DOWNLOAD_STARTED 时暂停高内存/高带宽任务。
//  */

// #include "OTA_Update.h"

// #include <string.h>
// #include <stdio.h>
// #include <stdlib.h>
// #include <ctype.h>

// #include "freertos/FreeRTOS.h"
// #include "freertos/task.h"
// #include "freertos/semphr.h"

// #include "esp_log.h"
// #include "esp_system.h"
// #include "esp_http_client.h"
// #include "esp_https_ota.h"
// #include "esp_ota_ops.h"
// #include "nvs.h"

// #include "cJSON.h"
// #include "mbedtls/sha256.h"

// #define TAG "OTA_UPDATE"

// /** /ota/check JSON 响应接收缓冲区大小 */
// #define OTA_CHECK_BUF_SIZE       1024
// /** esp_http_client 接收缓冲区大小 */
// #define OTA_HTTP_RX_BUFFER_SIZE  1024
// /** 版本号最大比较段数 */
// #define OTA_VERSION_COMPONENTS   4
// /** 后台任务失败/无需升级后的重试间隔（毫秒） */
// #define OTA_RETRY_INTERVAL_MS    30000

// /* NVS 约定：服务器地址由 node_role 统一持久化，键名与命名空间保持一致 */
// #define OTA_NVS_NAMESPACE        "node_role"
// #define OTA_NVS_KEY_OTA_URL      "ota_server_url"

// /* ======================== 内部类型定义 =========================================== */

// /**
//  * @brief 远端固件元数据
//  */
// typedef struct
// {
//     char version[OTA_VERSION_MAX_LEN];
//     char sha256[OTA_SHA256_MAX_LEN];
//     char url[OTA_URL_MAX_LEN];
// } ota_update_meta_t;

// /**
//  * @brief 下载过程中的 SHA256 计算上下文
//  */
// typedef struct
// {
//     mbedtls_sha256_context sha_ctx; /**< mbedtls SHA256 上下文 */
//     size_t received_bytes;          /**< 已接收数据字节数（用于日志/调试） */
// } ota_download_ctx_t;

// /**
//  * @brief OTA 模块运行时状态
//  */
// typedef struct
// {
//     bool initialized;
//     char check_url[OTA_URL_MAX_LEN];
//     char local_version[OTA_VERSION_MAX_LEN];
//     uint32_t check_timeout_ms;
//     uint32_t download_timeout_ms;
//     ota_update_event_cb_t event_cb;
//     void *event_user_ctx;

//     char remote_version[OTA_VERSION_MAX_LEN];
//     char remote_sha256[OTA_SHA256_MAX_LEN];
//     char remote_url[OTA_URL_MAX_LEN];

//     volatile bool busy;
// } ota_runtime_t;

// static ota_runtime_t s_rt;
// static SemaphoreHandle_t s_lock = NULL;
// static SemaphoreHandle_t s_sync_done = NULL;
// static volatile esp_err_t s_sync_result = ESP_OK;

// /* ======================== 内部工具函数 =========================================== */

// static void ota_lock(void)
// {
//     if (s_lock != NULL)
//     {
//         xSemaphoreTake(s_lock, portMAX_DELAY);
//     }
// }

// static void ota_unlock(void)
// {
//     if (s_lock != NULL)
//     {
//         xSemaphoreGive(s_lock);
//     }
// }

// /**
//  * @brief 安全拷贝字符串，目标缓冲区不足时自动截断并补结束符
//  */
// static void ota_str_copy(char *dst, size_t dst_size, const char *src)
// {
//     if (dst == NULL || dst_size == 0)
//     {
//         return;
//     }

//     if (src == NULL)
//     {
//         dst[0] = '\0';
//         return;
//     }

//     strncpy(dst, src, dst_size - 1);
//     dst[dst_size - 1] = '\0';
// }

// /**
//  * @brief 从 NVS 读取 OTA 服务器地址
//  *
//  * 服务器地址由 node_role 统一持久化（首次开机按默认值落盘）；
//  * 本模块按约定键名直接读取，NVS 未初始化或键不存在时返回 false，由调用方回退默认值。
//  */
// static bool ota_load_check_url_from_nvs(char *buf, size_t buf_size)
// {
//     if (buf == NULL || buf_size == 0)
//     {
//         return false;
//     }

//     nvs_handle_t handle;
//     esp_err_t err = nvs_open(OTA_NVS_NAMESPACE, NVS_READONLY, &handle);
//     if (err != ESP_OK)
//     {
//         ESP_LOGD(TAG, "NVS open failed: %s", esp_err_to_name(err));
//         return false;
//     }

//     size_t len = buf_size;
//     err = nvs_get_str(handle, OTA_NVS_KEY_OTA_URL, buf, &len);
//     nvs_close(handle);

//     if (err != ESP_OK || len == 0 || buf[0] == '\0')
//     {
//         ESP_LOGD(TAG, "OTA URL not found in NVS: %s", esp_err_to_name(err));
//         return false;
//     }

//     return true;
// }


// /**
//  * @brief 读取当前运行固件的版本号
//  */
// static esp_err_t ota_get_running_version(char *buf, size_t buf_size)
// {
//     const esp_partition_t *running = esp_ota_get_running_partition();
//     if (running == NULL)
//     {
//         return ESP_FAIL;
//     }

//     esp_app_desc_t desc;
//     esp_err_t err = esp_ota_get_partition_description(running, &desc);
//     if (err != ESP_OK)
//     {
//         return err;
//     }

//     ota_str_copy(buf, buf_size, desc.version);
//     return ESP_OK;
// }

// /**
//  * @brief 从版本字符串中解析数字段
//  *
//  * 支持 "1.0.0"、"v1.0.10"、"esp_v1.0.1.bin" 等格式：
//  * 函数会先跳过所有非数字字符，再从第一个数字开始解析。
//  */
// static bool ota_parse_version(const char *ver, uint32_t *parts, size_t *count)
// {
//     if (ver == NULL || parts == NULL || count == NULL)
//     {
//         return false;
//     }

//     const char *p = ver;
//     while (*p != '\0' && !isdigit((unsigned char)*p))
//     {
//         p++;
//     }

//     if (*p == '\0')
//     {
//         return false;
//     }

//     size_t n = 0;
//     while (n < OTA_VERSION_COMPONENTS)
//     {
//         char *end = NULL;
//         unsigned long value = strtoul(p, &end, 10);
//         if (end == p)
//         {
//             break;
//         }

//         parts[n++] = (uint32_t)value;
//         p = end;

//         if (*p != '.')
//         {
//             break;
//         }
//         p++;
//     }

//     *count = n;
//     return n > 0;
// }

// /**
//  * @brief 比较两个版本号
//  *
//  * @return -1：a < b；0：相等；1：a > b
//  * @note 任一侧无法解析时返回 0（无法判断时不升级）
//  */
// static int ota_compare_versions(const char *a, const char *b)
// {
//     uint32_t pa[OTA_VERSION_COMPONENTS] = {0};
//     uint32_t pb[OTA_VERSION_COMPONENTS] = {0};
//     size_t ca = 0;
//     size_t cb = 0;

//     if (!ota_parse_version(a, pa, &ca) || !ota_parse_version(b, pb, &cb))
//     {
//         return 0;
//     }

//     size_t max_components = ca > cb ? ca : cb;
//     for (size_t i = 0; i < max_components; i++)
//     {
//         if (pa[i] < pb[i])
//         {
//             return -1;
//         }
//         if (pa[i] > pb[i])
//         {
//             return 1;
//         }
//     }

//     return 0;
// }

// /**
//  * @brief 将二进制 SHA256 转为小写十六进制字符串
//  */
// static void ota_bytes_to_hex(const uint8_t *bytes, size_t len, char *out)
// {
//     if (bytes == NULL || out == NULL)
//     {
//         return;
//     }

//     for (size_t i = 0; i < len; i++)
//     {
//         sprintf(out + i * 2, "%02x", bytes[i]);
//     }
//     out[len * 2] = '\0';
// }

// /**
//  * @brief 不区分大小写比较两个十六进制 SHA256 字符串
//  */
// static bool ota_sha256_equal(const char *a, const char *b)
// {
//     if (a == NULL || b == NULL)
//     {
//         return false;
//     }

//     while (*a != '\0' && *b != '\0')
//     {
//         char ca = (char)tolower((unsigned char)*a);
//         char cb = (char)tolower((unsigned char)*b);
//         if (ca != cb)
//         {
//             return false;
//         }
//         a++;
//         b++;
//     }

//     return (*a == '\0') && (*b == '\0');
// }

// /* ======================== 事件通知 =============================================== */

// /**
//  * @brief 通知上层 OTA 状态变化
//  *
//  * @param event        事件类型
//  * @param progress     下载进度 0~100，其他事件传 -1
//  * @param http_status  HTTP 状态码，无 HTTP 上下文时传 0
//  */
// static void ota_notify(ota_update_event_t event, int progress, int http_status)
// {
//     ota_update_event_info_t info;
//     memset(&info, 0, sizeof(info));

//     info.event = event;
//     info.progress_percent = progress;
//     info.http_status = http_status;

//     ota_lock();
//     ota_str_copy(info.version, sizeof(info.version), s_rt.remote_version);
//     ota_str_copy(info.sha256, sizeof(info.sha256), s_rt.remote_sha256);
//     ota_str_copy(info.url, sizeof(info.url), s_rt.remote_url);

//     ota_update_event_cb_t cb = s_rt.event_cb;
//     void *ctx = s_rt.event_user_ctx;
//     ota_unlock();

//     if (cb != NULL)
//     {
//         cb(&info, ctx);
//     }
// }

// /* ======================== HTTP / JSON 处理 ======================================= */

// /**
//  * @brief HTTP GET 请求并将响应体读入缓冲区
//  *
//  * @param url         完整 URL
//  * @param timeout_ms  超时时间
//  * @param buf         接收缓冲区
//  * @param buf_size    接收缓冲区大小
//  * @param out_status  输出 HTTP 状态码
//  */
// static esp_err_t ota_http_get_to_buffer(const char *url, uint32_t timeout_ms,
//                                         char *buf, size_t buf_size, int *out_status)
// {
//     if (url == NULL || buf == NULL || buf_size < 2 || out_status == NULL)
//     {
//         return ESP_ERR_INVALID_ARG;
//     }

//     esp_http_client_config_t config = {
//         .url = url,
//         .method = HTTP_METHOD_GET,
//         .timeout_ms = (int)timeout_ms,
//         .buffer_size = OTA_HTTP_RX_BUFFER_SIZE,
//         .buffer_size_tx = 256,
//         .disable_auto_redirect = false,
//         .max_redirection_count = 3,
//     };

//     esp_http_client_handle_t client = esp_http_client_init(&config);
//     if (client == NULL)
//     {
//         ESP_LOGE(TAG, "esp_http_client_init failed");
//         return ESP_FAIL;
//     }

//     esp_err_t err = esp_http_client_open(client, 0);
//     if (err != ESP_OK)
//     {
//         ESP_LOGE(TAG, "HTTP open failed: %s", esp_err_to_name(err));
//         goto cleanup;
//     }

//     int content_length = esp_http_client_fetch_headers(client);
//     *out_status = esp_http_client_get_status_code(client);

//     if (*out_status != 200)
//     {
//         ESP_LOGE(TAG, "HTTP status error: %d", *out_status);
//         err = ESP_FAIL;
//         goto cleanup;
//     }

//     if (content_length >= 0 && (size_t)content_length >= buf_size)
//     {
//         ESP_LOGE(TAG, "Response too large: %d bytes, buffer=%u", content_length, (unsigned)buf_size);
//         err = ESP_ERR_NO_MEM;
//         goto cleanup;
//     }

//     size_t total = 0;
//     while ((total + 1) < buf_size)
//     {
//         int read_len = esp_http_client_read(client, buf + total, (int)(buf_size - 1 - total));
//         if (read_len < 0)
//         {
//             ESP_LOGE(TAG, "HTTP read failed: %d", read_len);
//             err = ESP_FAIL;
//             goto cleanup;
//         }

//         if (read_len == 0)
//         {
//             break;
//         }

//         total += (size_t)read_len;
//     }

//     buf[total] = '\0';
//     err = ESP_OK;

// cleanup:
//     esp_http_client_close(client);
//     esp_http_client_cleanup(client);
//     return err;
// }

// /**
//  * @brief 解析 /ota/check 返回的 JSON
//  */
// static esp_err_t ota_parse_check_response(const char *buf, ota_update_meta_t *meta)
// {
//     if (buf == NULL || meta == NULL)
//     {
//         return ESP_ERR_INVALID_ARG;
//     }

//     cJSON *root = cJSON_Parse(buf);
//     if (root == NULL)
//     {
//         ESP_LOGE(TAG, "Invalid JSON: %s", buf);
//         return ESP_FAIL;
//     }

//     const cJSON *version = cJSON_GetObjectItemCaseSensitive(root, "version");
//     const cJSON *sha256 = cJSON_GetObjectItemCaseSensitive(root, "sha256");
//     const cJSON *url = cJSON_GetObjectItemCaseSensitive(root, "url");

//     esp_err_t err = ESP_FAIL;
//     if (cJSON_IsString(version) && version->valuestring != NULL &&
//         cJSON_IsString(sha256) && sha256->valuestring != NULL &&
//         cJSON_IsString(url) && url->valuestring != NULL)
//     {
//         ota_str_copy(meta->version, sizeof(meta->version), version->valuestring);
//         ota_str_copy(meta->sha256, sizeof(meta->sha256), sha256->valuestring);
//         ota_str_copy(meta->url, sizeof(meta->url), url->valuestring);
//         err = ESP_OK;
//     }
//     else
//     {
//         ESP_LOGE(TAG, "Missing version/sha256/url in response");
//     }

//     cJSON_Delete(root);
//     return err;
// }

// /**
//  * @brief 执行一次 /ota/check 请求
//  */
// static esp_err_t ota_check_for_update(ota_update_meta_t *out_meta, int *out_http_status)
// {
//     if (out_meta == NULL || out_http_status == NULL)
//     {
//         return ESP_ERR_INVALID_ARG;
//     }

//     char json_buf[OTA_CHECK_BUF_SIZE] = {0};
//     int http_status = 0;

//     esp_err_t err = ota_http_get_to_buffer(s_rt.check_url, s_rt.check_timeout_ms,
//                                            json_buf, sizeof(json_buf), &http_status);
//     *out_http_status = http_status;

//     if (err != ESP_OK)
//     {
//         return err;
//     }

//     return ota_parse_check_response(json_buf, out_meta);
// }

// /* ======================== OTA 下载与升级 ========================================= */

// /**
//  * @brief esp_http_client 事件回调
//  *
//  * esp_https_ota 内部通过 esp_http_client_read() 读取数据，该读取过程仍会
//  * 触发 HTTP_EVENT_ON_DATA 事件。这里借此机会对固件数据流计算 SHA256。
//  */
// static esp_err_t ota_http_event_handler(esp_http_client_event_t *evt)
// {
//     ota_download_ctx_t *ctx = (ota_download_ctx_t *)evt->user_data;
//     if (ctx == NULL)
//     {
//         return ESP_OK;
//     }

//     switch (evt->event_id)
//     {
//         case HTTP_EVENT_ON_DATA:
//             if (evt->data != NULL && evt->data_len > 0)
//             {
//                 mbedtls_sha256_update(&ctx->sha_ctx, (const unsigned char *)evt->data, evt->data_len);
//                 ctx->received_bytes += (size_t)evt->data_len;
//             }
//             break;

//         case HTTP_EVENT_ERROR:
//             ESP_LOGE(TAG, "HTTP download error");
//             break;

//         default:
//             break;
//     }

//     return ESP_OK;
// }

// /**
//  * @brief 下载固件、校验 SHA256、切换启动分区
//  */
// static esp_err_t ota_download_and_apply(const ota_update_meta_t *meta)
// {
//     ota_download_ctx_t dl_ctx;
//     memset(&dl_ctx, 0, sizeof(dl_ctx));
//     mbedtls_sha256_init(&dl_ctx.sha_ctx);
//     mbedtls_sha256_starts(&dl_ctx.sha_ctx, 0);

//     esp_http_client_config_t http_config = {
//         .url = meta->url,
//         .method = HTTP_METHOD_GET,
//         .timeout_ms = (int)s_rt.download_timeout_ms,
//         .buffer_size = OTA_HTTP_RX_BUFFER_SIZE,
//         .buffer_size_tx = 256,
//         .keep_alive_enable = true,
//         .event_handler = ota_http_event_handler,
//         .user_data = &dl_ctx,
//     };

//     esp_https_ota_config_t ota_config = {
//         .http_config = &http_config,
//     };

//     esp_https_ota_handle_t handle = NULL;
//     esp_err_t err = esp_https_ota_begin(&ota_config, &handle);
//     if (err != ESP_OK)
//     {
//         ESP_LOGE(TAG, "esp_https_ota_begin failed: %s", esp_err_to_name(err));
//         return err;
//     }

//     int image_size = esp_https_ota_get_image_size(handle);
//     if (image_size > 0)
//     {
//         ESP_LOGI(TAG, "OTA image size: %d bytes", image_size);
//     }

//     ota_notify(OTA_UPDATE_EVT_DOWNLOAD_STARTED, 0, 0);

//     int last_progress = -1;
//     do
//     {
//         err = esp_https_ota_perform(handle);
//         if (err != ESP_OK && err != ESP_ERR_HTTPS_OTA_IN_PROGRESS)
//         {
//             break;
//         }

//         int len_read = esp_https_ota_get_image_len_read(handle);
//         int progress = 0;
//         if (image_size > 0)
//         {
//             progress = (int)(((int64_t)len_read * 100) / image_size);
//             if (progress < 0)
//             {
//                 progress = 0;
//             }
//             if (progress > 100)
//             {
//                 progress = 100;
//             }
//         }

//         if (progress != last_progress)
//         {
//             last_progress = progress;
//             ota_notify(OTA_UPDATE_EVT_DOWNLOAD_PROGRESS, progress, 0);
//         }
//     } while (err == ESP_ERR_HTTPS_OTA_IN_PROGRESS);

//     if (err != ESP_OK || !esp_https_ota_is_complete_data_received(handle))
//     {
//         ESP_LOGE(TAG, "OTA download incomplete, err=%s", esp_err_to_name(err));
//         esp_https_ota_abort(handle);
//         return (err == ESP_OK) ? ESP_FAIL : err;
//     }

//     uint8_t computed_hash[32] = {0};
//     mbedtls_sha256_finish(&dl_ctx.sha_ctx, computed_hash);

//     if (meta->sha256[0] != '\0')
//     {
//         char computed_hex[OTA_SHA256_MAX_LEN] = {0};
//         ota_bytes_to_hex(computed_hash, sizeof(computed_hash), computed_hex);

//         if (!ota_sha256_equal(computed_hex, meta->sha256))
//         {
//             ESP_LOGE(TAG, "SHA256 mismatch: expected=%s computed=%s", meta->sha256, computed_hex);
//             esp_https_ota_abort(handle);
//             return ESP_ERR_INVALID_RESPONSE;
//         }

//         ESP_LOGI(TAG, "SHA256 verification passed: %s", computed_hex);
//     }
//     else
//     {
//         ESP_LOGW(TAG, "Server did not provide sha256, skipping verification");
//     }

//     err = esp_https_ota_finish(handle);
//     if (err != ESP_OK)
//     {
//         ESP_LOGE(TAG, "esp_https_ota_finish failed: %s", esp_err_to_name(err));
//         return err;
//     }

//     ota_notify(OTA_UPDATE_EVT_DOWNLOAD_FINISHED, 100, 0);
//     ESP_LOGI(TAG, "OTA firmware downloaded and validated");
//     return ESP_OK;
// }

// /* ======================== 核心检测升级流程 ======================================= */

// /**
//  * @brief 执行一次完整的“检测-比较-下载-校验-重启”流程
//  */
// static esp_err_t ota_check_and_upgrade(void)
// {
//     ota_notify(OTA_UPDATE_EVT_CHECK_STARTED, -1, 0);

//     ota_update_meta_t remote;
//     memset(&remote, 0, sizeof(remote));

//     int http_status = 0;
//     esp_err_t err = ota_check_for_update(&remote, &http_status);
//     if (err != ESP_OK)
//     {
//         ESP_LOGW(TAG, "OTA check failed: %s", esp_err_to_name(err));
//         ota_notify(OTA_UPDATE_EVT_ABORTED, -1, http_status);
//         return err;
//     }

//     ota_lock();
//     ota_str_copy(s_rt.remote_version, sizeof(s_rt.remote_version), remote.version);
//     ota_str_copy(s_rt.remote_sha256, sizeof(s_rt.remote_sha256), remote.sha256);
//     ota_str_copy(s_rt.remote_url, sizeof(s_rt.remote_url), remote.url);
//     char local_version[OTA_VERSION_MAX_LEN] = {0};
//     ota_str_copy(local_version, sizeof(local_version), s_rt.local_version);
//     ota_unlock();

//     ESP_LOGI(TAG, "Remote firmware: version=%s, sha256=%s, url=%s",
//              remote.version, remote.sha256, remote.url);

//     int cmp = ota_compare_versions(local_version, remote.version);
//     if (cmp >= 0)
//     {
//         ESP_LOGI(TAG, "Firmware is up to date (local=%s remote=%s)", local_version, remote.version);
//         ota_notify(OTA_UPDATE_EVT_UP_TO_DATE, -1, http_status);
//         return ESP_OK;
//     }

//     ESP_LOGI(TAG, "New firmware available: %s -> %s", local_version, remote.version);
//     ota_notify(OTA_UPDATE_EVT_UPDATE_AVAILABLE, -1, http_status);

//     err = ota_download_and_apply(&remote);
//     if (err != ESP_OK)
//     {
//         ESP_LOGE(TAG, "OTA download/apply failed: %s", esp_err_to_name(err));
//         ota_notify(OTA_UPDATE_EVT_ABORTED, -1, 0);
//         return err;
//     }

//     ota_notify(OTA_UPDATE_EVT_READY_TO_REBOOT, 100, 0);
//     ESP_LOGI(TAG, "OTA success, rebooting ...");
//     vTaskDelay(pdMS_TO_TICKS(1000));
//     esp_restart();

//     return ESP_OK;
// }

// /* ======================== 后台任务 =============================================== */

// static void ota_update_task(void *pvParameters)
// {
//     ESP_LOGI(TAG, "OTA background task started");

//     for (;;)
//     {
//         esp_err_t err = ota_check_and_upgrade();

//         if (err == ESP_OK)
//         {
//             ESP_LOGI(TAG, "OTA check finished, retry after %d ms", OTA_RETRY_INTERVAL_MS);
//         }
//         else
//         {
//             ESP_LOGI(TAG, "OTA check error 0x%x, retry after %d ms", err, OTA_RETRY_INTERVAL_MS);
//         }

//         vTaskDelay(pdMS_TO_TICKS(OTA_RETRY_INTERVAL_MS));
//     }
// }

// /* ======================== 公共 API 实现 ========================================== */

// esp_err_t OTA_Update_Init(const ota_update_config_t *cfg)
// {
//     if (s_lock == NULL)
//     {
//         s_lock = xSemaphoreCreateMutex();
//         if (s_lock == NULL)
//         {
//             return ESP_ERR_NO_MEM;
//         }
//     }

//     ota_lock();

//     memset(&s_rt, 0, sizeof(s_rt));

//     if (cfg != NULL && cfg->check_url != NULL && cfg->check_url[0] != '\0')
//     {
//         ota_str_copy(s_rt.check_url, sizeof(s_rt.check_url), cfg->check_url);
//     }
//     else if (ota_load_check_url_from_nvs(s_rt.check_url, sizeof(s_rt.check_url)))
//     {
//         ESP_LOGI(TAG, "OTA check_url loaded from NVS");
//     }
//     else
//     {
//         ota_str_copy(s_rt.check_url, sizeof(s_rt.check_url), OTA_SERVER_CHECK_URL);
//     }

//     s_rt.check_timeout_ms = (cfg != NULL && cfg->check_timeout_ms > 0) ? cfg->check_timeout_ms : OTA_CHECK_TIMEOUT_MS;
//     s_rt.download_timeout_ms = (cfg != NULL && cfg->download_timeout_ms > 0) ? cfg->download_timeout_ms : OTA_DOWNLOAD_TIMEOUT_MS;

//     if (cfg != NULL && cfg->local_version != NULL && cfg->local_version[0] != '\0')
//     {
//         ota_str_copy(s_rt.local_version, sizeof(s_rt.local_version), cfg->local_version);
//     }
//     else if (ota_get_running_version(s_rt.local_version, sizeof(s_rt.local_version)) != ESP_OK)
//     {
//         ota_str_copy(s_rt.local_version, sizeof(s_rt.local_version), OTA_LOCAL_VERSION);
//     }

//     s_rt.event_cb = (cfg != NULL) ? cfg->event_cb : NULL;
//     s_rt.event_user_ctx = (cfg != NULL) ? cfg->event_user_ctx : NULL;
//     s_rt.busy = false;
//     s_rt.initialized = true;

//     ota_unlock();

//     ESP_LOGI(TAG, "OTA module initialized: check_url=%s local_version=%s",
//              s_rt.check_url, s_rt.local_version);
//     return ESP_OK;
// }

// esp_err_t OTA_Update_RegisterEventCallback(ota_update_event_cb_t cb, void *user_ctx)
// {
//     if (!s_rt.initialized)
//     {
//         return ESP_ERR_INVALID_STATE;
//     }

//     ota_lock();
//     s_rt.event_cb = cb;
//     s_rt.event_user_ctx = user_ctx;
//     ota_unlock();
//     return ESP_OK;
// }

// esp_err_t OTA_Update_Start(void)
// {
//     if (!s_rt.initialized)
//     {
//         esp_err_t err = OTA_Update_Init(NULL);
//         if (err != ESP_OK)
//         {
//             return err;
//         }
//     }

//     BaseType_t ret = xTaskCreatePinnedToCore(ota_update_task, "ota_update",
//                                  OTA_TASK_STACK_SIZE, NULL,
//                                  OTA_TASK_PRIORITY, NULL, 0);
//     if (ret != pdPASS)
//     {
//         ESP_LOGE(TAG, "Failed to create OTA task");
//         return ESP_FAIL;
//     }

//     return ESP_OK;
// }

// /**
//  * @brief 同步 OTA 检测专用任务
//  *
//  * OTA 下载过程会占用较大栈空间，因此同步接口也放到独立任务中执行，
//  * 避免在 main 任务等小栈任务中直接运行导致栈溢出。
//  */
// static void ota_sync_task(void *pvParameters)
// {
//     esp_err_t err = ota_check_and_upgrade();

//     ota_lock();
//     s_sync_result = err;
//     s_rt.busy = false;
//     ota_unlock();

//     if (s_sync_done != NULL)
//     {
//         xSemaphoreGive(s_sync_done);
//     }

//     vTaskDelete(NULL);
// }

// esp_err_t OTA_Update_CheckAndUpgrade(void)
// {
//     if (!s_rt.initialized)
//     {
//         esp_err_t err = OTA_Update_Init(NULL);
//         if (err != ESP_OK)
//         {
//             return err;
//         }
//     }

//     if (s_sync_done == NULL)
//     {
//         s_sync_done = xSemaphoreCreateBinary();
//         if (s_sync_done == NULL)
//         {
//             return ESP_ERR_NO_MEM;
//         }
//     }

//     ota_lock();
//     if (s_rt.busy)
//     {
//         ota_unlock();
//         return ESP_ERR_INVALID_STATE;
//     }
//     s_rt.busy = true;
//     ota_unlock();

//     BaseType_t ret = xTaskCreatePinnedToCore(ota_sync_task, "ota_sync",
//                                  OTA_TASK_STACK_SIZE, NULL,
//                                  OTA_TASK_PRIORITY, NULL, 0);
//     if (ret != pdPASS)
//     {
//         ESP_LOGE(TAG, "Failed to create OTA sync task");
//         ota_lock();
//         s_rt.busy = false;
//         ota_unlock();
//         return ESP_FAIL;
//     }

//     // 等待 OTA 专用任务执行完成；若发现新固件，任务内部会直接重启设备
//     xSemaphoreTake(s_sync_done, portMAX_DELAY);

//     ota_lock();
//     esp_err_t err = s_sync_result;
//     ota_unlock();

//     return err;
// }

// esp_err_t OTA_Update_MarkAppValid(void)
// {
// #if CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
//     const esp_partition_t *running = esp_ota_get_running_partition();
//     if (running == NULL)
//     {
//         return ESP_FAIL;
//     }

//     esp_ota_img_states_t state;
//     esp_err_t err = esp_ota_get_state_partition(running, &state);
//     if (err == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY)
//     {
//         err = esp_ota_mark_app_valid_cancel_rollback();
//         if (err == ESP_OK)
//         {
//             ESP_LOGI(TAG, "App marked valid, rollback cancelled");
//         }
//         else
//         {
//             ESP_LOGE(TAG, "Failed to mark app valid: %s", esp_err_to_name(err));
//         }
//         return err;
//     }

//     return ESP_OK;
// #else
//     ESP_LOGD(TAG, "Bootloader app rollback is disabled, skip marking app valid");
//     return ESP_OK;
// #endif
// }

// const char *OTA_Update_GetLocalVersion(void)
// {
//     return s_rt.local_version;
// }

// const char *OTA_Update_GetRemoteVersion(void)
// {
//     return s_rt.remote_version;
// }

// bool OTA_Update_IsBusy(void)
// {
//     bool busy = false;
//     ota_lock();
//     busy = s_rt.busy;
//     ota_unlock();
//     return busy;
// }