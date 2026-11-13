/*
 * NimBLE central that connects to the Shimano wireless unit (e.g. "SWEN600 ..."),
 * reads Battery Service 0x180F / Battery Level 0x2A19 and subscribes to notifications.
 */
#include "ble_battery.h"

#include <stdio.h>
#include <string.h>
#include <sys/param.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "store/config/ble_store_config.h"

static const char *TAG = "ep8_ble";

#define UUID_BAS        0x180F
#define UUID_BATT_LEVEL 0x2A19
#define UUID_CCCD       0x2902

void ble_store_config_init(void);

static ep8_update_cb_t s_cb;
static uint8_t s_own_addr_type;
static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_svc_end;
static uint16_t s_val_handle;
static uint16_t s_cccd_handle;
static bool s_discovering;
static bool s_security_tried;
static int s_level = -1;
static ep8_state_t s_state = EP8_STATE_SCANNING;
static esp_timer_handle_t s_poll_timer;
static esp_timer_handle_t s_retry_timer;

/* Reconnect backoff: 0 (immediate) -> 1 s -> 2 s -> ... -> 30 s.
 * Reset once a battery value has been read successfully. */
#define RETRY_FIRST_MS 1000
#define RETRY_MAX_MS   30000
static uint32_t s_backoff_ms;

static void start_scan(void);
static void start_discovery(void);
static int gap_event(struct ble_gap_event *event, void *arg);

static void notify_ui(ep8_state_t st)
{
    s_state = st;
    if (s_cb) {
        s_cb(s_state, s_level);
    }
}

static void set_level(int level)
{
    if (level < 0 || level > 100) {
        ESP_LOGW(TAG, "ignoring out-of-range level %d", level);
        return;
    }
    s_level = level;
    s_backoff_ms = 0; /* link is healthy */
    ESP_LOGI(TAG, "battery=%d%%", level);
    notify_ui(EP8_STATE_CONNECTED);
}

/* Returns true if the error means we have to pair/encrypt first. */
static bool is_security_error(const struct ble_gatt_error *err)
{
    return err->status == BLE_HS_ATT_ERR(BLE_ATT_ERR_INSUFFICIENT_AUTHEN) ||
           err->status == BLE_HS_ATT_ERR(BLE_ATT_ERR_INSUFFICIENT_ENC) ||
           err->status == BLE_HS_ATT_ERR(BLE_ATT_ERR_INSUFFICIENT_KEY_SZ);
}

static bool handle_security_error(uint16_t conn, const struct ble_gatt_error *err)
{
    if (!is_security_error(err)) {
        return false;
    }
    if (s_security_tried) {
        ESP_LOGE(TAG, "access still denied after pairing (status=%d)", err->status);
        ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
        return true;
    }
    ESP_LOGI(TAG, "peer requires encryption, starting pairing");
    s_security_tried = true;
    s_discovering = false;
    notify_ui(EP8_STATE_PAIRING);
    int rc = ble_gap_security_initiate(conn);
    if (rc != 0) {
        ESP_LOGE(TAG, "security_initiate rc=%d", rc);
        ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
    }
    return true;
}

/* ---------- GATT client chain ---------- */

static int on_read(uint16_t conn, const struct ble_gatt_error *err, struct ble_gatt_attr *attr, void *arg)
{
    if (err->status == 0 && attr && OS_MBUF_PKTLEN(attr->om) >= 1) {
        uint8_t v;
        os_mbuf_copydata(attr->om, 0, 1, &v);
        set_level(v);
    } else if (err->status != 0 && !handle_security_error(conn, err)) {
        ESP_LOGW(TAG, "read failed status=%d", err->status);
    }
    return 0;
}

static int on_cccd_written(uint16_t conn, const struct ble_gatt_error *err, struct ble_gatt_attr *attr, void *arg)
{
    if (err->status == 0) {
        ESP_LOGI(TAG, "notifications enabled");
    } else if (!handle_security_error(conn, err)) {
        ESP_LOGW(TAG, "CCCD write failed status=%d (falling back to polling)", err->status);
    }
    return 0;
}

static void read_and_subscribe(uint16_t conn)
{
    s_discovering = false;
    int rc = ble_gattc_read(conn, s_val_handle, on_read, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "gattc_read rc=%d", rc);
    }
    if (s_cccd_handle) {
        static const uint8_t enable_notify[2] = {0x01, 0x00};
        rc = ble_gattc_write_flat(conn, s_cccd_handle, enable_notify, sizeof(enable_notify), on_cccd_written, NULL);
        if (rc != 0) {
            ESP_LOGE(TAG, "CCCD write rc=%d", rc);
        }
    } else {
        ESP_LOGW(TAG, "no CCCD found, polling only");
    }
    esp_timer_stop(s_poll_timer);
    esp_timer_start_periodic(s_poll_timer, (uint64_t)CONFIG_EP8_POLL_INTERVAL_S * 1000000ULL);
}

static int on_dsc(uint16_t conn, const struct ble_gatt_error *err, uint16_t chr_val_handle,
                  const struct ble_gatt_dsc *dsc, void *arg)
{
    if (err->status == 0) {
        if (ble_uuid_cmp(&dsc->uuid.u, BLE_UUID16_DECLARE(UUID_CCCD)) == 0) {
            s_cccd_handle = dsc->handle;
        }
    } else if (err->status == BLE_HS_EDONE) {
        read_and_subscribe(conn);
    } else if (!handle_security_error(conn, err)) {
        ESP_LOGW(TAG, "descriptor discovery status=%d", err->status);
        read_and_subscribe(conn);
    }
    return 0;
}

static int on_chr(uint16_t conn, const struct ble_gatt_error *err, const struct ble_gatt_chr *chr, void *arg)
{
    if (err->status == 0) {
        if (!s_val_handle) {
            s_val_handle = chr->val_handle;
        }
    } else if (err->status == BLE_HS_EDONE) {
        if (!s_val_handle) {
            ESP_LOGE(TAG, "Battery Level characteristic not found");
            ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
            return 0;
        }
        if (s_val_handle < s_svc_end) {
            ble_gattc_disc_all_dscs(conn, s_val_handle, s_svc_end, on_dsc, NULL);
        } else {
            read_and_subscribe(conn);
        }
    } else if (!handle_security_error(conn, err)) {
        ESP_LOGE(TAG, "chr discovery status=%d", err->status);
        ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
    }
    return 0;
}

static int on_svc(uint16_t conn, const struct ble_gatt_error *err, const struct ble_gatt_svc *svc, void *arg)
{
    static uint16_t start;
    if (err->status == 0) {
        start = svc->start_handle;
        s_svc_end = svc->end_handle;
    } else if (err->status == BLE_HS_EDONE) {
        if (!s_svc_end) {
            ESP_LOGE(TAG, "Battery Service not found");
            ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
            return 0;
        }
        ble_gattc_disc_chrs_by_uuid(conn, start, s_svc_end, BLE_UUID16_DECLARE(UUID_BATT_LEVEL), on_chr, NULL);
    } else if (!handle_security_error(conn, err)) {
        ESP_LOGE(TAG, "svc discovery status=%d", err->status);
        ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
    }
    return 0;
}

static void start_discovery(void)
{
    if (s_conn == BLE_HS_CONN_HANDLE_NONE || s_discovering) {
        return;
    }
    if (s_val_handle) { /* already discovered (e.g. retry after pairing) */
        read_and_subscribe(s_conn);
        return;
    }
    s_discovering = true;
    s_svc_end = 0;
    int rc = ble_gattc_disc_svc_by_uuid(s_conn, BLE_UUID16_DECLARE(UUID_BAS), on_svc, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "disc_svc rc=%d", rc);
        ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
    }
}

static void poll_timer_cb(void *arg)
{
    if (s_conn != BLE_HS_CONN_HANDLE_NONE && s_val_handle) {
        ble_gattc_read(s_conn, s_val_handle, on_read, NULL);
    }
}

/* ---------- GAP ---------- */

static bool parse_mac(const char *str, ble_addr_t *out)
{
    unsigned b[6];
    if (sscanf(str, "%x:%x:%x:%x:%x:%x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) {
        return false;
    }
    for (int i = 0; i < 6; i++) {
        out->val[5 - i] = (uint8_t)b[i]; /* NimBLE stores little-endian */
    }
    return true;
}

static bool is_target(const struct ble_gap_disc_desc *d)
{
    ble_addr_t want;
    if (CONFIG_EP8_TARGET_MAC[0] && parse_mac(CONFIG_EP8_TARGET_MAC, &want)) {
        return memcmp(want.val, d->addr.val, 6) == 0;
    }
    struct ble_hs_adv_fields f;
    if (ble_hs_adv_parse_fields(&f, d->data, d->length_data) != 0 || f.name == NULL) {
        return false;
    }
    size_t plen = strlen(CONFIG_EP8_TARGET_NAME_PREFIX);
    return f.name_len >= plen && memcmp(f.name, CONFIG_EP8_TARGET_NAME_PREFIX, plen) == 0;
}

static void start_scan(void)
{
    struct ble_gap_disc_params p = {
        .passive = 0, /* active scan so we also get the scan response (name) */
        .filter_duplicates = 1,
        .itvl = 0x50,
        .window = 0x30,
    };
    notify_ui(EP8_STATE_SCANNING);
    int rc = ble_gap_disc(s_own_addr_type, BLE_HS_FOREVER, &p, gap_event, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "ble_gap_disc rc=%d", rc);
    } else {
        ESP_LOGI(TAG, "scanning for '%s'...", CONFIG_EP8_TARGET_MAC[0] ? CONFIG_EP8_TARGET_MAC : CONFIG_EP8_TARGET_NAME_PREFIX);
    }
}

static void retry_timer_cb(void *arg)
{
    start_scan();
}

/* Restart scanning after a failed/lost connection, with exponential backoff
 * so we don't hammer the Shimano unit (e.g. while the phone app holds the link). */
static void schedule_rescan(void)
{
    uint32_t delay = s_backoff_ms;
    s_backoff_ms = delay ? MIN(delay * 2, RETRY_MAX_MS) : RETRY_FIRST_MS;

    notify_ui(EP8_STATE_SCANNING);
    if (delay == 0) {
        start_scan();
        return;
    }
    ESP_LOGI(TAG, "retrying in %lu ms", (unsigned long)delay);
    esp_timer_stop(s_retry_timer);
    esp_timer_start_once(s_retry_timer, (uint64_t)delay * 1000ULL);
}

static bool have_bond(const ble_addr_t *peer)
{
    struct ble_store_key_sec key = {.peer_addr = *peer};
    struct ble_store_value_sec val;
    return ble_store_read_peer_sec(&key, &val) == 0;
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_DISC:
        if (is_target(&event->disc)) {
            ESP_LOGI(TAG, "found target %02X:%02X:%02X:%02X:%02X:%02X, connecting",
                     event->disc.addr.val[5], event->disc.addr.val[4], event->disc.addr.val[3],
                     event->disc.addr.val[2], event->disc.addr.val[1], event->disc.addr.val[0]);
            ble_gap_disc_cancel();
            notify_ui(EP8_STATE_CONNECTING);
            int rc = ble_gap_connect(s_own_addr_type, &event->disc.addr, 30000, NULL, gap_event, NULL);
            if (rc != 0) {
                ESP_LOGE(TAG, "ble_gap_connect rc=%d", rc);
                schedule_rescan();
            }
        }
        return 0;

    case BLE_GAP_EVENT_DISC_COMPLETE:
        return 0;

    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status != 0) {
            ESP_LOGW(TAG, "connect failed status=%d", event->connect.status);
            schedule_rescan();
            return 0;
        }
        s_conn = event->connect.conn_handle;
        s_val_handle = s_cccd_handle = s_svc_end = 0;
        s_discovering = false;
        s_security_tried = false;
        ESP_LOGI(TAG, "connected");
        {
            struct ble_gap_conn_desc desc;
            if (ble_gap_conn_find(s_conn, &desc) == 0 && have_bond(&desc.peer_id_addr)) {
                ESP_LOGI(TAG, "known bond, encrypting link");
                s_security_tried = true;
                notify_ui(EP8_STATE_PAIRING);
                if (ble_gap_security_initiate(s_conn) == 0) {
                    return 0;
                }
            }
        }
        start_discovery();
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGW(TAG, "disconnected reason=0x%x", event->disconnect.reason);
        esp_timer_stop(s_poll_timer);
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        s_discovering = false;
        schedule_rescan();
        return 0;

    case BLE_GAP_EVENT_ENC_CHANGE:
        if (event->enc_change.status == 0) {
            ESP_LOGI(TAG, "link encrypted");
            start_discovery();
        } else {
            ESP_LOGE(TAG, "encryption failed status=%d", event->enc_change.status);
            /* Stale bond (e.g. unit was reset): drop it so the next attempt re-pairs. */
            struct ble_gap_conn_desc desc;
            if (ble_gap_conn_find(event->enc_change.conn_handle, &desc) == 0) {
                ble_store_util_delete_peer(&desc.peer_id_addr);
            }
            ble_gap_terminate(event->enc_change.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        }
        return 0;

    case BLE_GAP_EVENT_PASSKEY_ACTION: {
        struct ble_sm_io pk = {.action = event->passkey.params.action};
        if (pk.action == BLE_SM_IOACT_INPUT || pk.action == BLE_SM_IOACT_DISP) {
            pk.passkey = CONFIG_EP8_PASSKEY;
            ESP_LOGI(TAG, "passkey action %d, using %06d", pk.action, CONFIG_EP8_PASSKEY);
        } else if (pk.action == BLE_SM_IOACT_NUMCMP) {
            pk.numcmp_accept = 1;
        } else {
            return 0;
        }
        ble_sm_inject_io(event->passkey.conn_handle, &pk);
        return 0;
    }

    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        struct ble_gap_conn_desc desc;
        if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
            ble_store_util_delete_peer(&desc.peer_id_addr);
        }
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }

    case BLE_GAP_EVENT_NOTIFY_RX:
        if (event->notify_rx.attr_handle == s_val_handle && OS_MBUF_PKTLEN(event->notify_rx.om) >= 1) {
            uint8_t v;
            os_mbuf_copydata(event->notify_rx.om, 0, 1, &v);
            set_level(v);
        }
        return 0;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGD(TAG, "mtu=%d", event->mtu.value);
        return 0;

    default:
        return 0;
    }
}

/* ---------- host setup ---------- */

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    assert(rc == 0);
    rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    assert(rc == 0);
    start_scan();
}

static void on_reset(int reason)
{
    ESP_LOGE(TAG, "host reset, reason=%d", reason);
}

static void host_task(void *param)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void ble_battery_start(ep8_update_cb_t cb)
{
    s_cb = cb;

    const esp_timer_create_args_t targs = {.callback = poll_timer_cb, .name = "ep8_poll"};
    ESP_ERROR_CHECK(esp_timer_create(&targs, &s_poll_timer));
    const esp_timer_create_args_t rargs = {.callback = retry_timer_cb, .name = "ep8_retry"};
    ESP_ERROR_CHECK(esp_timer_create(&rargs, &s_retry_timer));

    ESP_ERROR_CHECK(nimble_port_init());

    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    ble_store_config_init();
    nimble_port_freertos_init(host_task);
}
