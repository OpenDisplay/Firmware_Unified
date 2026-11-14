/* GATT table: GAP, GATT, the OpenDisplay service and the (gated) Telink OTA service. Table layout and the GAP/GATT rows follow
 * Telink tc_ble_single_sdk vendor/ble_sample/app_att.c (Apache-2.0). */
#include "tl_common.h"
#include "stack/ble/ble.h"

#include "app_att.h"
#include "tlsr_port.h"

#define OD_UUID16          0x2446
#define OD_WRITE_MAX       (MTU_SIZE_SETTING - 3)   /* ATT write value bound: MTU minus opcode+handle */
#define OD_NAME_MAX        16

static const u16 my_primaryServiceUUID  = GATT_UUID_PRIMARY_SERVICE;
static const u16 my_characterUUID       = GATT_UUID_CHARACTER;
static const u16 clientCharacterCfgUUID = GATT_UUID_CLIENT_CHAR_CFG;
static const u16 serviceChangeUUID      = GATT_UUID_SERVICE_CHANGE;

static const u16 my_gapServiceUUID      = SERVICE_UUID_GENERIC_ACCESS;
static const u16 my_devNameUUID         = GATT_UUID_DEVICE_NAME;
static const u16 my_appearanceUUID      = GATT_UUID_APPEARANCE;
static const u16 my_periConnParamUUID   = GATT_UUID_PERI_CONN_PARAM;
static const u16 my_appearance          = GAP_APPEARE_UNKNOWN;
static const u16 my_gattServiceUUID     = SERVICE_UUID_GENERIC_ATTRIBUTE;
/* Peripheral Preferred Connection Parameters (0x2A04): interval min/max, latency, timeout. */
static const u16 my_periConnParameters[4] = {8, 24, 0, 400};

static u16 serviceChangeVal[2];
static u8  serviceChangeCCC[2];

static u8  my_devName[OD_NAME_MAX] = {'O', 'D'};

static const u16 my_odUUID = OD_UUID16;
static u8  my_odData[OD_WRITE_MAX];
static u8  odDataCCC[2];

static const u8 my_devNameCharVal[5] = {
    CHAR_PROP_READ,
    U16_LO(GenericAccess_DeviceName_DP_H), U16_HI(GenericAccess_DeviceName_DP_H),
    U16_LO(GATT_UUID_DEVICE_NAME), U16_HI(GATT_UUID_DEVICE_NAME)
};
static const u8 my_appearanceCharVal[5] = {
    CHAR_PROP_READ,
    U16_LO(GenericAccess_Appearance_DP_H), U16_HI(GenericAccess_Appearance_DP_H),
    U16_LO(GATT_UUID_APPEARANCE), U16_HI(GATT_UUID_APPEARANCE)
};
static const u8 my_periConnParamCharVal[5] = {
    CHAR_PROP_READ,
    U16_LO(CONN_PARAM_DP_H), U16_HI(CONN_PARAM_DP_H),
    U16_LO(GATT_UUID_PERI_CONN_PARAM), U16_HI(GATT_UUID_PERI_CONN_PARAM)
};
static const u8 my_serviceChangeCharVal[5] = {
    CHAR_PROP_INDICATE,
    U16_LO(GenericAttribute_ServiceChanged_DP_H), U16_HI(GenericAttribute_ServiceChanged_DP_H),
    U16_LO(GATT_UUID_SERVICE_CHANGE), U16_HI(GATT_UUID_SERVICE_CHANGE)
};
/* Telink OTA service, rows as in the SDK's ble_sample. Every write goes through ota_write(),
 * which drops it unless tlsr_port_ota_arm() was called on this connection -- i.e. unless the
 * central sent OpenDisplay's ENTER_DFU (0x0051), which the command gate authenticates whenever
 * the device has a key. Unarmed, the service is inert: anyone in range could otherwise reflash. */
static const u16 userdesc_UUID = GATT_UUID_CHAR_USER_DESC;
static const u8  my_OtaServiceUUID[16] = WRAPPING_BRACES(TELINK_OTA_UUID_SERVICE);
static const u8  my_OtaUUID[16]        = WRAPPING_BRACES(TELINK_SPP_DATA_OTA);
static u8        my_OtaData;
static u8        otaDataCCC[2];
static const u8  my_OtaName[] = {'O', 'T', 'A'};
static const u8  my_OtaCharVal[19] = {
    CHAR_PROP_READ | CHAR_PROP_WRITE_WITHOUT_RSP | CHAR_PROP_NOTIFY | CHAR_PROP_WRITE,
    U16_LO(OTA_CMD_OUT_DP_H), U16_HI(OTA_CMD_OUT_DP_H),
    TELINK_SPP_DATA_OTA,
};
static u8 s_ota_armed;

void tlsr_port_ota_arm(bool on)
{
    s_ota_armed = on ? 1 : 0;
}

static int ota_write(void *p)
{
    if (!s_ota_armed) {
        return 0;
    }
    return otaWrite(p);
}

static const u8 my_odCharVal[5] = {
    CHAR_PROP_WRITE | CHAR_PROP_WRITE_WITHOUT_RSP | CHAR_PROP_NOTIFY,
    U16_LO(OD_DP_H), U16_HI(OD_DP_H),
    U16_LO(OD_UUID16), U16_HI(OD_UUID16)
};

static int od_att_write(void *p)
{
    rf_packet_att_write_t *req = (rf_packet_att_write_t *)p;

    tlsr_port_crumb(1);
    if (req->l2capLen >= 3u) {                       /* opcode + handle precede the value */
        od_tlsr_on_write(&req->value, (u16)(req->l2capLen - 3u));
    }
    tlsr_port_crumb(3);
    return 0;
}

static int od_ccc_write(void *p)
{
    rf_packet_att_write_t *req = (rf_packet_att_write_t *)p;

    if (req->l2capLen >= 3u + 2u) {
        odDataCCC[0] = (&req->value)[0];
        odDataCCC[1] = (&req->value)[1];
        od_tlsr_on_notify_enabled((odDataCCC[0] & 0x01u) != 0u);
    }
    return 0;
}

/* attrLen of the device name is fixed at table build; app_att_set_device_name() fills the
 * buffer and the table row is rebuilt with the real length before the stack sees it. */
static attribute_t my_Attributes[] = {
    {ATT_END_H - 1, 0, 0, 0, 0, 0, 0, 0},

    {7, ATT_PERMISSIONS_READ, 2, 2, (u8 *)&my_primaryServiceUUID, (u8 *)&my_gapServiceUUID, 0, 0},
    {0, ATT_PERMISSIONS_READ, 2, sizeof(my_devNameCharVal), (u8 *)&my_characterUUID, (u8 *)my_devNameCharVal, 0, 0},
    {0, ATT_PERMISSIONS_READ, 2, 2, (u8 *)&my_devNameUUID, (u8 *)my_devName, 0, 0},
    {0, ATT_PERMISSIONS_READ, 2, sizeof(my_appearanceCharVal), (u8 *)&my_characterUUID, (u8 *)my_appearanceCharVal, 0, 0},
    {0, ATT_PERMISSIONS_READ, 2, sizeof(my_appearance), (u8 *)&my_appearanceUUID, (u8 *)&my_appearance, 0, 0},
    {0, ATT_PERMISSIONS_READ, 2, sizeof(my_periConnParamCharVal), (u8 *)&my_characterUUID, (u8 *)my_periConnParamCharVal, 0, 0},
    {0, ATT_PERMISSIONS_READ, 2, sizeof(my_periConnParameters), (u8 *)&my_periConnParamUUID, (u8 *)my_periConnParameters, 0, 0},

    {4, ATT_PERMISSIONS_READ, 2, 2, (u8 *)&my_primaryServiceUUID, (u8 *)&my_gattServiceUUID, 0, 0},
    {0, ATT_PERMISSIONS_READ, 2, sizeof(my_serviceChangeCharVal), (u8 *)&my_characterUUID, (u8 *)my_serviceChangeCharVal, 0, 0},
    {0, ATT_PERMISSIONS_READ, 2, sizeof(serviceChangeVal), (u8 *)&serviceChangeUUID, (u8 *)serviceChangeVal, 0, 0},
    {0, ATT_PERMISSIONS_RDWR, 2, sizeof(serviceChangeCCC), (u8 *)&clientCharacterCfgUUID, (u8 *)serviceChangeCCC, 0, 0},

    {4, ATT_PERMISSIONS_READ, 2, 2, (u8 *)&my_primaryServiceUUID, (u8 *)&my_odUUID, 0, 0},
    {0, ATT_PERMISSIONS_READ, 2, sizeof(my_odCharVal), (u8 *)&my_characterUUID, (u8 *)my_odCharVal, 0, 0},
    {0, ATT_PERMISSIONS_WRITE, 2, sizeof(my_odData), (u8 *)&my_odUUID, my_odData, &od_att_write, 0},
    {0, ATT_PERMISSIONS_RDWR, 2, sizeof(odDataCCC), (u8 *)&clientCharacterCfgUUID, odDataCCC, &od_ccc_write, 0},

    {5, ATT_PERMISSIONS_READ, 2, 16, (u8 *)&my_primaryServiceUUID, (u8 *)&my_OtaServiceUUID, 0, 0},
    {0, ATT_PERMISSIONS_READ, 2, sizeof(my_OtaCharVal), (u8 *)&my_characterUUID, (u8 *)my_OtaCharVal, 0, 0},
    {0, ATT_PERMISSIONS_RDWR, 16, sizeof(my_OtaData), (u8 *)&my_OtaUUID, &my_OtaData, &ota_write, 0},
    {0, ATT_PERMISSIONS_RDWR, 2, sizeof(otaDataCCC), (u8 *)&clientCharacterCfgUUID, otaDataCCC, 0, 0},
    {0, ATT_PERMISSIONS_READ, 2, sizeof(my_OtaName), (u8 *)&userdesc_UUID, (u8 *)my_OtaName, 0, 0},
};

void app_att_set_device_name(const unsigned char *name, unsigned char len)
{
    if (len > OD_NAME_MAX) {
        len = OD_NAME_MAX;
    }
    memcpy(my_devName, name, len);
    my_Attributes[GenericAccess_DeviceName_DP_H].attrLen = len;
}

int app_att_notify_enabled(void)
{
    return (odDataCCC[0] & 0x01u) != 0u;
}

void app_att_reset_subscriptions(void)
{
    odDataCCC[0] = 0;
    odDataCCC[1] = 0;
}

void my_att_init(void)
{
    bls_att_setAttributeTable((u8 *)my_Attributes);
}
