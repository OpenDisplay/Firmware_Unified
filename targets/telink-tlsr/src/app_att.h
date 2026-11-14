#ifndef APP_ATT_H_
#define APP_ATT_H_

/* Attribute handles. The order is the table order in app_att.c; handle 0 is its count row. */
typedef enum {
    ATT_H_START = 0,

    GenericAccess_PS_H,
    GenericAccess_DeviceName_CD_H,
    GenericAccess_DeviceName_DP_H,
    GenericAccess_Appearance_CD_H,
    GenericAccess_Appearance_DP_H,
    CONN_PARAM_CD_H,
    CONN_PARAM_DP_H,

    GenericAttribute_PS_H,
    GenericAttribute_ServiceChanged_CD_H,
    GenericAttribute_ServiceChanged_DP_H,
    GenericAttribute_ServiceChanged_CCB_H,

    OD_PS_H,          /* 0x2800  service 0x2446 */
    OD_CD_H,          /* 0x2803  write | write-without-response | notify */
    OD_DP_H,          /* 0x2446  the OpenDisplay pipe */
    OD_CCB_H,         /* 0x2902  notify subscription */

    OTA_PS_H,         /* 0x2800  Telink OTA service */
    OTA_CMD_OUT_CD_H, /* 0x2803  read | write | write-without-response | notify */
    OTA_CMD_OUT_DP_H, /* Telink OTA data -- ignored unless armed by OD command 0x0051 */
    OTA_CMD_INPUT_CCB_H,
    OTA_CMD_OUT_DESC_H,

    ATT_END_H,
} ATT_HANDLE;

void my_att_init(void);
void app_att_set_device_name(const unsigned char *name, unsigned char len);
int  app_att_notify_enabled(void);
void app_att_reset_subscriptions(void);

#endif /* APP_ATT_H_ */
