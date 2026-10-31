/**
  * @file    mqtt_link.c
  * @brief   MQTT 链路 —— 连巴法云 broker、订阅主题、收发消息
  *
  * 对外接口、两个发布口的区别、以及"stop() 为什么不能真停"
  * 都写在 mqtt_link.h 里，这里只记实现上不能动的地方。
  */

#include <stdio.h>
#include <string.h>

/* SDK 的 esp-mqtt。本模块的接口头文件叫 mqtt_link.h，就是为了不跟它撞名 ——
   两边都叫 mqtt_client.h 的话，这一行拿到的是谁全看编译器先搜到哪个目录。 */
#include "mqtt_client.h"

#include "mqtt_link.h"

/* ==================== 连接参数：要改的就是这几行 ==================== */

/**
  * @brief broker 地址
  *
  * @note  格式是 <协议>://<域名>:<端口>。协议头决定走哪种传输 ——
  *        "mqtt" 是明文 TCP（走 SDK 里那个 "mqtt" transport），
  *        "mqtts" 才是 TLS。巴法云的 9501 是明文口，所以这里不能写 mqtts。
  */
#define MQTT_BROKER_URI   "mqtt://mqtt.bemfa.com:9501"

/**
  * @brief 客户端 ID
  *
  * @note  巴法云后台"设备云 → MQTT 设备"里那串。它同时也是 broker 认人的
  *        依据：两个设备用同一个 ID 连上去，后连的会把先连的顶掉。
  */
#define MQTT_CLIENT_ID    "60f1b4f920fc0abd0d06c7037b5ca066"

/**
  * @brief 用户名 / 密码 —— 现在是空的，【预留给将来】
  *
  * @note  为什么是 NULL 而不是 ""：空字符串会让 CONNECT 报文里带上一段
  *        长度为 0 的用户名，有的 broker 会直接拒绝；NULL 才是"这个字段
  *        干脆不带"。
  *
  * @note  将来要填，把 NULL 换成字符串即可，别处不用动：
  *          #define MQTT_USERNAME   "我的用户名"
  */
#define MQTT_USERNAME     NULL
#define MQTT_PASSWORD     NULL

/**
  * @brief 订阅的主题
  *
  * @note  下面两个发布口就是在这个名字后面接 "/up" 和 "/set" ——
  *        用字符串拼接写，改主题时只要改这一行。巴法云的规矩是"主题名"
  *        本身不带斜杠，斜杠是发布时加的。
  */
#define MQTT_TOPIC        "kChtBO89U002"

/** @brief 推给所有订阅者的发布口 */
#define MQTT_TOPIC_UP     MQTT_TOPIC "/up"

/** @brief 只更新云端值的发布口 */
#define MQTT_TOPIC_SET    MQTT_TOPIC "/set"

/**
  * @brief 收发的服务质量，0 = 最多一次
  *
  * @note  巴法云自己的例程用的就是 QoS 0（订阅和发布都是），跟着来最稳。
  *        升到 1 的话 broker 会重发，我们这边就得多处理"同一条命令到两次"，
  *        而开关灯是幂等的，多一次也无所谓 —— 收益不值这个复杂度。
  */
#define MQTT_QOS          0

/**
  * @brief 心跳间隔（秒）
  *
  * @note  空闲超过它的一半（30 秒）就发一次 PINGREQ，收不到应答就判定断线。
  *        设备躲在 NAT 后面时，这个间隔也是那条映射的保鲜期。
  */
#define MQTT_KEEPALIVE_S  60

/**
  * @brief    收发缓冲区大小（字节），收发各一份
  *
  * @note     它要装下【整个报文】：主题 + 内容 + 报头。我们的主题 12 字节、
  *           命令 2~4 字节，512 绰绰有余，比默认的 1024 省一半内存。
  *
  * @warning  超过它长度的消息发不出去（建报文时直接失败）、也收不全
  *           （会被拆成几段，见下面 MQTT_EVENT_DATA 的处理）。
  */
#define MQTT_BUFFER_SIZE  512

/**
  * @brief 单条消息最多攒多少字节
  *
  * @note  比收发缓冲区小：命令只有几个字节，攒到 128 还没完说明对方发来的
  *        不是命令，没必要继续占内存。
  */
#define MQTT_RX_BUF_SIZE  128

/**
  * @brief 断线之后隔多久重连（毫秒）
  *
  * @note  这一条只对"连上之后掉线"生效；开机时连不上由 esp-mqtt 自己
  *        按同样的间隔一直重试。
  */
#define MQTT_RECONNECT_MS 10000

/* ======================= 模块状态 ======================= */

/** @brief esp-mqtt 的客户端句柄；NULL = 还没 init 过 */
static esp_mqtt_client_handle_t s_client = NULL;

/** @brief 收到消息时往哪儿送。由 mqtt_link_set_rx_handler() 填。 */
static mqtt_rx_handler_t s_rx_handler = NULL;

/**
  * @brief    本模块当前是不是"在用"
  *
  * @note     只由 start() / stop() 改。false 的时候：收进来的消息不往业务层
  *           送，要发出去的字节直接丢弃。连接本身还在（原因见 mqtt_link.h）。
  */
static volatile bool s_running = false;

/**
  * @brief    和 broker 之间现在是通的吗
  *
  * @note     只能由事件回调改 —— 它是唯一知道真相的地方。发送前看它一眼，
  *           是为了打出一句诚实的日志，而不是靠 esp_mqtt_client_publish()
  *           自己把消息默默丢掉。
  */
static volatile bool s_connected = false;

/**
  * @brief esp_mqtt_client_start() 调过没有
  *
  * @note  有了它 start() 才是幂等的：esp_mqtt_client_start() 第二次调用会
  *        报 "Client has started" 并返回失败，那不是错误，是"已经在跑了"。
  */
static bool s_started = false;

/** @brief 攒一条消息用。超过单条上限的消息会被 esp-mqtt 拆成几段送进来。 */
static char s_rx_buf[MQTT_RX_BUF_SIZE + 1];

/** @brief s_rx_buf 里当前攒了多少字节 */
static int  s_rx_len = 0;

/* ======================= 内部实现 ======================= */

/**
  * @brief    往指定主题发布一段字节
  *
  * 🟡 L1 —— 架构：两个发布口共用的落点。没连上就丢，返回 -1；
  *           成功返回【字节数】而不是 esp-mqtt 那个 msg_id。
  *
  * @param[in] topic  发到哪个主题
  * @param[in] data   要发的数据
  * @param[in] len    字节数；传 0 表示自己算长度
  *
  * @return   发出去的字节数；失败返回 -1
  *
  * @note     为什么返回字节数、不返回 esp_mqtt_client_publish() 给的 msg_id：
  *           本项目另外两个传输模块的 send() 都返回字节数（link_send() 按
  *           这个约定用），这里跟齐。再说 QoS 0 的 msg_id 恒为 0，给出去
  *           也没信息量。
  */
static int mqtt_publish(const char *topic, const char *data, int len)
{
    if (len == 0) {
        len = strlen(data);
    }

    if (!s_running) {
        printf("[mqtt] 未启用，%d 字节被丢弃\n", len);
        return -1;
    }
    if (!s_connected) {
        printf("[mqtt] 还没连上 broker，%d 字节被丢弃\n", len);
        return -1;
    }

    /* 返回的是 msg_id：QoS 0 时成功恒为 0，失败为 -1。
       ⚠ 所以判据是 "< 0"，不是 "== 0"。 */
    if (esp_mqtt_client_publish(s_client, topic, data, len, MQTT_QOS, 0) < 0) {
        printf("[mqtt] 发布失败 topic=%s\n", topic);
        return -1;
    }
    return len;
}

/**
  * @brief    esp-mqtt 的事件回调 —— 连接状态、订阅结果、收到消息都从这儿来
  *
  * 🟡 L1 —— 架构：它的每一行都跑在【MQTT 自己的任务】上，不能阻塞、不能
  *           在这里停客户端（理由见 mqtt_link_stop()）。
  *
  * @param[in] event  事件内容
  *
  * @retval    ESP_OK  一律返回它；esp-mqtt 不看别的
  *
  * @note      能在这个回调里直接调 subscribe()/publish()（本文件就调了），
  *            是因为 esp-mqtt 的内部 API 锁是【递归锁】，同一条任务重复
  *            获取不会死锁。换一把普通互斥锁，这个设计立刻就是死锁。
  *
  * @warning   esp_mqtt_client_stop() 是唯一不能在这里碰的 API ——
  *            它自己也用了那把递归锁，却在报错返回前忘了放掉。
  */
static esp_err_t mqtt_event_handler(esp_mqtt_event_handle_t event)
{
    switch (event->event_id) {

    case MQTT_EVENT_CONNECTED:
        s_connected = true;
        printf("[mqtt] 已连上 %s\n", MQTT_BROKER_URI);

        /* ⚠ 每一次连上都要重新订阅，不能只在开机时订一次。
               clean session 默认是开的，broker 不会替我们记着订阅关系 ——
               掉线重连之后不重订，现象就是"日志说连上了，但发消息进来没反应"。 */
        esp_mqtt_client_subscribe(s_client, MQTT_TOPIC, MQTT_QOS);
        break;

    case MQTT_EVENT_DISCONNECTED:
        s_connected = false;
        printf("[mqtt] 连接断开，esp-mqtt 会按 %d 秒的间隔重连\n",
               MQTT_RECONNECT_MS / 1000);
        break;

    case MQTT_EVENT_SUBSCRIBED:
        printf("[mqtt] 订阅成功: %s\n", MQTT_TOPIC);
        break;

    case MQTT_EVENT_DATA:
        /* 比接收缓冲区还长的消息会被 esp-mqtt 拆成几段依次送进来：
           第一段带主题，后面几段只有数据，靠 current_data_offset 认出"这是新的一条"。 */
        if (event->current_data_offset == 0) {
            s_rx_len = 0;
        }

        if (s_rx_len + event->data_len > MQTT_RX_BUF_SIZE) {
            printf("[mqtt] 消息太长（%d 字节），丢弃\n", event->total_data_len);
            s_rx_len = 0;
            break;
        }

        if (event->data_len > 0) {
            memcpy(s_rx_buf + s_rx_len, event->data, event->data_len);
            s_rx_len += event->data_len;
        }

        /* 还没收全就等下一段。分段时只有第一段带主题，所以日志只在收全时打。 */
        if (s_rx_len < event->total_data_len) {
            break;
        }

        s_rx_buf[s_rx_len] = '\0';
        printf("[mqtt] 收到 %d 字节: %s\n", s_rx_len, s_rx_buf);

        /* 停用之后就不再往业务层送了。这一层和 cmd 那边的来源判断是两道独立的闸，
           哪一道先挡住都行 —— 多一道是故意的。 */
        if (s_running && s_rx_handler != NULL) {
            s_rx_handler(s_rx_buf, s_rx_len);
        }
        s_rx_len = 0;
        break;

    case MQTT_EVENT_ERROR:
        if (event->error_handle != NULL) {
            printf("[mqtt] 出错: type=%d", event->error_handle->error_type);
            if (event->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
                /* 这里最值得看的是 connect_return_code：
                     4 = 用户名密码不对   5 = 没授权（多半是 client ID 不认） */
                printf("，broker 拒绝连接 code=%d",
                       event->error_handle->connect_return_code);
            }
            printf("\n");
        }
        break;

    default:
        break;
    }

    return ESP_OK;
}

/* ======================= 对外接口 ======================= */
/* 接口契约都在 mqtt_link.h 里，这里不重复。 */

void mqtt_link_set_rx_handler(mqtt_rx_handler_t handler)
{
    s_rx_handler = handler;
}

void mqtt_link_start(void)
{
    if (s_client == NULL) {
        esp_mqtt_client_config_t cfg = { 0 };

        cfg.uri          = MQTT_BROKER_URI;
        cfg.client_id    = MQTT_CLIENT_ID;
        cfg.username     = MQTT_USERNAME;   /* NULL = 报文里不带这个字段 */
        cfg.password     = MQTT_PASSWORD;
        cfg.event_handle = mqtt_event_handler;

        /* 用例程里的写法把版本钉死，不吃菜单里的默认值 —— 换 SDK 版本时
           行为不会跟着变。 */
        cfg.protocol_ver = MQTT_PROTOCOL_V_3_1_1;

        cfg.keepalive    = MQTT_KEEPALIVE_S;
        cfg.buffer_size  = MQTT_BUFFER_SIZE;
        cfg.reconnect_timeout_ms = MQTT_RECONNECT_MS;

        s_client = esp_mqtt_client_init(&cfg);
        if (s_client == NULL) {
            printf("[mqtt] 初始化失败（多半是内存不够）\n");
            return;
        }
    }

    if (!s_started) {
        if (esp_mqtt_client_start(s_client) != ESP_OK) {
            printf("[mqtt] 启动失败\n");
            return;
        }
        s_started = true;
        printf("[mqtt] 启动，目标 %s\n", MQTT_BROKER_URI);
    }

    s_running = true;
}

void mqtt_link_stop(void)
{
    /* 就这一行 —— 不能顺手补一句 esp_mqtt_client_stop()，
       那会把整条链路弄废。两个理由写在 mqtt_link.h 上。 */
    s_running = false;
}

int mqtt_link_send(const char *data, int len)
{
    return mqtt_publish(MQTT_TOPIC_UP, data, len);
}

int mqtt_link_send_set(const char *data, int len)
{
    return mqtt_publish(MQTT_TOPIC_SET, data, len);
}
