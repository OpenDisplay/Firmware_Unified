/* NO <Arduino.h>, <SPI.h> or <Wire.h> here (phase C step 11). This header defines the
 * firmware's globals; it uses no Arduino type or function of its own, and the three includes
 * were serving main.cpp -- its only consumer -- transitively. main.cpp now names its own
 * dependency explicitly, which is the point: a header that pulls in a surface it does not use
 * hides which file actually depends on it.
 *
 * <bb_epaper.h> below is NOT a back door. It includes <Arduino.h> only under #ifdef ARDUINO,
 * which this build does not define (it takes the __LINUX__ branch), so nothing is laundered
 * in through it. */
#include "structs.h"
#include "od_cmd.h"   /* od_cmd_result_t / od_cmd_ctx_t, for the handler declarations below */
#include "od_zlib_inflate.h"
#include <bb_epaper.h>
#include "encryption_state.h"
#include "config_parser.h"
#include "ble_transport.h"
#include "od_rxq.h"
#include "wifi_service.h"
#include "display_service.h"

#ifndef BUILD_VERSION
#define BUILD_VERSION "1.0.0"
#endif
#ifndef SHA
#define SHA ""
#endif

#define STRINGIFY(x) #x
#define XSTRINGIFY(x) STRINGIFY(x)
#define SHA_STRING XSTRINGIFY(SHA)


/* OD: config storage is NVS (shared/hal/od_hal_nvs.h); LittleFS is gone from this target. */


#define MAX_BLOCKS 64
// Text rendering constants
#define FONT_CHAR_WIDTH 16  // 7 font columns + 1 blank column, each doubled (8*2)
#define FONT_CHAR_HEIGHT 16 // 8 pixels tall, doubled (8*2)
#define FONT_BASE_WIDTH 8   // Base font width (7 columns + 1 spacing)
#define FONT_BASE_HEIGHT 8  // Base font height (8 rows)
#define FONT_SMALL_THRESHOLD 264  // Use 1x scale for displays narrower than this
// Config chunked write constants (CONFIG_CHUNK_SIZE, CONFIG_CHUNK_SIZE_WITH_PREFIX,
// MAX_CONFIG_CHUNKS), response buffer size (MAX_RESPONSE_DATA_SIZE), BLE response
// codes (RESP_*), and PIPE_WRITE constants all come from the canonical
// opendisplay_protocol.h (vendored, included via structs.h). Do not redefine here.

// Communication mode bit definitions (for system_config.communication_modes)
#define COMM_MODE_BLE           (1 << 0)  // Bit 0: BLE transfer supported
#define COMM_MODE_OEPL          (1 << 1)  // Bit 1: OEPL based transfer supported
#define COMM_MODE_WIFI          (1 << 2)  // Bit 2: WiFi transfer supported

// Device flags bit definitions (for system_config.device_flags)
#define DEVICE_FLAG_PWR_PIN      (1 << 0)  // Bit 0: Device has external power management pin
#define DEVICE_FLAG_XIAOINIT     (1 << 1)  // Bit 1: Call xiaoinit() after config load (nRF52840 only)
#define DEVICE_FLAG_WS_PP_INIT   (1 << 2)  // Bit 2: Call ws_pp_init() after config load (Waveshare Photo Printer)
#define DEVICE_FLAG_BATTERY_LATCH (1 << 3) // Bit 3: Self-holding battery latch on pwr_pin_2; optional active-low long-press shutdown button on pwr_pin_3
#define DEVICE_FLAG_PWR_LATCH_DFF (1 << 4) // Bit 4: 74AHC1G79 D-FF latch; pwr_pin_2=D, pwr_pin_3=CP; release via command 0x0052


// No BLE stack types here any more: NimBLE lives behind ble_transport_esp32.cpp.
#include <esp_system.h>
#include <esp_mac.h>
#include <esp_timer.h>
#include "esp_sleep.h"

// RTC memory variables for deep sleep state tracking (declared in main.cpp)
extern bool advertising_timeout_active;
extern uint32_t advertising_start_time;

BBEPDISP bbep;

// Forward declarations for bbep functions
void bbepInitIO(BBEPDISP *pBBEP, uint8_t u8DC, uint8_t u8RST, uint8_t u8BUSY, uint8_t u8CS, uint8_t u8MOSI, uint8_t u8SCK, uint32_t u32Speed);
int bbepSetPanelType(BBEPDISP *pBBEP, int iPanel);
void bbepSetRotation(BBEPDISP *pBBEP, int iRotation);
void bbepStartWrite(BBEPDISP *pBBEP, int iPlane);
int bbepRefresh(BBEPDISP *pBBEP, int iMode);
bool bbepIsBusy(BBEPDISP *pBBEP);
void bbepWakeUp(BBEPDISP *pBBEP);
void bbepSleep(BBEPDISP *pBBEP, int deepSleep);
void bbepSendCMDSequence(BBEPDISP *pBBEP, const uint8_t *pSeq);
void bbepSetAddrWindow(BBEPDISP *pBBEP, int x, int y, int cx, int cy);
void bbepWriteData(BBEPDISP *pBBEP, uint8_t *pData, int iLen);

uint8_t decompressionChunk[OPENDISPLAY_DECOMPRESSION_CHUNK_SIZE];
uint8_t bleResponseBuffer[94];
// Rolling MSD sequence nibble; persists across deep sleep so advertisements
// stay distinguishable across sleep/wake cycles.
RTC_DATA_ATTR uint8_t mloopcounter = 0;
// Persists across deep sleep so a wake is not mistaken for a reboot. Re-armed
// on the boot-screen path in setup(), which is the only path a real reset takes.
RTC_DATA_ATTR uint8_t rebootFlag = 1;  // Set to 1 after reboot, cleared to 0 after BLE connection
uint8_t connectionRequested = 0;  // Reserved for future features (connection requested flag)
uint8_t dynamicreturndata[11] = {0};  // Dynamic return data blocks (bytes 2-12 in advertising payload)
uint8_t msd_payload[16] = {0};  // Manufacturer Specific Data payload (public, updated by updatemsdata())

ButtonState buttonStates[MAX_BUTTONS] = {0};  // Button state tracking
uint8_t buttonStateCount = 0;  // Number of initialized buttons
volatile bool buttonEventPending = false;  // Flag set by ISR to indicate button event
volatile uint8_t lastChangedButtonIndex = 0xFF;  // Index of button that last changed (set by ISR)
uint8_t activeLedInstance = 0xFF;  // LED instance index for flashing (0xFF = none configured)

uint8_t staticRowBuffer[BOOT_ROW_BUFFER_SIZE];

char wifiSsid[33] = {0};  // 32 bytes + null terminator
char wifiPassword[33] = {0};  // 32 bytes + null terminator
uint8_t wifiEncryptionType = 0;  // 0x00=none, 0x01=WEP, 0x02=WPA, 0x03=WPA2, 0x04=WPA3
bool wifiConfigured = false;  // True if WiFi config packet (0x26) was received and parsed
// Small config-storage / status globals: kept on all ESP32 targets (config_parser
// and the config-dump report reference them regardless of whether the WiFi
// transport is compiled in). Trivial RAM cost.
bool wifiConnected = false;
bool wifiInitialized = false;
char wifiServerUrl[65] = {0};
uint16_t wifiServerPort = 2446;
// bool wifiServerConfigured = false;  // dead -- nothing reads it (config_parser.cpp)
#ifdef OPENDISPLAY_HAS_WIFI
// Heavy WiFi-transport surface: the TCP server/client objects and the RX
// reassembly buffer exist ONLY when the WiFi transport is compiled in -- the S3
// envs (E1004 inherits the flag from esp32-s3-N32R8-extuart). The classic and
// no-PSRAM parts reclaim this RAM; see src/wifi_service.h for the env list.
//
// tcpReceiveBuffer is the DEFINITION of the pointer declared in wifi_service.h
// (included above); its storage is reserved at boot by odLanReserveRxBuffer(),
// in PSRAM where there is any. Size lives with the declaration as
// OD_LAN_RX_BUFFER_SIZE -- do not reintroduce a literal here.
//
// The WiFiServer/WiFiClient objects that used to live here are GONE (phase C step
// 9b-iii). They were globals only because main.h is where this firmware's globals
// go; nothing outside wifi_service.cpp ever touched them, and they are now two
// file-static lwIP descriptors there. wifiServerConnected stays public because
// communication.cpp and the config-dump report do read it.
bool wifiServerConnected = false;
uint8_t* tcpReceiveBuffer = nullptr;
uint32_t tcpReceiveBufferPos = 0;
#endif

bool displayPowerState = false;  // Track display power state (true = powered on, false = powered off)
// EPD panel power state machine — single source of truth for panel power. The
// legacy displayPowerState bool is kept synced: displayPowerState == (pwrmgmState != PWR_OFF).
// enum PwrMgmState + EPD_KEEPALIVE_MAX_S are defined in display_service.h (shared header).
volatile uint8_t pwrmgmState = PWR_OFF;  // PWR_OFF / PWR_WARM / PWR_ACTIVE
uint32_t pwrmgmOffDeadlineMs = 0;        // keep-alive deadline (millis); valid only in PWR_WARM
// Session try-lock, uncontended since Phase 3 moved nRF dispatch onto loop().
// Kept as defence in depth -- see the rationale at pwrmgmLockTake() in
// display_service.cpp.
volatile uint8_t pwrmgmLock = 0;

bool waitforrefresh(int timeout);
void pwrmgm(bool onoff);
bool powerDownExternalFlash(uint8_t mosiPin, uint8_t misoPin, uint8_t sckPin, uint8_t csPin, uint8_t wpPin, uint8_t holdPin);
void powerDownExternalFlashFromConfig(void);
void xiaoinit();
void ws_pp_init();
void fullSetupAfterConnection();
// force: sleep even with a client connected (explicit host request 0x0053).
// overrideSleepSeconds: nonzero replaces deep_sleep_time_seconds for this one
// sleep cycle (0x0053 duration payload); never changes sleep eligibility.
void enterDeepSleep(bool force = false, uint16_t overrideSleepSeconds = 0);
extern bool advertising_timeout_active;
extern uint32_t advertising_start_time;
void getChipIdHex(char* out, size_t out_size);   // see encryption.h for the contract
void secureEraseConfig();
void checkResetPin();
void reboot();
void enterDFUMode();
void initio();
void initDataBuses();
void initButtons();
void processButtonEvents();  // Process button events and update BLE data
void idleDelay(uint32_t delayMs);  // Non-blocking delay that processes buttons at 100ms intervals
void flashLed(uint8_t color, uint8_t brightness);  // Flash LED with color and brightness
void processLedFlash();  // Advance async LED flash state machine
void handleLedActivate(uint8_t* data, uint16_t len);  // Handle LED activation command
void handleLedStop(uint8_t* data, uint16_t len);  // Stop running LED flash sequence
// Shared ISR handler. No target guard: both arms of the former #ifdef declared this
// identically -- IRAM_ATTR belongs on the DEFINITION (device_control.cpp), not here,
// and a declaration that differs only in its trailing comment is not a difference.
void handleButtonISR(uint8_t buttonIndex);
void scanI2CDevices();
void initSensors();
void initAXP2101(uint8_t busId);
void updatemsdata();
uint16_t calculateCRC16CCITT(uint8_t* data, uint32_t len);
od_cmd_result_t handleReadConfig(const od_cmd_ctx_t *ctx);
od_cmd_result_t handleWriteConfig(const od_cmd_ctx_t *ctx, uint8_t* data, uint16_t len);
od_cmd_result_t handleWriteConfigChunk(const od_cmd_ctx_t *ctx, uint8_t* data, uint16_t len);
od_cmd_result_t handleFirmwareVersion(const od_cmd_ctx_t *ctx);
od_cmd_result_t handleReadMSD(const od_cmd_ctx_t *ctx);  // Read Manufacturer Specific Data (MSD) payload
const char* getFirmwareShaString();
int mapEpd(int id);
uint8_t getFirmwareMajor();
uint8_t getFirmwareMinor();
uint8_t getFirmwarePatch();
float readBatteryVoltage();  // Returns battery voltage in volts, or -1.0 if not configured
float readChipTemperature();  // Returns chip temperature in degrees Celsius
int displayBootPlane(uint8_t colorScheme);
int displayBootBitsPerPixel(uint8_t colorScheme);

// chunked_write_state_t comes from config_parser.h so this file and
// communication.cpp cannot drift apart on the buffer size.
/* chunkedWriteState is GONE -- see config_parser.h. The 4 KB reassembly buffer now lives in
 * communication.cpp's g_configAsm (shared/core/od_config_asm.h). Same one buffer, same static
 * allocation; it simply is not defined in a header any more. */
struct od_config globalConfig = {0};
uint8_t configReadResponseBuffer[128];

// Security configuration. An ALIAS, not a second object: security is a member of the parsed
// config so the zero-key normalisation cannot be separated from the parse that applies it
// (shared/core/od_config.h). Keeping the old name means the consumers in encryption.cpp,
// boot_screen.cpp and communication.cpp do not care where it now lives.
struct SecurityConfig &securityConfig = globalConfig.security;
bool encryptionInitialized = false;

// 0x00000000 = "not set". Persists across deep sleep on ESP32.
RTC_DATA_ATTR uint32_t displayed_etag = 0;

// RTC memory variables for deep sleep state tracking
RTC_DATA_ATTR bool woke_from_deep_sleep = false;
RTC_DATA_ATTR uint32_t deep_sleep_count = 0;

// Advertising timeout state variables
bool advertising_timeout_active = false;
uint32_t advertising_start_time = 0;

// Minimum awake window, armed in setup() on first boot or button wake (never
// on timer wake). A floor layered under the quiet-window logic: sleep needs
// both the idle/advertising quiet condition AND this hold expired.
static constexpr uint16_t DEFAULT_MIN_WAKE_TIME_SECONDS = 120;
bool minWakeWindowActive = false;
uint32_t minWakeWindowStartMs = 0;

// Stamped by pollActivity() at the top of every loop() pass. Both sleep paths
// require a continuous quiet window since this stamp, so a dropped link, an
// in-flight command, or a pending ack extends the window rather than racing a
// single idle iteration.
uint32_t lastActivityMs = 0;
// Quiet window required before deep sleep when sleep_timeout_ms is unset.
static constexpr uint32_t DEFAULT_IDLE_HOLD_MS = 10000;

#define AXP2101_SLAVE_ADDRESS 0x34
#define AXP2101_REG_POWER_STATUS 0x00
#define AXP2101_REG_POWER_ON_STATUS 0x20
#define AXP2101_REG_POWER_OFF_STATUS 0x21
#define AXP2101_REG_DC_ONOFF_DVM_CTRL 0x80
#define AXP2101_REG_LDO_ONOFF_CTRL0 0x90
#define AXP2101_REG_DC_VOL0_CTRL 0x82  // DCDC1 voltage
#define AXP2101_REG_LDO_VOL2_CTRL 0x94  // ALDO3 voltage
#define AXP2101_REG_LDO_VOL3_CTRL 0x95  // ALDO4 voltage
#define AXP2101_REG_POWER_WAKEUP_CTL 0x26
#define AXP2101_REG_ADC_CHANNEL_CTRL 0x30
#define AXP2101_REG_ADC_DATA_BAT_VOL_H 0x34
#define AXP2101_REG_ADC_DATA_BAT_VOL_L 0x35
#define AXP2101_REG_ADC_DATA_VBUS_VOL_H 0x38
#define AXP2101_REG_ADC_DATA_VBUS_VOL_L 0x39
#define AXP2101_REG_ADC_DATA_SYS_VOL_H 0x3A
#define AXP2101_REG_ADC_DATA_SYS_VOL_L 0x3B
#define AXP2101_REG_BAT_PERCENT_DATA 0xA4
#define AXP2101_REG_PWRON_STATUS 0x20
#define AXP2101_REG_BAT_DETECTION_CTRL 0x68  // Battery detection control
#define AXP2101_REG_IRQ_ENABLE1 0x40  // IRQ enable register 1
#define AXP2101_REG_IRQ_ENABLE2 0x41  // IRQ enable register 2
#define AXP2101_REG_IRQ_ENABLE3 0x42  // IRQ enable register 3
#define AXP2101_REG_IRQ_STATUS1 0x48  // IRQ status register 1
#define AXP2101_REG_IRQ_STATUS2 0x49  // IRQ status register 2
#define AXP2101_REG_IRQ_STATUS3 0x4A  // IRQ status register 3
#define AXP2101_REG_LDO_ONOFF_CTRL1 0x91  // LDO control register 1 (DLDO2)

// The deferred-work flags that used to be defined here are now file-static in
// main.cpp: nothing outside it reads them, and the two that other translation
// units need to RAISE go through requestTransferSessionCleanup() /
// requestAdvertisingRestart() (communication.h). Keeping them here would have
// re-exported mutable state that only one file has any business touching.

extern const uint8_t writelineFont[] PROGMEM;
