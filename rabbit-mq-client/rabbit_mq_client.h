/**
* @file         : rabbit_mq_client.h
* @brief        : This file contains headers and other declaration of
*                rabbit_mq_client.c
* @version      : 1.0
* @author       : VVDN Technologies
* @copyright    : (c) 2016-2017 , VVDN Technologies Pvt. Ltd.
*                Permission is hereby granted to everyone in VVDN Technologies
*                to use the Software without restriction, including without
*                limitation the rights to use, copy, modify, merge, publish,
*                distribute, distribute with modifications.
*/


#ifndef __RABBIT_MQ_CLIENT_H__
#define __RABBIT_MQ_CLIENT_H__


#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <limits.h>
#include <errno.h>
#include <pthread.h>
#include <sqlite3.h>
#include <signal.h>
#include <dirent.h>
#include <stdint.h>
#include <amqp_tcp_socket.h>
#include <amqp.h>
#include <amqp_framing.h>
#include <assert.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <curl/curl.h>

#include "msg_def.h"
#include "file_mgr.h"
#include "file_msg_drv.h"
#include "sys_env_type.h"
#include "common.h"
#include "json_fields.h"
#include "cloud_control.h"
#include "sem_util.h"
#include "sqlite_common.h"
#include "wdm_lib.h"
#include "utils.h"
#include "cJSON.h"

/**
* @macros   : generic macros
*/
#define MAX_SLEEP_TIME              1
#define EVENT_FLAG_ENABLE           1
#define EVENT_FLAG_DISABLE          -1
#define MAX_CMDNAME_LENGTH          128
#define MAX_SQL_STRING_LEN          512
#define MAX_SQL_VALUE_LEN           512
#define MAX_DOOR_LIST_LEN           256
#define MAX_PHONE_NUMBER_LIST_LEN   256
#define MAX_TIME_SLOTS_LEN          256
#define CLOUD_COMM_APP_WDM_TIMEOUT  20
#define MAX_EPOCH_LEN               16
#define MAX_SCHEDULE_LEN            128
#define MAX_SCHEDULE_STRING_LEN     1024
#define SHADOW_YIELD_TIMEOUT        400
#define MAX_FILENAME_LEN            64
#define MAX_FILENAME_COMPARE        14
#define ZEROTH_POSITION             0
#define READ_BUF_LEN                128
#define CONFIG_NAME_LEN             64
#define CONFIG_VALUE_LEN            128
#define MAX_CLOUD_COMM_FILENAME_LEN 128
#define PROC_PATH                   "/proc"
#define ACCESS_PROCESS_NAME         "access_service_app"
#define MAX_HOSTNAME_LEN            64
#define MAX_CERT_LEN                128
#define MAX_QUEUE_LEN               64
#define RABBIT_MQ_FILE_NAME         "/home/surabhi/rabbit_mq.cfg"
#define RABBIT_MQ_CERT_DIR          "surcerts"
#define MAX_RABBIT_MQ_FILENAME_LEN  64
#define MAX_RABBIT_MQ_USERNAME_LEN  64
#define MAX_RABBIT_MQ_PASSWORD_LEN  64
#define MAX_RECV_BUF_SZ             16384
#define MAX_URL_SZ                  128

/**
* @macros   : BASIC_SETTINGS_INIT
*/
#define BASIC_SETTINGS_INIT { \
    DEVICE_STATUS_INVALID, \
    TAMPER_DETECTION_INVALID, \
    MICROPHONE_SENSITIVITY_INVALID, \
    SPEAKER_VOLUME_INVALID, \
    INVALID_DEVICE, \
    INVALID_TIMEZONE, \
    BEEPER_INVALID, \
    {'\0'},\
    {'\0'} \
}

/**
* @macros   : CAMERA_SETTINGS_INIT
*/
#define CAMERA_SETTINGS_INIT { \
    {0,0,0,0,0,0,0,0}, \
    {'\0'}, \
    {'\0'}, \
}

/**
* @macros   : VIDEO_SETTINGS_INIT
*/
#define VIDEO_SETTINGS_INIT { \
    RES_INVALID, \
    FPS_INVALID, \
    INVALID_VALUE, \
    {EVENT_INVALID, \
        EVENT_INVALID, \
        EVENT_INVALID, \
        EVENT_INVALID, \
        EVENT_INVALID, \
        EVENT_INVALID, \
        EVENT_INVALID, \
        EVENT_INVALID, \
        EVENT_INVALID} \
}

/**
* @macros   : SNAPSHOT_SETTINGS_INIT
*/
#define SNAPSHOT_SETTINGS_INIT { \
    INVALID_FORMAT, \
    {EVENT_INVALID, \
        EVENT_INVALID, \
        EVENT_INVALID, \
        EVENT_INVALID, \
        EVENT_INVALID, \
        EVENT_INVALID, \
        EVENT_INVALID, \
        EVENT_INVALID, \
        EVENT_INVALID} \
}

/**
* @macros   : NOTIFICATION_SETTINGS_INIT
*/
#define NOTIFICATION_SETTINGS_INIT { \
    {EVENT_INVALID, \
        EVENT_INVALID, \
        EVENT_INVALID, \
        EVENT_INVALID, \
        EVENT_INVALID, \
        EVENT_INVALID, \
        EVENT_INVALID, \
        EVENT_INVALID, \
        EVENT_INVALID} \
}

/**
* @macros   : PHONECODE_SETTINGS_INIT
*/
#define PHONECODE_SETTINGS_INIT { \
    INVALID_CALL, \
    INVALID_MODE, \
    INVALID_VALUE, \
    INVALID_KEY \
}

/**
* @macros   : BLE_SETTINGS_INIT
*/
#define BLE_SETTINGS_INIT { \
    BLE_INVALID, \
    {'\0'}, \
    INVALID_VALUE \
}

/**
* @macros   : MOTION_SETTINGS_INIT
*/
#define MOTION_SETTINGS_INIT { \
    MOTION_DETECTION_INVALID, \
    INVALID_VALUE, \
    INVALID_VALUE \
}

/**
* @macros   : CLOUD_SETTINGS_INIT
*/
#define CLOUD_SETTINGS_INIT { \
    CLOUD_STORAGE_INVALID, \
    {'\0'}, \
    {'\0'}, \
    {'\0'}, \
    {'\0'}, \
    {'\0'}, \
    {'\0'}, \
    {'\0'} \
}

/**
* @macros   : KEYPAD_SETTINGS_INIT
*/
#define KEYPAD_SETTINGS_INIT { \
    CALL_BUTTON_INVALID, \
    SPECIAL_BUTTON_INVALID, \
    INVALID_VALUE, \
    INVALID_VALUE, \
    KEYPAD_LIGHT_INVALID \
}

/**
* @macros   : DISPLAY_SETTINGS_INIT
*/
#define DISPLAY_SETTINGS_INIT { \
    INVALID_FONT, \
    INVALID_VALUE, \
    {'\0'} \
}

/**
* @macros   : LOCKOUT_SETTINGS_INIT
*/
#define LOCKOUT_SETTINGS_INIT { \
    INVALID_VALUE, \
    INVALID_VALUE, \
    {LOCKOUT_INVALID, \
        LOCKOUT_INVALID, \
        LOCKOUT_INVALID, \
        LOCKOUT_INVALID, \
        LOCKOUT_INVALID} \
}

/**
* @macros   : NETWORK_SETTINGS_INIT
*/
#define NETWORK_SETTINGS_INIT { \
    0, \
    0, \
    0 \
}

/**
* @macros   : COURTESYLIGHT_SETTINGS_INIT
*/
#define COURTESYLIGHT_SETTINGS_INIT { \
    INVALID_VALUE, \
    COURTESY_LIGHT_INVALID \
}

/**
* @macros   : SCHEDULE_SETTINGS_INIT
*/
#define SCHEDULE_SETTINGS_INIT { \
    INVALID_VALUE, \
    INVALID_VALUE, \
    INVALID_VALUE, \
    INVALID_VALUE \
}

/**
* @macros   : SIP_SETTINGS_INIT
*/
#define SIP_SETTINGS_INIT { \
    {'\0'}, \
    {'\0'}, \
    {'\0'} \
}

/**
* @enums        : CMD_TYPE
*/
typedef enum {
    ADD_CMD,            // Add command
    EDIT_CMD,           // Edit command
    DELETE_CMD          // Delete command
} CMD_TYPE;

/**
* @enums        : RELAY_ID
*/
typedef enum {
    FIRST_RELAY,        // RELAY 1
    SECOND_RELAY,       // RELAY 2
    THIRD_RELAY,        // RELAY 3
    FOURTH_RELAY        // RELAY 4
} RELAY_ID;


/**
* @structure            : rabbitMqCfg_t
* @brief                : rabbit MQ configurations
* @members              :
*   rabbitMqHostname    : Rabbit MQ Hostname
*   rabbitMqPort        : Rabbbit MQ Port
*   rabbitMqCACert      : Rabbit MQ CA Cert
*   rabbitMqClientCert  : Rabbit MQ Client Cert
*   rabbitMqClientKey   : Rabbit MQ Client Key
*   rabbitMqQueue       : Rabbit MQ Queue
*/
typedef struct {
    int rabbitMqPort;
    char rabbitMqHostname[MAX_HOSTNAME_LEN];
    char rabbitMqCACert[MAX_CERT_LEN];
    char rabbitMqClientCert[MAX_CERT_LEN];
    char rabbitMqClientKey[MAX_CERT_LEN];
    char rabbitMqQueue[MAX_QUEUE_LEN];
    char rabbitMqUSername[MAX_RABBIT_MQ_USERNAME_LEN];
    char rabbitMqPassword[MAX_RABBIT_MQ_PASSWORD_LEN];
} rabbitMqCfg_t;

/**
* @globals      : DB handler
*/
sqlite3 *gDbHandle = NULL;

/**
 * @globals      : cloud command list
 */
char cmdList[][MAX_CMDNAME_LENGTH] = {
    // Access Code Command
    "ACCESS_CODE_ADD",
    "ACCESS_CODE_EDIT",
    "ACCESS_CODE_DELETE",

    // Phone Code Command
    "PHONE_CODE_ADD",
    "PHONE_CODE_EDIT",
    "PHONE_CODE_DELETE",

    // BLE Code Command
    "BLE_CODE_ADD",
    "BLE_CODE_EDIT",
    "BLE_CODE_DELETE",

    // NFC Code Command
    "NFC_CODE_ADD",
    "NFC_CODE_EDIT",
    "NFC_CODE_DELETE",

    // RFID Code Command
    "RFID_CODE_ADD",
    "RFID_CODE_EDIT",
    "RFID_CODE_DELETE",

    // RFID Code Command
    "WIEGAND_CODE_ADD",
    "WIEGAND_CODE_EDIT",
    "WIEGAND_CODE_DELETE",

    // User Command
    "USER_ADD",
    "USER_EDIT",
    "USER_DELETE",

    // Door Schedule Command
    "ADD_USER_DOOR_SCHEDULE",
    "EDIT_USER_DOOR_SCHEDULE",
    "DELETE_USER_DOOR_SCHEDULE",

    // Door Mapping Command
    "DOOR_ADD",
    "DOOR_DELETE",

    // Camera IP Mapping Command
    "CAMERA_ADD",
    "CAMERA_EDIT",
    "CAMERA_DELETE",

    // Relay Mapping Command
    "RELAY_MAPPING",

    // Device setting Command
    "DEVICE_SETTINGS"
};


/**
* @threads declaration
*/
void *WDTClientThread(void *arg);

/**
* @function headers declaration
*/
bool loadRabbitMqConfiguration(rabbitMqCfg_t *pRabbitMqCfg);
void downloadCerts(const char *queuename);
void jsonParser(size_t length, void const *value);
void cloudCommandParser(cJSON *parser);
void recvCmdHandler(cJSON *cmd);
void accessCodeCmd(cJSON *cmd, int mode);
void phoneCodeCmd(cJSON *cmd, int mode);
void bleCodeCmd(cJSON *cmd, int mode);
void nfcCodeCmd(cJSON *cmd, int mode);
void rfidCodeCmd(cJSON *cmd, int mode);
void wiegandCodeCmd(cJSON *cmd, int mode);
void userCmd(cJSON *cmd, int mode);
void userDoorScheduleCmd(cJSON *cmd, int mode);
void cameraCmd(cJSON *cmd, int mode);
void relayMappingCmd(cJSON *cmd);
void relayMapping(cJSON *cmd, int realyId);
void deviceSettingsCmd(cJSON *cmd);
void timeSlots(cJSON *schedule, int timeSlotLength, char *dayOfWeek);
void processAccessEvent(cJSON *deviceSetting, EVENT_TYPE *recordEvent);
void processLockoutMode(cJSON *deviceSetting, LOCKOUT_MODE *lockoutMode);
void processDoorArray(cJSON *deviceSetting, int *doors);
int dataFixUp(int dataLen, int length, char *data);
int getPid(const char* processName);


#endif  /* __RABBIT_MQ_CLIENT_H__ */
