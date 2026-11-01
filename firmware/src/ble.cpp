#include "ble.h"
#include <Arduino.h>
#include <NimBLEDevice.h>
#include <NimBLEHIDDevice.h>
#include <Preferences.h>

#define DEVICE_NAME "Clawdmeter2"

// Custom GATT UUIDs for data channel
#define SERVICE_UUID        "4c41555a-4465-7669-6365-000000000001"
#define RX_CHAR_UUID        "4c41555a-4465-7669-6365-000000000002"  // host writes here
#define TX_CHAR_UUID        "4c41555a-4465-7669-6365-000000000003"  // device ack/nack notifies
#define REQ_CHAR_UUID       "4c41555a-4465-7669-6365-000000000004"  // device-initiated refresh request

#define BLE_BUF_SIZE 768   // a full history message (30 + 24 numbers + a joke) fits

// HID keyboard report descriptor (standard 6-KRO boot-protocol-compatible).
// Includes the LED output report (Num/Caps/Scroll Lock indicators) — without
// it macOS's Keyboard Setup Assistant flags the device as "unidentifiable"
// because the descriptor doesn't look like a complete keyboard.
static const uint8_t HID_REPORT_MAP[] = {
    0x05, 0x01,  // Usage Page (Generic Desktop)
    0x09, 0x06,  // Usage (Keyboard)
    0xA1, 0x01,  // Collection (Application)
    0x85, 0x01,  //   Report ID (1)
    0x05, 0x07,  //   Usage Page (Key Codes)
    0x19, 0xE0,  //   Usage Minimum (224) - Left Control
    0x29, 0xE7,  //   Usage Maximum (231) - Right GUI
    0x15, 0x00,  //   Logical Minimum (0)
    0x25, 0x01,  //   Logical Maximum (1)
    0x75, 0x01,  //   Report Size (1)
    0x95, 0x08,  //   Report Count (8)
    0x81, 0x02,  //   Input (Data, Variable, Absolute) - Modifier byte
    0x95, 0x01,  //   Report Count (1)
    0x75, 0x08,  //   Report Size (8)
    0x81, 0x01,  //   Input (Constant) - Reserved byte
    // LED output report — required for macOS to treat this as a full keyboard.
    0x95, 0x05,  //   Report Count (5)
    0x75, 0x01,  //   Report Size (1)
    0x05, 0x08,  //   Usage Page (LEDs)
    0x19, 0x01,  //   Usage Minimum (Num Lock)
    0x29, 0x05,  //   Usage Maximum (Kana)
    0x91, 0x02,  //   Output (Data, Variable, Absolute) - LED report
    0x95, 0x01,  //   Report Count (1)
    0x75, 0x03,  //   Report Size (3)
    0x91, 0x01,  //   Output (Constant) - LED report padding
    0x95, 0x06,  //   Report Count (6)
    0x75, 0x08,  //   Report Size (8)
    0x15, 0x00,  //   Logical Minimum (0)
    0x25, 0x65,  //   Logical Maximum (101)
    0x05, 0x07,  //   Usage Page (Key Codes)
    0x19, 0x00,  //   Usage Minimum (0)
    0x29, 0x65,  //   Usage Maximum (101)
    0x81, 0x00,  //   Input (Data, Array) - Key array (6 keys)
    0xC0,        // End Collection
};

static NimBLEServer* server = nullptr;
static NimBLEHIDDevice* hid_dev = nullptr;
static NimBLECharacteristic* input_kbd = nullptr;
static NimBLECharacteristic* tx_char = nullptr;
static NimBLECharacteristic* rx_char = nullptr;
static NimBLECharacteristic* req_char = nullptr;

static ble_state_t state = BLE_STATE_INIT;
static bool need_advertise = false;

// One-shot supervision-timeout pushback (see onConnParamsUpdate). Written by
// NimBLE host-task callbacks, consumed by ble_tick() on the loop task.
static const uint16_t CONN_HANDLE_NONE  = 0xFFFF;
static const uint16_t DESIRED_TIMEOUT   = 600;   // ×10ms = 6s, matches PPCP
static volatile uint16_t param_fix_handle = CONN_HANDLE_NONE;  // pending retry
static volatile uint32_t param_fix_at_ms  = 0;                 // when to send it
static volatile uint16_t param_fix_spent  = CONN_HANDLE_NONE;  // one per connection
// RX writes land in a small queue on the NimBLE host task (core 0); the loop
// parses on core 1, one message per pass. Both sides copy under rx_lock, and
// the loop parses its own copy (parse_buf), so a write never changes the text
// mid-parse. clawdmeter2: a queue, not one slot: the app sends a permission
// card and a usage payload back to back, and one slot lost the first.
#define RX_SLOTS 4
static char rx_ring[RX_SLOTS][BLE_BUF_SIZE];
static uint8_t rx_head = 0, rx_tail = 0;   // write / read positions; equal = empty
static char parse_buf[BLE_BUF_SIZE];
static portMUX_TYPE rx_lock = portMUX_INITIALIZER_UNLOCKED;
static volatile bool has_received_data = false;
static char mac_str[18];

// --- Owner lock (clawdmeter2: several owners) ------------------------------
//
// The board is a BLE peripheral that any central in range could connect to and
// write usage data to. To stop a stranger's machine writing to it, only its
// OWNERS may: the identity addresses of up to OWNERS_MAX machines (e.g. a
// workstation at home and a laptop at work), persisted in NVS. The first
// machine to pair becomes an owner; another one joins only while the pairing
// window is open (ble_open_pairing: the PWR hold gesture). A full list drops
// its oldest owner for the new one. Anyone else that pairs is un-bonded and
// dropped. Clearing the bonds forgets every owner.
#define OWNERS_MAX 3
static Preferences prefs;
static char owners[OWNERS_MAX][18] = {};   // identity addresses, "aa:bb:cc:dd:ee:ff"
static int  owner_count = 0;
static bool owner_set = false;             // owner_count > 0
static const char* ZERO_ADDR = "00:00:00:00:00:00";
static volatile uint32_t pairing_until_ms = 0;   // window for a new owner; 0 = closed

static bool pairing_open() {
    const uint32_t until = pairing_until_ms;
    if (until == 0) return false;
    if ((int32_t)(until - millis()) > 0) return true;
    // Expired: close it for good, or the wrapped comparison would reopen it
    // ~24.8 days later.
    pairing_until_ms = 0;
    return false;
}

static bool is_owner(const char* id) {
    for (int i = 0; i < owner_count; i++)
        if (strcmp(owners[i], id) == 0) return true;
    return false;
}

static void save_owners() {
    String all;
    for (int i = 0; i < owner_count; i++) {
        if (i) all += ",";
        all += owners[i];
    }
    prefs.begin("clawd", false);
    prefs.putString("owners", all);
    prefs.remove("owner");   // the single-owner key of older firmware
    prefs.end();
}

static void clear_owner() {
    owner_count = 0;
    owner_set = false;
    prefs.begin("clawd", false);
    prefs.remove("owner");
    prefs.remove("owners");
    prefs.end();
}

static void add_owner_addr(const char* id) {
    if (strlen(id) != 17 || is_owner(id)) return;
    if (owner_count == OWNERS_MAX) {           // drop the oldest; its bond goes in prune_foreign_bonds()
        Serial.printf("BLE: owner list full, dropping %s\n", owners[0]);
        for (int i = 1; i < OWNERS_MAX; i++) memcpy(owners[i - 1], owners[i], 18);
        owner_count--;
    }
    memcpy(owners[owner_count], id, 17);
    owners[owner_count][17] = '\0';
    owner_count++;
    owner_set = true;
}

static void load_owner() {
    prefs.begin("clawd", true);
    String all = prefs.getString("owners", "");
    String legacy = prefs.getString("owner", "");
    prefs.end();
    owner_count = 0;
    int start = 0;
    while (start < (int)all.length()) {
        int comma = all.indexOf(',', start);
        if (comma < 0) comma = all.length();
        add_owner_addr(all.substring(start, comma).c_str());
        start = comma + 1;
    }
    if (legacy.length() == 17) add_owner_addr(legacy.c_str());
    owner_set = owner_count > 0;
    for (int i = 0; i < owner_count; i++) Serial.printf("BLE: owner loaded = %s\n", owners[i]);
}

// Delete every stored bond that isn't an owner. Removing a bond shifts the
// indices, so restart from 0.
static void prune_foreign_bonds() {
    if (!owner_set) return;
    bool removed;
    do {
        removed = false;
        int n = NimBLEDevice::getNumBonds();
        for (int i = 0; i < n; i++) {
            NimBLEAddress a = NimBLEDevice::getBondedAddress(i);
            if (!is_owner(a.toString().c_str())) {
                Serial.printf("BLE: pruning non-owner bond %s\n", a.toString().c_str());
                NimBLEDevice::deleteBond(a);
                removed = true;
                break;
            }
        }
    } while (removed);
}

static void claim_owner(const std::string& id) {
    add_owner_addr(id.c_str());
    save_owners();
    pairing_until_ms = 0;                      // one new computer per window
    Serial.printf("BLE: owner added = %s (%d owners)\n", id.c_str(), owner_count);
    prune_foreign_bonds();
}

// A machine that just authenticated or wrote: may it become an owner?
static bool may_claim(const std::string& id) {
    return id != ZERO_ADDR && !is_owner(id.c_str()) && (!owner_set || pairing_open());
}

static void start_advertising() {
    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    adv->reset();
    // Primary advertising packet (≤31 bytes):
    //   flags (3) + appearance (4) + HID service 0x1812 (4) + name "Clawdmeter2" (13)
    //   = 24 bytes. macOS Bluetooth Settings only surfaces BLE-only devices
    //   that explicitly advertise the standard HID service UUID (0x1812) —
    //   without it the device is recognized internally but hidden from the
    //   GUI nearby-devices list.
    adv->setAppearance(HID_KEYBOARD);
    adv->addServiceUUID(NimBLEUUID((uint16_t)0x1812));  // BLE HID Service
    adv->setName(DEVICE_NAME);
    // Scan response carries the 128-bit custom data-service UUID for active
    // scanners (the host app scans actively).
    NimBLEAdvertisementData scanResp;
    scanResp.setCompleteServices(NimBLEUUID(SERVICE_UUID));
    adv->setScanResponseData(scanResp);
    adv->enableScanResponse(true);
    bool ok = adv->start();
    // Only reflect ADVERTISING in the UI state when no client is connected.
    // With MAX_CONNECTIONS=2, onConnect re-advertises to fill the second slot;
    // without this guard the UI would flip CONNECTED → ADVERTISING on every
    // first connect and never come back until a second client arrived.
    if (!server || server->getConnectedCount() == 0) {
        state = BLE_STATE_ADVERTISING;
    }
    Serial.printf("BLE: advertising start=%s (connected=%u)\n",
        ok ? "OK" : "FAILED",
        server ? (unsigned)server->getConnectedCount() : 0);
}

class ServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer* s, NimBLEConnInfo& info) override {
        state = BLE_STATE_CONNECTED;
        Serial.printf("BLE: [%lu ms] connected from %s (active=%u)\n",
            (unsigned long)millis(), info.getAddress().toString().c_str(),
            (unsigned)s->getConnectedCount());
        // Log negotiated link timing — the difference between guessing and
        // knowing when debugging disconnects (e.g. reason=520 supervision
        // timeouts are only explainable next to the negotiated timeout).
        // Units: interval ×1.25ms, timeout ×10ms, latency = skippable events.
        Serial.printf("BLE: connparams itvl=%u(%.2fms) lat=%u timeout=%u(%ums)\n",
            info.getConnInterval(), info.getConnInterval() * 1.25f,
            info.getConnLatency(), info.getConnTimeout(), info.getConnTimeout() * 10);
        // Keep advertising while a connection slot is still free so a second
        // central (e.g. the host app alongside an OS-held HID link) can
        // discover and connect. NimBLE auto-stops advertising on each accept.
        if (s->getConnectedCount() < CONFIG_BT_NIMBLE_MAX_CONNECTIONS) {
            need_advertise = true;
        }
        // Start pairing/encryption ourselves. RX writes are only accepted over
        // an encrypted link, but nothing in the GATT table forces the central
        // to pair; macOS can keep a cached GATT table after a bond clear and
        // never read a protected HID characteristic, leaving it stuck on
        // "Connecting" with an unencrypted link.
        // Only for peers we have NO bond with: on a bonded reconnect the
        // central (Windows) starts encryption itself, and our extra Security
        // Request overlapping it broke the link (enc=0, reason=520) after
        // every board reboot until the user re-paired.
        bool known = NimBLEDevice::isBonded(info.getIdAddress()) ||
                     NimBLEDevice::isBonded(info.getAddress());
        Serial.printf("BLE: peer bonded-before=%d enc=%d\n", known ? 1 : 0, info.isEncrypted() ? 1 : 0);
        if (!info.isEncrypted() && !known) {
            int rc = 0;
            bool ok = NimBLEDevice::startSecurity(info.getConnHandle(), &rc);
            Serial.printf("BLE: startSecurity %s (rc=%d)\n", ok ? "OK" : "FAILED", rc);
        }
    }

    void onDisconnect(NimBLEServer* s, NimBLEConnInfo& info, int reason) override {
        // Only flip the UI state to DISCONNECTED when the last client leaves.
        if (s->getConnectedCount() == 0) state = BLE_STATE_DISCONNECTED;
        need_advertise = true;
        // Drop any pending/spent param pushback for this handle — NimBLE
        // reuses conn handles, so stale state would leak onto the next link.
        if (param_fix_handle == info.getConnHandle()) param_fix_handle = CONN_HANDLE_NONE;
        if (param_fix_spent  == info.getConnHandle()) param_fix_spent  = CONN_HANDLE_NONE;
        Serial.printf("BLE: [%lu ms] disconnected (reason=%d, remaining=%u)\n",
            (unsigned long)millis(), reason, (unsigned)s->getConnectedCount());
    }

    // Centrals re-negotiate parameters mid-connection. Windows in particular
    // clamps the supervision timeout to 2s once an app GATT session goes
    // active (captured on hardware; it honors 9.6s while only its HID driver
    // holds the link) — and a 2s window is tight enough that ordinary radio
    // gaps kill the link (HCI 0x208 → reason=520), which caused constant
    // reconnect churn. Push back ONCE per connection: schedule a
    // deferred LL connection-parameter request for the same interval range but
    // a 6s timeout (the mechanism Microsoft's accessory guidelines prescribe).
    // Deferred ~2s so it can't race the central's own in-flight update
    // transaction (Windows has a documented late-instant bug there), and
    // one-shot so a central that re-clamps doesn't trigger an update war.
    void onConnParamsUpdate(NimBLEConnInfo& info) override {
        Serial.printf("BLE: connparams update itvl=%u(%.2fms) lat=%u timeout=%u(%ums)\n",
            info.getConnInterval(), info.getConnInterval() * 1.25f,
            info.getConnLatency(), info.getConnTimeout(), info.getConnTimeout() * 10);
        if (info.getConnTimeout() < DESIRED_TIMEOUT &&
            info.getConnHandle() != param_fix_spent) {
            param_fix_handle = info.getConnHandle();
            param_fix_at_ms  = millis() + 2000;
        }
    }

    // Lock the board to a single owner machine. The first machine to bond
    // becomes the owner; any other machine that pairs is un-bonded and dropped
    // so the board never shows (or rotates to) a second machine's account.
    void onAuthenticationComplete(NimBLEConnInfo& info) override {
        std::string id = info.getIdAddress().toString();
        Serial.printf("BLE: [%lu ms] auth complete peer=%s conn=%s bonded=%d enc=%d auth=%d\n",
            (unsigned long)millis(), id.c_str(), info.getAddress().toString().c_str(),
            info.isBonded() ? 1 : 0, info.isEncrypted() ? 1 : 0,
            info.isAuthenticated() ? 1 : 0);
        // Bonded reconnects START at the central's clamped parameters (no
        // later update event fires), so the supervision-timeout pushback must
        // also arm here, not just in onConnParamsUpdate.
        if (info.getConnTimeout() < DESIRED_TIMEOUT &&
            info.getConnHandle() != param_fix_spent) {
            param_fix_handle = info.getConnHandle();
            param_fix_at_ms  = millis() + 2000;
        }
        if (id == ZERO_ADDR) return;
        if (may_claim(id)) {
            claim_owner(id);
        } else if (!is_owner(id.c_str())) {
            Serial.printf("BLE: rejecting non-owner %s (%d owners, pairing closed)\n", id.c_str(), owner_count);
            NimBLEDevice::deleteBond(info.getIdAddress());
            server->disconnect(info);
        }
    }

};

class RxCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* chr, NimBLEConnInfo& info) override {
        // Only accept usage data over a bonded+encrypted link, and only from the
        // owner machine. Another machine's app in range is ignored so the
        // display never rotates to a foreign account. The first encrypted writer
        // claims ownership when none is set yet (e.g. a fresh pairing).
        std::string id = info.getIdAddress().toString();
        if (!info.isEncrypted()) {
            Serial.println("BLE: dropping RX write from unencrypted link");
            return;
        }
        if (may_claim(id)) {
            claim_owner(id);
        }
        if (owner_set && !is_owner(id.c_str())) {
            Serial.printf("BLE: dropping RX write from non-owner %s\n", id.c_str());
            return;
        }
        std::string val = chr->getValue();
        size_t len = std::min(val.length(), (size_t)(BLE_BUF_SIZE - 1));
        portENTER_CRITICAL(&rx_lock);
        memcpy(rx_ring[rx_head], val.c_str(), len);
        rx_ring[rx_head][len] = '\0';
        rx_head = (uint8_t)((rx_head + 1) % RX_SLOTS);
        if (rx_head == rx_tail) rx_tail = (uint8_t)((rx_tail + 1) % RX_SLOTS);   // full: drop the oldest
        portEXIT_CRITICAL(&rx_lock);
        has_received_data = true;
    }
};

// When the app enables notifications on the refresh char, ask for data
// if we have none yet. Firing on subscribe (not on connect) ensures the
// notification isn't dropped before the app's CCCD write completes.
class ReqCallbacks : public NimBLECharacteristicCallbacks {
    void onSubscribe(NimBLECharacteristic* chr, NimBLEConnInfo& info, uint16_t subValue) override {
        Serial.printf("BLE: req_char onSubscribe subValue=%u has_data=%d\n", subValue, has_received_data ? 1 : 0);
        if (subValue != 0 && !has_received_data) {
            ble_request_refresh();
        }
    }
};

void ble_init(void) {
    NimBLEDevice::init(DEVICE_NAME);
    NimBLEDevice::setSecurityAuth(true, false, true);  // bonding, no MITM, SC
    // Exchange identity keys too, so a bonded central using a private
    // address is still recognised after the board reboots.
    NimBLEDevice::setSecurityInitKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);
    NimBLEDevice::setSecurityRespKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);

    // Restore the locked owner (if any) and drop any stale non-owner bonds so
    // the board stays paired to a single machine across reboots.
    load_owner();
    prune_foreign_bonds();

    // Format MAC address
    NimBLEAddress addr = NimBLEDevice::getAddress();
    snprintf(mac_str, sizeof(mac_str), "%s", addr.toString().c_str());
    for (int i = 0; mac_str[i]; i++) {
        if (mac_str[i] >= 'a' && mac_str[i] <= 'f') mac_str[i] -= 32;
    }

    server = NimBLEDevice::createServer();
    static ServerCallbacks serverCb;
    server->setCallbacks(&serverCb);

    // --- HID keyboard service ---
    hid_dev = new NimBLEHIDDevice(server);
    hid_dev->setReportMap((uint8_t*)HID_REPORT_MAP, sizeof(HID_REPORT_MAP));
    hid_dev->setManufacturer("Clawdmeter2");
    // PnP ID: (vendorIdSource, vendorId, productId, version).
    // Source 1 = Bluetooth SIG, vendor 0x02E5 = Espressif. Don't claim
    // Apple's IDs (USB vendor 0x05AC): macOS validates Apple-claimed HIDs
    // against known devices and silently refuses to surface a Connect button
    // for spoofers.
    hid_dev->setPnp(0x01, 0x02E5, 0x0001, 0x0100);
    // country=33 (US ANSI). Setting this to 0 ("not supported") causes macOS
    // to launch the Keyboard Setup Assistant on first pair asking the user
    // to identify the layout — we only ever send Space / Shift+Tab so the
    // physical layout is irrelevant; advertise a known one to skip the wizard.
    hid_dev->setHidInfo(33, 0x02);
    hid_dev->setBatteryLevel(100);
    input_kbd = hid_dev->getInputReport(1);  // report ID 1

    // --- Custom data service ---
    NimBLEService* svc = server->createService(SERVICE_UUID);

    rx_char = svc->createCharacteristic(
        RX_CHAR_UUID,
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR
    );
    static RxCallbacks rxCb;
    rx_char->setCallbacks(&rxCb);

    tx_char = svc->createCharacteristic(
        TX_CHAR_UUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
    );

    req_char = svc->createCharacteristic(
        REQ_CHAR_UUID,
        NIMBLE_PROPERTY::NOTIFY
    );
    static ReqCallbacks reqCb;
    req_char->setCallbacks(&reqCb);

    svc->start();
    server->start();
    start_advertising();

    Serial.printf("BLE: init complete, MAC=%s\n", mac_str);
}

void ble_tick(void) {
    if (need_advertise) {
        need_advertise = false;
        start_advertising();
    }
    // Deferred one-shot supervision-timeout pushback (see onConnParamsUpdate).
    if (param_fix_handle != CONN_HANDLE_NONE &&
        (int32_t)(millis() - param_fix_at_ms) >= 0) {
        uint16_t h = param_fix_handle;
        param_fix_handle = CONN_HANDLE_NONE;
        param_fix_spent  = h;
        if (server && server->getConnectedCount() > 0) {
            Serial.println("BLE: requesting 6s supervision timeout");
            server->updateConnParams(h, 12, 24, 0, DESIRED_TIMEOUT);
        }
    }
}

ble_state_t ble_get_state(void) {
    return state;
}

const char* ble_get_device_name(void) {
    return DEVICE_NAME;
}

const char* ble_get_mac_address(void) {
    return mac_str;
}

void ble_clear_bonds(void) {
    // Delete bonds one at a time (always index 0: each delete shifts the list)
    // and report failures: deleteAllBonds() bails silently on the first error,
    // leaving a stale bond in NVS that breaks re-pairing.
    for (int guard = NimBLEDevice::getNumBonds(); guard > 0 && NimBLEDevice::getNumBonds() > 0; guard--) {
        NimBLEAddress a = NimBLEDevice::getBondedAddress(0);
        bool ok = NimBLEDevice::deleteBond(a);
        if (!ok) {
            // ble_gap_unpair() stops at the first missing record (e.g. our_sec
            // already gone) and leaves the peer's security entry behind, so
            // delete each record directly and ignore "not found".
            struct ble_store_key_sec key;
            memset(&key, 0, sizeof(key));
            key.peer_addr = *a.getBase();
            int r1 = ble_store_delete_peer_sec(&key);
            int r2 = ble_store_delete_our_sec(&key);
            Serial.printf("BLE: unpair failed; direct delete peer_sec rc=%d our_sec rc=%d\n", r1, r2);
        }
        Serial.printf("BLE: delete bond %s -> %s\n", a.toString().c_str(), ok ? "ok" : "forced");
    }
    clear_owner();  // release ownership so the board can be handed to another machine
    Serial.printf("BLE: bonds cleared (remaining=%d)\n", NimBLEDevice::getNumBonds());
    // Every link, not just the first: a second one would still be encrypted
    // and could claim the board again with its next write.
    if (state == BLE_STATE_CONNECTED) ble_disconnect_all();
    need_advertise = true;
}

void ble_print_status(void) {
    static const char* names[] = {"INIT", "ADVERTISING", "CONNECTED", "DISCONNECTED"};
    Serial.printf("BLE status: state=%s bonds=%d owners=%d pairing=%s\n",
        names[state], NimBLEDevice::getNumBonds(), owner_count, pairing_open() ? "open" : "closed");
    for (int i = 0; i < owner_count; i++) Serial.printf("BLE status: owner=%s\n", owners[i]);
    if (!server) return;
    for (uint8_t i = 0; i < server->getConnectedCount(); i++) {
        NimBLEConnInfo ci = server->getPeerInfo(i);
        Serial.printf("BLE status: peer=%s id=%s enc=%d bonded=%d\n",
            ci.getAddress().toString().c_str(), ci.getIdAddress().toString().c_str(),
            ci.isEncrypted() ? 1 : 0, ci.isBonded() ? 1 : 0);
    }
}

void ble_open_pairing(uint32_t ms) {
    pairing_until_ms = millis() + ms;
    if (pairing_until_ms == 0) pairing_until_ms = 1;
    need_advertise = true;                     // a free slot advertises for the newcomer
    Serial.printf("BLE: pairing open for %lu s (%d owners kept)\n", (unsigned long)(ms / 1000), owner_count);
}

bool ble_pairing_open(void) { return pairing_open(); }

void ble_close_pairing(void) { pairing_until_ms = 0; }

void ble_disconnect_all(void) {
    if (!server) return;
    // Collect the handles first: each disconnect shrinks the peer list.
    uint16_t handles[CONFIG_BT_NIMBLE_MAX_CONNECTIONS];
    uint8_t n = 0;
    for (uint8_t i = 0; i < server->getConnectedCount() && n < CONFIG_BT_NIMBLE_MAX_CONNECTIONS; i++) {
        handles[n++] = server->getPeerInfo(i).getConnHandle();
    }
    for (uint8_t i = 0; i < n; i++) server->disconnect(handles[i]);
}

bool ble_has_data(void) {
    portENTER_CRITICAL(&rx_lock);
    const bool any = rx_head != rx_tail;
    portEXIT_CRITICAL(&rx_lock);
    return any;
}

const char* ble_get_data(void) {
    portENTER_CRITICAL(&rx_lock);
    if (rx_head != rx_tail) {
        memcpy(parse_buf, rx_ring[rx_tail], sizeof(parse_buf));
        rx_tail = (uint8_t)((rx_tail + 1) % RX_SLOTS);
    } else {
        parse_buf[0] = '\0';
    }
    portEXIT_CRITICAL(&rx_lock);
    return parse_buf;
}

void ble_send_ack(void) {
    if (state == BLE_STATE_CONNECTED && tx_char) {
        tx_char->setValue("{\"ack\":true}");
        tx_char->notify();
    }
}

void ble_send_nack(void) {
    if (state == BLE_STATE_CONNECTED && tx_char) {
        tx_char->setValue("{\"err\":true}");
        tx_char->notify();
    }
}

void ble_send_perm(uint32_t id, bool allow) {
    if (state == BLE_STATE_CONNECTED && tx_char) {
        char buf[48];
        snprintf(buf, sizeof(buf), "{\"perm\":%lu,\"allow\":%s}", (unsigned long)id, allow ? "true" : "false");
        tx_char->setValue(buf);
        tx_char->notify();
        Serial.printf("BLE: permission %lu %s on the device\n", (unsigned long)id, allow ? "allowed" : "denied");
    }
}

void ble_send_dismiss(void) {
    if (state == BLE_STATE_CONNECTED && tx_char) {
        tx_char->setValue("{\"dismiss\":true}");
        tx_char->notify();
        Serial.println("BLE: alert dismissed on the device");
    }
}

void ble_set_battery_level(int pct) {
    if (!hid_dev || pct < 0) return;
    if (pct > 100) pct = 100;
    hid_dev->setBatteryLevel((uint8_t)pct, state == BLE_STATE_CONNECTED);
}

void ble_request_refresh(void) {
    if (state == BLE_STATE_CONNECTED && req_char) {
        uint8_t v = 0x01;
        req_char->setValue(&v, 1);
        req_char->notify();
        Serial.println("BLE: refresh requested");
    }
}

void ble_keyboard_press(uint8_t key, uint8_t modifier) {
    if (state != BLE_STATE_CONNECTED || !input_kbd) return;
    // HID report: [modifier, reserved, key1, key2, key3, key4, key5, key6]
    uint8_t report[8] = {modifier, 0, key, 0, 0, 0, 0, 0};
    input_kbd->setValue(report, sizeof(report));
    input_kbd->notify();
}

void ble_keyboard_release(void) {
    if (state != BLE_STATE_CONNECTED || !input_kbd) return;
    uint8_t report[8] = {0};
    input_kbd->setValue(report, sizeof(report));
    input_kbd->notify();
}
