/**
 * @file         : rabbit_mq_client.c
 * @brief        : This file contains source code of rabbit mq client.
 *                Rabbit MQ Client allows communication between cloud and
 *                device.
 * @version      : 1.0
 * @author       : VVDN Technologies
 * @copyright    : (c) 2016-2017, VVDN Technologies Pvt. Ltd.
 *                Permission is hereby granted to everyone in VVDN Technologies
 *                to use the Software without restriction, including without
 *                limitation the rights to use, copy, modify, merge, publish,
 *                distribute, distribute with modifications.
 */

#include"rabbit_mq_client.h"

/**
 * @function     : main
 * @param1[in]   : argc
 *                contains the number of arguments passed to the program
 * @param2[in]   : argv
 *                contains arguments passed to the program
 * @return       : returns 0 on success otherwise failure
 */
int main(int argc, char const *const *argv)
{
    int rc;
    int port, status;
    char const *hostname;
    char const *queuename;
    amqp_socket_t *socket;
    amqp_connection_state_t conn;
    rabbitMqCfg_t rabbitMqCfg;

    deviceConfData_t *devConf = NULL;

    /* Load Cloud Configuration */
    memset(&rabbitMqCfg, 0, sizeof(rabbitMqCfg_t));
    if (false == loadRabbitMqConfiguration(&rabbitMqCfg)) {
        printf("Failed to load rabbit Mq configurations\n");
        exit(EXIT_FAILURE);
    }
    hostname = rabbitMqCfg.rabbitMqHostname;
    port = rabbitMqCfg.rabbitMqPort;
    queuename = rabbitMqCfg.rabbitMqQueue;

    // Download certs for rabbitMQ
    downloadCerts(queuename);

    //Thread Attributes
    pthread_t wdtClientThrId;
    pthread_attr_t wdtClientThrAttr;

    // Send REGISTER to WDM
    wdmMessage(REGISTER, CLOUD_COMMUNICATION_APP, CLOUD_COMM_APP_WDM_TIMEOUT,
            WDM_CCA_MSG);

    // Create WDTClientThread
    rc = pthread_attr_init(&wdtClientThrAttr);
    if (0 != rc) {
        printf("WDTClientThread attribute creation failed!\n");
        exit(EXIT_FAILURE);
    }

    rc = pthread_attr_setdetachstate(&wdtClientThrAttr,
            PTHREAD_CREATE_DETACHED);
    if (0 != rc) {
        printf("Setting detached state for WDTClientThread attribute "
                "failed!\n");
        exit(EXIT_FAILURE);
    }

    rc = pthread_create(&wdtClientThrId, &wdtClientThrAttr,
            WDTClientThread, NULL);
    if (0 != rc) {
        printf("WDTClientThread creation failed!\n");
        exit(EXIT_FAILURE);
    }

    // Initialize Shared Memory
    if (initFileMsgDrv(DEVICE_CONF_KEY) < 0) {
        printf("FileMsg Driver initialization failed\n");
        return -1;
    }

    // Get Shared Memory Reference
    devConf = getDevConf();

    // Initialize/Read Device Configuration
    fileMgrInit(devConf);

    // Set Semaphore Value
    setSemVal();

    // Establish connection with RabbitMQ Server
    conn = amqp_new_connection();

    socket = amqp_ssl_socket_new(conn);
    if (!socket) {
        printf("Creating SSL/TLS socket failed!\n");
        exit(EXIT_FAILURE);
    }

    status = amqp_ssl_socket_set_cacert(socket, rabbitMqCfg.rabbitMqCACert);
    if (status) {
        printf("Setting CA certificate failed!\n");
        exit(EXIT_FAILURE);
    }

    amqp_ssl_socket_set_verify_peer(socket, 1);
    // TODO: Enable this when cloud supported
    amqp_ssl_socket_set_verify_hostname(socket, 0);

    status = amqp_ssl_socket_set_key(socket, rabbitMqCfg.rabbitMqClientCert,
            rabbitMqCfg.rabbitMqClientKey);
    if (status) {
        printf("Setting client cert failed!\n");
        exit(EXIT_FAILURE);
    }

    status = amqp_socket_open(socket, hostname, port);
    if (status) {
        printf("Opening SSL/TLS connection failed!\n");
        exit(EXIT_FAILURE);
    }

    die_on_amqp_error(amqp_login(conn, "/", 0, 131072, 0,
                AMQP_SASL_METHOD_PLAIN, rabbitMqCfg.rabbitMqPassword,
                rabbitMqCfg.rabbitMqUSername),
            "Logging in");
    amqp_channel_open(conn, 1);
    die_on_amqp_error(amqp_get_rpc_reply(conn), "Opening channel!");


    amqp_basic_consume(conn, 1, amqp_cstring_bytes(queuename), amqp_empty_bytes,
            0, 1, 0, amqp_empty_table);
    die_on_amqp_error(amqp_get_rpc_reply(conn), "Consuming!");

    {
        for (;;) {
            amqp_rpc_reply_t res;
            amqp_envelope_t envelope;

            amqp_maybe_release_buffers(conn);

            res = amqp_consume_message(conn, &envelope, NULL, 0);
            if (AMQP_RESPONSE_NORMAL != res.reply_type) {
                break;
            }

            printf("Delivery %u, exchange %.*s routingkey %.*s\n",
                    (unsigned) envelope.delivery_tag,
                    (int) envelope.exchange.len, (char *) envelope.exchange.bytes,
                    (int) envelope.routing_key.len, (char *) envelope.routing_key.bytes);

            if (envelope.message.properties._flags & AMQP_BASIC_CONTENT_TYPE_FLAG) {
                printf("Content-type: %.*s\n",
                        (int) envelope.message.properties.content_type.len,
                        (char *) envelope.message.properties.content_type.bytes);
            }

            jsonParser(envelope.message.body.len, envelope.message.body.bytes);
            amqp_destroy_envelope(&envelope);
            fflush(stdout);
            fflush(stderr);
        }
    }

    die_on_amqp_error(amqp_channel_close(conn, 1, AMQP_REPLY_SUCCESS),
            "Closing channel");
    die_on_amqp_error(amqp_connection_close(conn, AMQP_REPLY_SUCCESS),
            "Closing connection");
    die_on_error(amqp_destroy_connection(conn), "Ending connection");


    return 0;
}

/**
* @function     : loadRabbitMqConfiguration
* @param1[in]   : pRabbitMqCfg
*                pointer holding cloud configurations
* @return       : returns true on success, otherwise false
*/
bool loadRabbitMqConfiguration(rabbitMqCfg_t *pRabbitMqCfg)
{
    size_t len;
    bool ret = false;
    FILE *fPtr = NULL;
    char readBuf[READ_BUF_LEN];
    char configName[CONFIG_NAME_LEN];
    char configVal[CONFIG_VALUE_LEN];
    char rabbitMqCfgName[MAX_RABBIT_MQ_FILENAME_LEN];

    // Initialize
    len = 0;

    snprintf(rabbitMqCfgName, MAX_RABBIT_MQ_FILENAME_LEN,
            RABBIT_MQ_FILE_NAME);
    fPtr = fopen(rabbitMqCfgName, "r");
    if (!fPtr) {
        printf("Can't open configuration file %s. Error [%s]\n",
                rabbitMqCfgName, strerror(errno));
        goto error;
    }

    // Read cloud configuration
    while (fgets(readBuf, READ_BUF_LEN, fPtr)) {
        len = strlen(readBuf) - 1;
        while (isspace(readBuf[len])) {
            readBuf[len] = 0;
            len--;
        }

        if (sscanf(readBuf, "%s = %[^\n]s", configName, configVal) == 2) {
            if (!strcasecmp(configName, "RABBIT_MQ_HOSTNAME")) {
                strcpy(pRabbitMqCfg->rabbitMqHostname, configVal);
                printf("RABBIT_MQ_HOSTNAME: [%s]\n",
                        pRabbitMqCfg->rabbitMqHostname);
            } else if (!strcasecmp(configName, "RABBIT_MQ_PORT")) {
                pRabbitMqCfg->rabbitMqPort = atoi(configVal);
                printf("RABBIT_MQ_PORT: [%d]\n", pRabbitMqCfg->rabbitMqPort);
            } else if (!strcasecmp(configName, "RABBIT_MQ_CA_CERT")) {
                strcpy(pRabbitMqCfg->rabbitMqCACert, configVal);
                printf("RABBIT_MQ_CA_CERT: [%s]\n",
                        pRabbitMqCfg->rabbitMqCACert);
            }else if (!strcasecmp(configName, "RABBIT_MQ_CLIENT_KEY")) {
                strcpy(pRabbitMqCfg->rabbitMqClientKey, configVal);
                printf("RABBIT_MQ_CLIENT_KEY: [%s]\n",
                        pRabbitMqCfg->rabbitMqClientKey);
            } else if (!strcasecmp(configName, "RABBIT_MQ_CLIENT_CERT")) {
                strcpy(pRabbitMqCfg->rabbitMqClientCert, configVal);
                printf("RABBIT_MQ_CLIENT_CERT: [%s]\n",
                        pRabbitMqCfg->rabbitMqClientCert);
            } else if (!strcasecmp(configName, "RABBIT_MQ_QUEUE")) {
                strcpy(pRabbitMqCfg->rabbitMqQueue, configVal);
                printf("RABBIT_MQ_QUEUE: [%s]\n",
                        pRabbitMqCfg->rabbitMqQueue);
            } else if (!strcasecmp(configName, "RABBIT_MQ_USERNAME")) {
                strcpy(pRabbitMqCfg->rabbitMqUSername, configVal);
                printf("RABBIT_MQ_USERNAME: [%s]\n",
                        pRabbitMqCfg->rabbitMqUSername);
            } else if (!strcasecmp(configName, "RABBIT_MQ_PASSWORD")) {
                strcpy(pRabbitMqCfg->rabbitMqPassword, configVal);
                printf("RABBIT_MQ_PASSWORD: [%s]\n",
                        pRabbitMqCfg->rabbitMqPassword);
            }
        }
    }

    // Sanity checks
    if (!pRabbitMqCfg->rabbitMqHostname[0] || !pRabbitMqCfg->rabbitMqCACert[0] ||
            !pRabbitMqCfg->rabbitMqClientKey[0] ||
            !pRabbitMqCfg->rabbitMqClientCert[0] ||
            !pRabbitMqCfg->rabbitMqQueue[0] ||
            !pRabbitMqCfg->rabbitMqPort ||
            !pRabbitMqCfg->rabbitMqUSername ||
            !pRabbitMqCfg->rabbitMqPassword) {
        printf("Missing configuration parameters or "
                "invalid configuration\n");
        goto error;
    }
    ret = true;

error:
    if (fPtr) {
        fclose(fPtr);
    }

    return ret;
}

/*
 * @function     : downloadCerts
 * @brief        : download certs from cloud
 * @param1[in]   : queuename
 *                rabbitMQ queue namq
 * @return       : none
 */
void downloadCerts(const char *queuename)
{
    char url[MAX_URL_SZ];
    DIR *dir;

    // open cert directory
    dir = opendir(RABBIT_MQ_CERT_DIR);
    if (!dir) {
        mkdir(RABBIT_MQ_CERT_DIR, 0777);
        chdir(RABBIT_MQ_CERT_DIR);
        //system("wget  --no-directories -r http://www.textfiles.com/100/");
        system("wget  http://www.textfiles.com/100/ad.txt");
        chdir("../");
        system("pwd");

    }
}

/*
 * @function     : jsonParser
 * @brief        : get data from the queue
 * @param1[in]   : length
 *                length of received data
 * @param2[in]   : value
 *                received data
 * @return       : none
 */
void jsonParser(size_t length, void const *value)
{
    int len;
    char temp[MAX_RECV_BUF_SZ];
    cJSON *parser = NULL;

    // Initialisation
    len = 0;
    parser = NULL;

    memset(temp, '\0', MAX_RECV_BUF_SZ);
    len = sprintf(temp, "%.*s", (int)length, (char*)value);
    temp[len+1] = '\0';

    printf("Received JSON: [%s]\n", temp);

    // Get JSON parser
    parser = cJSON_Parse(temp);
    if (NULL != parser) {
        cloudCommandParser(parser);
    } else {
        printf("Invalid JSON recieved!\n");
    }
    cJSON_Delete(parser);
}

/**
 * @function     : cloudCommandParser
 * @brief        : this parses the received command & performs the
 *                required functionalities depending upon whether it's a Device Setting or
 *                Device Data
 * @param1[in]   : parser
 *                received command in cJSON format
 * @return       : none
 */
void cloudCommandParser(cJSON *parser)
{
    int idx, cmdIdx;
    bool cmdFound;
    cJSON *getcJSONObject, *getcJSONArray, *cmd;

    // Initialize
    idx = cmdIdx = 0;
    cmdFound = false;

    for (idx=0; idx<cJSON_GetArraySize(parser); idx++) {
        getcJSONArray = cJSON_GetArrayItem(parser, idx);
        cmdFound = false;
        // Process received cloud command
        for (cmdIdx=0; cmdIdx<sizeof(cmdList)/sizeof(cmdList[0]) &&
                !cmdFound; cmdIdx++) {
            cmd = cJSON_GetObjectItem(getcJSONArray, cmdList[cmdIdx]);
            if (NULL != cmd) {
                printf("cmd value %s\n", cmd->string);
                cmdFound = true;
                recvCmdHandler(cmd);
                break;
            }
        }
    }
}

/**
 * @function     : recvCmdHandler
 * @brief        : this parses the received Device Data from Cloud
 * @param1[in]   : cmd
 *                received device data in cJSON format
 * @return       : none
 */
void recvCmdHandler(cJSON *cmd)
{
    // Access Code Command
    if (!strcmp(cmd->string, ACCESS_CODE_ADD_JF)) {
        printf("ACCESS_CODE_ADD command received\n");
        accessCodeCmd(cmd, ADD_CMD);
    } else if (!strcmp(cmd->string, ACCESS_CODE_EDIT_JF)) {
        printf("ACCESS_CODE_EDIT command received\n");
        accessCodeCmd(cmd, EDIT_CMD);
    } else if (!strcmp(cmd->string, ACCESS_CODE_DELETE_JF)) {
        printf("ACCESS_CODE_DELETE command received\n");
        accessCodeCmd(cmd, DELETE_CMD);
    // Phone Code Command
    } else if (!strcmp(cmd->string, PHONE_CODE_ADD_JF)){
        printf("PHONE_CODE_ADD command received\n");
        phoneCodeCmd(cmd, ADD_CMD);
    } else if (!strcmp(cmd->string, PHONE_CODE_EDIT_JF)) {
        printf("PHONE_CODE_EDIT command received\n");
        phoneCodeCmd(cmd, EDIT_CMD);
    } else if (!strcmp(cmd->string, PHONE_CODE_DELETE_JF)) {
        printf("PHONE_CODE_DELETE command received\n");
        phoneCodeCmd(cmd, DELETE_CMD);
    // BLE Code Command
    } else if (!strcmp(cmd->string, BLE_CODE_ADD_JF)) {
        printf("BLE_CODE_ADD command received\n");
        bleCodeCmd(cmd, ADD_CMD);
    } else if (!strcmp(cmd->string, BLE_CODE_EDIT_JF)) {
        printf("BLE_CODE_EDIT command received\n");
        bleCodeCmd(cmd, EDIT_CMD);
    } else if (!strcmp(cmd->string, BLE_CODE_DELETE_JF)) {
        printf("BLE_CODE_DELETE command received\n");
        bleCodeCmd(cmd, DELETE_CMD);
    // NFC Code Command
    } else if (!strcmp(cmd->string, NFC_CODE_ADD_JF)) {
        printf("NFC_CODE_ADD command received\n");
        nfcCodeCmd(cmd, ADD_CMD);
    } else if (!strcmp(cmd->string, NFC_CODE_EDIT_JF)) {
        printf("NFC_CODE_EDIT command received\n");
        nfcCodeCmd(cmd, EDIT_CMD);
    } else if (!strcmp(cmd->string, NFC_CODE_DELETE_JF)) {
        printf("NFC_CODE_DELETE command received\n");
        nfcCodeCmd(cmd, DELETE_CMD);
    // RFID Code Command
    } else if (!strcmp(cmd->string, RFID_CODE_ADD_JF)) {
        printf("RFID_CODE_ADD command received\n");
        rfidCodeCmd(cmd, ADD_CMD);
    } else if (!strcmp(cmd->string, RFID_CODE_EDIT_JF)) {
        printf("RFID_CODE_EDIT command received\n");
        rfidCodeCmd(cmd, EDIT_CMD);
    } else if (!strcmp(cmd->string, RFID_CODE_DELETE_JF)) {
        printf("RFID_CODE_DELETE command received\n");
        rfidCodeCmd(cmd, DELETE_CMD);
    // Wiegand Code Command
    } else if (!strcmp(cmd->string, WIEGAND_CODE_ADD_JF)) {
        printf("WIEGAND_CODE_ADD command received\n");
        wiegandCodeCmd(cmd, ADD_CMD);
    } else if (!strcmp(cmd->string, WIEGAND_CODE_EDIT_JF)) {
        printf("WIEGAND_CODE_EDIT command received\n");
        wiegandCodeCmd(cmd, EDIT_CMD);
    } else if (!strcmp(cmd->string, WIEGAND_CODE_DELETE_JF)) {
        printf("WIEGAND_CODE_DELETE command received\n");
        wiegandCodeCmd(cmd, DELETE_CMD);
    // User Command
    } else if (!strcmp(cmd->string, USER_ADD_JF)) {
        printf("USER_ADD command received\n");
        userCmd(cmd, ADD_CMD);
    } else if (!strcmp(cmd->string, USER_EDIT_JF)) {
        printf("USER_EDIT command received\n");
        userCmd(cmd, EDIT_CMD);
    } else if (!strcmp(cmd->string, USER_DELETE_JF)) {
        printf("USER_DELETE command received\n");
        userCmd(cmd, DELETE_CMD);
    // Door Schedule Command
    } else if (!strcmp(cmd->string, ADD_USER_DOOR_SCHEDULE_JF)) {
        printf("ADD_USER_DOOR_SCHEDULE command received\n");
        userDoorScheduleCmd(cmd, ADD_CMD);
    } else if (!strcmp(cmd->string, EDIT_USER_DOOR_SCHEDULE_JF)) {
        printf("EDIT_USER_DOOR_SCHEDULE command received\n");
        userDoorScheduleCmd(cmd, EDIT_CMD);
    } else if (!strcmp(cmd->string, DELETE_USER_DOOR_SCHEDULE_JF)) {
        printf("DELETE_USER_DOOR_SCHEDULE command received\n");
        userDoorScheduleCmd(cmd, DELETE_CMD);
    // Camera IP Mapping Command
    } else if (!strcmp(cmd->string, CAMERA_ADD_JF)) {
        printf("CAMERA_ADD command received\n");
        cameraCmd(cmd, ADD_CMD);
    } else if (!strcmp(cmd->string, CAMERA_EDIT_JF)) {
        printf("CAMERA_EDIT command received\n");
        cameraCmd(cmd, EDIT_CMD);
    } else if (!strcmp(cmd->string, CAMERA_DELETE_JF)) {
        printf("CAMERA_DELETE command received\n");
        cameraCmd(cmd, DELETE_CMD);
    // Relay Mapping Command
    } else if (!strcmp(cmd->string, RELAY_MAPPING_JF)) {
        printf("RELAY MAPPING command received\n");
        relayMappingCmd(cmd);
    } else if (!strcmp(cmd->string, DEVICE_SETTINGS_CMD_JF)) {
        // Process device settings cloud command
        deviceSettingsCmd(cmd);
    } else {
        // Shouldn't reach here
        printf("Illegal Command\n");
    }
}

/**
 * @function     : accessCodeCmd
 * @brief        : parses the Access Code Command & performs the required
 *                operation on DB
 * @param1[in]   : cmd
 *                received Keypad Access related data
 * @param2[in]   : cmdType
 *                whether it's an ADD, EDIT or DELETE operation
 * @return       : none
 */
void accessCodeCmd(cJSON *cmd, int cmdType)
{
    int idx;
    int userId;
    int credentialId;
    int keypadAccessCode;
    int keypadAccessCodeStatus;
    int sqlStringLen;
    int valStringLen;
    int doorListLen;
    char *serialNumber;
    char doorList[MAX_DOOR_LIST_LEN];
    char sqlString[MAX_SQL_STRING_LEN];
    char valString[MAX_SQL_VALUE_LEN];
    cJSON *params, *doorId;

    // Initialize
    userId = 0;
    credentialId = 0;
    keypadAccessCode = 0;
    keypadAccessCodeStatus = -1;
    sqlStringLen = valStringLen = doorListLen = 0;
    serialNumber = NULL;

    memset(sqlString, '\0', MAX_SQL_STRING_LEN);
    memset(valString, '\0', MAX_SQL_VALUE_LEN);
    memset(doorList, '\0', MAX_DOOR_LIST_LEN);

    switch (cmdType) {
        case ADD_CMD:               // Add
            params = cJSON_GetObjectItem(cmd, SERIAL_NUMBER_JF);
            if (NULL != params) {
                serialNumber = params->valuestring;
                printf("Serial Number: [%s]\n", serialNumber);
            }

            params = cJSON_GetObjectItem(cmd, CREDENTIAL_ID_JF);
            if (NULL != params) {
                credentialId = params->valueint;
                printf("Credential ID: [%d]\n", credentialId);
            }

            params = cJSON_GetObjectItem(cmd, DOORS_JF);
            if (NULL != params) {
                for (idx = 0; idx < cJSON_GetArraySize(params); idx++) {
                    if (doorListLen >= MAX_DOOR_LIST_LEN) {
                        doorListLen = dataFixUp(MAX_DOOR_LIST_LEN, doorListLen,
                                doorList);
                        break;
                    }
                    doorId = cJSON_GetArrayItem(params, idx);
                    doorListLen += sprintf(doorList+doorListLen,
                            "%d,",doorId->valueint);
                }
                if (doorListLen >= MAX_DOOR_LIST_LEN) {
                    doorListLen = dataFixUp(MAX_DOOR_LIST_LEN, doorListLen,
                            doorList);
                }
                doorList[--doorListLen] = '\0';
                printf("Door List: [%s]\n", doorList);
            }

            params = cJSON_GetObjectItem(cmd, USER_ID_JF);
            if (NULL != params) {
                userId = params->valueint;
                printf("User ID: [%d]\n", userId);
            }

            params = cJSON_GetObjectItem(cmd, ACCESS_CODE_JF);
            if (NULL != params) {
                keypadAccessCode = params->valueint;
                printf("Access Code: [%d]\n", keypadAccessCode);
            }

            params = cJSON_GetObjectItem(cmd, STATUS_JF);
            if (NULL != params) {
                keypadAccessCodeStatus = params->valueint;
                printf("Code Status: [%d]\n", keypadAccessCodeStatus);
            }

            // SQL Insert
            sprintf(sqlString, "INSERT INTO keypad_access VALUES"
                    " (%d,%d,%d,\"%s\",%d)", keypadAccessCode, credentialId,
                     userId, doorList,
                keypadAccessCodeStatus);
            executeSqlQuery(gDbHandle, sqlString);

            break;

        case EDIT_CMD:              // Edit
            params = cJSON_GetObjectItem(cmd, SERIAL_NUMBER_JF);
            if (NULL != params) {
                serialNumber = params->valuestring;
                printf("Serial Number: [%s]\n", serialNumber);
            }

            params = cJSON_GetObjectItem(cmd, CREDENTIAL_ID_JF);
            if (NULL != params) {
                credentialId = params->valueint;
                printf("Credential ID: [%d]\n", credentialId);
            }

            params = cJSON_GetObjectItem(cmd, DOORS_JF);
            if (NULL != params) {
                for (idx = 0; idx < cJSON_GetArraySize(params); idx++) {
                    if (doorListLen >= MAX_DOOR_LIST_LEN) {
                        doorListLen = dataFixUp(MAX_DOOR_LIST_LEN, doorListLen,
                                doorList);
                        break;
                    }
                    doorId = cJSON_GetArrayItem(params, idx);
                    doorListLen += sprintf(doorList+doorListLen,
                            "%d,",doorId->valueint);
                }
                if (doorListLen >= MAX_DOOR_LIST_LEN) {
                    doorListLen = dataFixUp(MAX_DOOR_LIST_LEN, doorListLen,
                            doorList);
                }
                doorList[--doorListLen] = '\0';
                printf("Door List: [%s]\n", doorList);
            }

            params = cJSON_GetObjectItem(cmd, USER_ID_JF);
            if (NULL != params) {
                userId = params->valueint;
                printf("User ID: [%d]\n", userId);
            }

            params = cJSON_GetObjectItem(cmd, ACCESS_CODE_JF);
            if (NULL != params) {
                keypadAccessCode = params->valueint;
                printf("Access Code: [%d]\n", keypadAccessCode);
            }

            params = cJSON_GetObjectItem(cmd, STATUS_JF);
            if (NULL != params) {
                keypadAccessCodeStatus = params->valueint;
                printf("Code Status: [%d]\n", keypadAccessCodeStatus);
            }

            // SQL Update
            sqlStringLen = sprintf(sqlString+sqlStringLen, "UPDATE"
                    " keypad_access SET ");
            if (keypadAccessCode) {
                valStringLen += sprintf(valString+valStringLen, " %s=%d,",
                        "keypad_access_code", keypadAccessCode);
            }
            if (doorList) {
                valStringLen += sprintf(valString+valStringLen, " %s=\"%s\",",
                        "door_id", doorList);
            }

            if (keypadAccessCodeStatus != -1) {
                valStringLen += sprintf(valString+valStringLen, "%s=%d,",
                        "code_status", keypadAccessCodeStatus);
            }

            valString[--valStringLen] = '\0';
            sqlStringLen += sprintf(sqlString+sqlStringLen, "%s WHERE"
                    " user_id=%d AND keypad_credential_id=%d", valString,
                    userId, credentialId);
            executeSqlQuery(gDbHandle, sqlString);

            break;

        case DELETE_CMD:                // Delete
            params = cJSON_GetObjectItem(cmd, SERIAL_NUMBER_JF);
            if (NULL != params) {
                serialNumber = params->valuestring;
                printf("Serial Number: [%s]\n", serialNumber);
            }

            params = cJSON_GetObjectItem(cmd, CREDENTIAL_ID_JF);
            if (NULL != params) {
                credentialId = params->valueint;
                printf("Credential ID: [%d]\n", credentialId);
            }

            // SQL Delete
            sprintf(sqlString, "DELETE FROM keypad_access WHERE"
                    " keypad_credential_id=%d", credentialId);
            executeSqlQuery(gDbHandle, sqlString);

            break;

        default:
            // Shouldn't reach here
            printf("Illegal\n");

            break;
    }
}

/**
 * @function     : phoneCodeCmd
 * @brief        : parses the Phone Code Command & performs the required
 *                operation on DB
 * @param1[in]   : cmd
 *                received Phone Code Access related data
 * @param2[in]   : cmdType
 *                whether it's an ADD, EDIT or DELETE operation
 * @return       : none
 */
void phoneCodeCmd(cJSON *cmd, int cmdType)
{
    int idx;
    int userId;
    CALL_HUNT_TYPE huntType;
    int phoneCode;
    int credentialId;
    int phoneCodeStatus;
    int sqlStringLen;
    int valStringLen;
    int doorListLen;
    int phoneNumberListLen;
    char *serialNumber;
    char sqlString[MAX_SQL_STRING_LEN];
    char valString[MAX_SQL_VALUE_LEN];
    char doorList[MAX_DOOR_LIST_LEN];
    char phoneNumberList[MAX_PHONE_NUMBER_LIST_LEN];
    cJSON *params, *doorId, *phoneNumber;

    // Initialize
    userId = 0;
    huntType = CALL_HUNT_TYPE_INVALID;
    phoneCode = 0;
    credentialId = 0;
    phoneCodeStatus = -1;
    phoneNumberListLen = 0;
    sqlStringLen = valStringLen = doorListLen = 0;
    serialNumber = NULL;

    memset(sqlString, '\0', MAX_SQL_STRING_LEN);
    memset(valString, '\0', MAX_SQL_VALUE_LEN);
    memset(phoneNumberList, '\0', MAX_PHONE_NUMBER_LIST_LEN);
    memset(doorList, '\0', MAX_DOOR_LIST_LEN);

    switch (cmdType) {
        case ADD_CMD:               // Add
            params = cJSON_GetObjectItem(cmd, SERIAL_NUMBER_JF);
            if (NULL != params) {
                serialNumber = params->valuestring;
                printf("Serial Number: [%s]\n", serialNumber);
            }

            params = cJSON_GetObjectItem(cmd, CREDENTIAL_ID_JF);
            if (NULL != params) {
                credentialId = params->valueint;
                printf("Credential ID: [%d]\n", credentialId);
            }

            params = cJSON_GetObjectItem(cmd, DOORS_JF);
            if (NULL != params) {
                for (idx = 0; idx < cJSON_GetArraySize(params); idx++) {
                    if (doorListLen >= MAX_DOOR_LIST_LEN) {
                        doorListLen = dataFixUp(MAX_DOOR_LIST_LEN, doorListLen,
                                doorList);
                        break;
                    }
                    doorId = cJSON_GetArrayItem(params, idx);
                    doorListLen += sprintf(doorList+doorListLen,
                            "%d,",doorId->valueint);
                }
                if (doorListLen >= MAX_DOOR_LIST_LEN) {
                    doorListLen = dataFixUp(MAX_DOOR_LIST_LEN, doorListLen,
                            doorList);
                }
                doorList[--doorListLen] = '\0';
                printf("Door List: [%s]\n", doorList);
            }

            params = cJSON_GetObjectItem(cmd, USER_ID_JF);
            if (NULL != params) {
                userId = params->valueint;
                printf("User ID: [%d]\n", userId);
            }

            params = cJSON_GetObjectItem(cmd, PHONE_CODE_JF);
            if (NULL != params) {
                phoneCode = params->valueint;
                printf("Phone Code: [%d]\n", phoneCode);
            }

            params = cJSON_GetObjectItem(cmd, HUNT_TYPE_JF);
            if (NULL != params) {
                if (params->valueint == CALL_HUNT_TYPE_SERIAL) {
                    huntType = CALL_HUNT_TYPE_SERIAL;
                } else if (params->valueint == CALL_HUNT_TYPE_PARALLEL) {
                    huntType = CALL_HUNT_TYPE_PARALLEL;
                } else {
                    huntType = CALL_HUNT_TYPE_INVALID;
                }
                printf("Hunt Type: [%d]\n", huntType);
            }

            params = cJSON_GetObjectItem(cmd, STATUS_JF);
            if (NULL != params) {
                phoneCodeStatus = params->valueint;
                printf("Code Status: [%d]\n", phoneCodeStatus);
            }

            params = cJSON_GetObjectItem(cmd, PHONE_NUMBERS_JF);
            if (NULL != params) {
                for (idx = 0; idx < cJSON_GetArraySize(params); idx++) {
                    if (phoneNumberListLen >= MAX_PHONE_NUMBER_LIST_LEN) {
                        dataFixUp(MAX_PHONE_NUMBER_LIST_LEN, phoneNumberListLen,
                                phoneNumberList);
                        break;
                    }
                    phoneNumber = cJSON_GetArrayItem(params, idx);
                    phoneNumberListLen += sprintf(phoneNumberList+phoneNumberListLen,
                            "%s,",phoneNumber->valuestring);
                }
                if (phoneNumberListLen >= MAX_PHONE_NUMBER_LIST_LEN) {
                    dataFixUp(MAX_PHONE_NUMBER_LIST_LEN, phoneNumberListLen,
                            phoneNumberList);
                }
                phoneNumberList[--phoneNumberListLen] = '\0';
                printf("Phone Number List [%s]\n", phoneNumberList);
            }

            // SQL Insert
            sprintf(sqlString, "INSERT INTO phone_access values"
                    " (%d,%d,%d,\"%s\",\"%s\",%d,%d)", phoneCode, credentialId,
                    userId, doorList, phoneNumberList, huntType,
                    phoneCodeStatus);
            executeSqlQuery(gDbHandle, sqlString);

            break;

        case EDIT_CMD:              // Edit
            params = cJSON_GetObjectItem(cmd, SERIAL_NUMBER_JF);
            if (NULL != params) {
                serialNumber = params->valuestring;
                printf("Serial Number: [%s]\n", serialNumber);
            }

            params = cJSON_GetObjectItem(cmd, CREDENTIAL_ID_JF);
            if (NULL != params) {
                credentialId = params->valueint;
                printf("Credential ID: [%d]\n", credentialId);
            }

            params = cJSON_GetObjectItem(cmd, DOORS_JF);
            if (NULL != params) {
                for (idx = 0; idx < cJSON_GetArraySize(params); idx++) {
                    if (doorListLen >= MAX_DOOR_LIST_LEN) {
                        doorListLen = dataFixUp(MAX_DOOR_LIST_LEN, doorListLen,
                                doorList);
                        break;
                    }
                    doorId = cJSON_GetArrayItem(params, idx);
                    doorListLen += sprintf(doorList+doorListLen,
                            "%d,",doorId->valueint);
                }
                if (doorListLen >= MAX_DOOR_LIST_LEN) {
                    doorListLen = dataFixUp(MAX_DOOR_LIST_LEN, doorListLen,
                            doorList);
                }
                doorList[--doorListLen] = '\0';
                printf("Door List: [%s]\n", doorList);
            }

            params = cJSON_GetObjectItem(cmd, USER_ID_JF);
            if (NULL != params) {
                userId = params->valueint;
                printf("User ID: [%d]\n", userId);
            }

            params = cJSON_GetObjectItem(cmd, PHONE_CODE_JF);
            if (NULL != params) {
                phoneCode = params->valueint;
                printf("Phone Code: [%d]\n", phoneCode);
            }

            params = cJSON_GetObjectItem(cmd, HUNT_TYPE_JF);
            if (NULL != params) {
                if (params->valueint == CALL_HUNT_TYPE_SERIAL) {
                    huntType = CALL_HUNT_TYPE_SERIAL;
                } else if (params->valueint == CALL_HUNT_TYPE_PARALLEL) {
                    huntType = CALL_HUNT_TYPE_PARALLEL;
                } else {
                    huntType = CALL_HUNT_TYPE_INVALID;
                }
                printf("Hunt Type: [%d]\n", huntType);
            }

            params = cJSON_GetObjectItem(cmd, STATUS_JF);
            if (NULL != params) {
                phoneCodeStatus = params->valueint;
                printf("Code Status: [%d]\n", phoneCodeStatus);
            }

            params = cJSON_GetObjectItem(cmd, PHONE_NUMBERS_JF);
            if (NULL != params) {
                for (idx = 0; idx < cJSON_GetArraySize(params); idx++) {
                    if (phoneNumberListLen >= MAX_PHONE_NUMBER_LIST_LEN) {
                        dataFixUp(MAX_PHONE_NUMBER_LIST_LEN, phoneNumberListLen,
                                phoneNumberList);
                        break;
                    }
                    phoneNumber = cJSON_GetArrayItem(params, idx);
                    phoneNumberListLen += sprintf(phoneNumberList+phoneNumberListLen,
                            "%s,",phoneNumber->valuestring);
                }
                if (phoneNumberListLen >= MAX_PHONE_NUMBER_LIST_LEN) {
                    dataFixUp(MAX_PHONE_NUMBER_LIST_LEN, phoneNumberListLen,
                            phoneNumberList);
                }
                phoneNumberList[--phoneNumberListLen] = '\0';
                printf("Phone Number [%s]\n", phoneNumberList);
            }

            // SQL Update
            sqlStringLen = sprintf(sqlString+sqlStringLen, "UPDATE"
                    " phone_access SET");

            if (phoneCode) {
                valStringLen += sprintf(valString+valStringLen, " %s=%d,",
                        "phone_code", phoneCode);
            }

            if (doorList) {
                valStringLen += sprintf(valString+valStringLen, "%s=\"%s\",",
                        "door_id", doorList);
            }

            if (huntType != CALL_HUNT_TYPE_INVALID) {
                valStringLen += sprintf(valString+valStringLen, "%s=%d,",
                        "phone_hunt_type", huntType);
            }

            if (phoneNumberList) {
                valStringLen += sprintf(valString+valStringLen, "%s=\"%s\",",
                        "phone_number_list", phoneNumberList);
            }

            if (phoneCodeStatus != -1) {
                valStringLen += sprintf(valString+valStringLen, "%s=%d,",
                        "code_status", phoneCodeStatus);
            }

            valString[--valStringLen] = '\0';
            sqlStringLen += sprintf(sqlString+sqlStringLen, " %s WHERE"
                    " user_id=%d AND phone_credential_id=%d", valString, userId,
                    credentialId);
            executeSqlQuery(gDbHandle, sqlString);

            break;

        case DELETE_CMD:                // Delete
            params = cJSON_GetObjectItem(cmd, SERIAL_NUMBER_JF);
            if (NULL != params) {
                serialNumber = params->valuestring;
                printf("Serial Number: [%s]\n", serialNumber);
            }

            params = cJSON_GetObjectItem(cmd, CREDENTIAL_ID_JF);
            if (NULL != params) {
                credentialId = params->valueint;
                printf("Credential ID: [%d]\n", credentialId);
            }

            // SQL Delete
            sprintf(sqlString, "DELETE FROM phone_access WHERE"
                    " phone_credential_id=%d", credentialId);
            executeSqlQuery(gDbHandle, sqlString);

            break;

        default:
            // Shouldn't reach here
            printf("Illegal\n");

            break;
    }
}

/**
 * @function     : bleCodeCmd
 * @brief        : parses the BLE Code Command & performs the required
 *                operation on DB
 * @param1[in]   : cmd
 *                received BLE Code Access related data
 * @param2[in]   : cmdType
 *                whether it's an ADD, EDIT or DELETE operation
 * @return       : none
 */
void bleCodeCmd(cJSON *cmd, int cmdType)
{
    int idx;
    int userId;
    int credentialId;
    int bleCodeStatus;
    int doorListLen;
    int sqlStringLen;
    int valStringLen;
    char *bleUserName;
    char *blePassword;
    char *serialNumber;
    char sqlString[MAX_SQL_STRING_LEN];
    char valString[MAX_SQL_VALUE_LEN];
    char doorList[MAX_DOOR_LIST_LEN];
    cJSON *params, *doorId;

    //Initialise
    userId = 0;
    credentialId = 0;
    bleCodeStatus = -1;
    bleUserName = blePassword = serialNumber =  NULL;
    sqlStringLen = valStringLen = doorListLen = 0;

    memset(sqlString, '\0', MAX_SQL_STRING_LEN);
    memset(valString, '\0', MAX_SQL_VALUE_LEN);
    memset(doorList, '\0', MAX_DOOR_LIST_LEN);


    switch (cmdType) {
        case ADD_CMD:               // Add
            params = cJSON_GetObjectItem(cmd, SERIAL_NUMBER_JF);
            if (NULL != params) {
                serialNumber = params->valuestring;
                printf("Serial Number: [%s]\n", serialNumber);
            }

            params = cJSON_GetObjectItem(cmd, CREDENTIAL_ID_JF);
            if (NULL != params) {
                credentialId = params->valueint;
                printf("Credential ID: [%d]\n", credentialId);
            }

            params = cJSON_GetObjectItem(cmd, DOORS_JF);
            if (NULL != params) {
                for (idx = 0; idx < cJSON_GetArraySize(params); idx++) {
                    if (doorListLen >= MAX_DOOR_LIST_LEN) {
                        doorListLen = dataFixUp(MAX_DOOR_LIST_LEN, doorListLen,
                                doorList);
                        break;
                    }
                    doorId = cJSON_GetArrayItem(params, idx);
                    doorListLen += sprintf(doorList+doorListLen,
                            "%d,",doorId->valueint);
                }
                if (doorListLen >= MAX_DOOR_LIST_LEN) {
                    doorListLen = dataFixUp(MAX_DOOR_LIST_LEN, doorListLen,
                            doorList);
                }
                doorList[--doorListLen] = '\0';
                printf("Door List: [%s]\n", doorList);
            }

            params = cJSON_GetObjectItem(cmd, USER_ID_JF);
            if (NULL != params) {
                userId = params->valueint;
                printf("User ID: [%d]\n", userId);
            }

            params = cJSON_GetObjectItem(cmd, USERNAME_JF);
            if (NULL != params) {
                bleUserName = params->valuestring;
                printf("BLE Username: [%s]\n", bleUserName);
            }

            params = cJSON_GetObjectItem(cmd, PASSWORD_JF);
            if (NULL != params) {
                blePassword = params->valuestring;
                printf("BLE Password: [%s]\n", blePassword);
            }

            params = cJSON_GetObjectItem(cmd, STATUS_JF);
            if (NULL != params) {
                bleCodeStatus = params->valueint;
                printf("Code Status: [%d]\n", bleCodeStatus);
            }

            // SQL Insert
            sprintf(sqlString, "INSERT INTO ble_access values (\"%s\",\"%s\","
                    "%d,%d,\"%s\",%d)", bleUserName, blePassword, credentialId,
                    userId, doorList, bleCodeStatus);
            executeSqlQuery(gDbHandle, sqlString);

            break;

        case EDIT_CMD:              // Edit
            params = cJSON_GetObjectItem(cmd, SERIAL_NUMBER_JF);
            if (NULL != params) {
                serialNumber = params->valuestring;
                printf("Serial Number: [%s]\n", serialNumber);
            }

            params = cJSON_GetObjectItem(cmd, CREDENTIAL_ID_JF);
            if (NULL != params) {
                credentialId = params->valueint;
                printf("Credential ID: [%d]\n", credentialId);
            }

            params = cJSON_GetObjectItem(cmd, DOORS_JF);
            if (NULL != params) {
                for (idx = 0; idx < cJSON_GetArraySize(params); idx++) {
                    if (doorListLen >= MAX_DOOR_LIST_LEN) {
                        doorListLen = dataFixUp(MAX_DOOR_LIST_LEN, doorListLen,
                                doorList);
                        break;
                    }
                    doorId = cJSON_GetArrayItem(params, idx);
                    doorListLen += sprintf(doorList+doorListLen,
                            "%d,",doorId->valueint);
                }
                if (doorListLen >= MAX_DOOR_LIST_LEN) {
                    doorListLen = dataFixUp(MAX_DOOR_LIST_LEN, doorListLen,
                            doorList);
                }
                doorList[--doorListLen] = '\0';
                printf("Door List: [%s]\n", doorList);
            }

            params = cJSON_GetObjectItem(cmd, USER_ID_JF);
            if (NULL != params) {
                userId = params->valueint;
                printf("User ID: [%d]", userId);
            }

            params = cJSON_GetObjectItem(cmd, USERNAME_JF);
            if (NULL != params) {
                bleUserName = params->valuestring;
                printf("BLE Username: [%s]\n", bleUserName);
            }

            params = cJSON_GetObjectItem(cmd, PASSWORD_JF);
            if (NULL != params) {
                blePassword = params->valuestring;
                printf("BLE Password: [%s]\n", blePassword);
            }

            params = cJSON_GetObjectItem(cmd, STATUS_JF);
            if (NULL != params) {
                bleCodeStatus = params->valueint;
                printf("Code Status: [%d]\n", bleCodeStatus);
            }

            // SQL Update
            sqlStringLen = sprintf(sqlString+sqlStringLen, "UPDATE"
                    " ble_access SET ");
            if (bleUserName) {
                valStringLen += sprintf(valString+valStringLen,
                        "%s=\"%s\",", "ble_username", bleUserName);
            }

            if (blePassword) {
                valStringLen += sprintf(valString+valStringLen,
                        "%s=\"%s\",", "ble_password", blePassword);
            }

            if (doorList) {
                valStringLen += sprintf(valString+valStringLen,
                        "%s=\"%s\",", "door_id", doorList);
            }

            if (bleCodeStatus != -1) {
                valStringLen += sprintf(valString+valStringLen, "%s=%d,",
                        "code_status", bleCodeStatus);
            }

            valString[--valStringLen] = '\0';
            sqlStringLen += sprintf(sqlString+sqlStringLen, "%s WHERE"
                    " user_id=%d AND ble_credential_id=%d", valString, userId,
                     credentialId);
            executeSqlQuery(gDbHandle, sqlString);

            break;

        case DELETE_CMD:                // Delete
            params = cJSON_GetObjectItem(cmd, SERIAL_NUMBER_JF);
            if (NULL != params) {
                serialNumber = params->valuestring;
                printf("Serial Number: [%s]\n", serialNumber);
            }

            params = cJSON_GetObjectItem(cmd, CREDENTIAL_ID_JF);
            if (NULL != params) {
                credentialId = params->valueint;
                printf("Credential ID: [%d]\n", credentialId);
            }

            // SQL Delete
            sprintf(sqlString, "DELETE FROM ble_access WHERE"
                    " ble_credential_id=%d", credentialId);
            executeSqlQuery(gDbHandle, sqlString);

            break;

        default:
            // Shouldn't reach here
            printf("Illegal\n");

            break;
    }
}

/**
 * @function     : nfcCodeCmd
 * @brief        : parses the NFC Code Command & performs the required
 *                operation on DB
 * @param1[in]   : cmd
 *                received NFC Code Access related data
 * @param2[in]   : cmdType
 *                whether it's an ADD, EDIT or DELETE operation
 * @return       : none
 */
void nfcCodeCmd(cJSON *cmd, int cmdType)
{
    int idx;
    int userId;
    int credentialId;
    int nfcCodeStatus;
    int doorListLen;
    int sqlStringLen;
    int valStringLen;
    char *facilityCode;
    char *cardNumber;
    char *serialNumber;
    char doorList[MAX_DOOR_LIST_LEN];
    char sqlString[MAX_SQL_STRING_LEN];
    char valString[MAX_SQL_VALUE_LEN];
    cJSON *params, *doorId;

    //Initialise
    userId = 0;
    credentialId = 0;
    nfcCodeStatus = -1;
    facilityCode = cardNumber = serialNumber =  NULL;
    sqlStringLen = valStringLen = doorListLen = 0;

    memset(sqlString, '\0', MAX_SQL_STRING_LEN);
    memset(valString, '\0', MAX_SQL_VALUE_LEN);
    memset(doorList, '\0', MAX_DOOR_LIST_LEN);

    switch (cmdType) {
        case ADD_CMD:               // Add
            params = cJSON_GetObjectItem(cmd, SERIAL_NUMBER_JF);
            if (NULL != params) {
                serialNumber = params->valuestring;
                printf("Serial Number: [%s]\n", serialNumber);
            }

            params = cJSON_GetObjectItem(cmd, CREDENTIAL_ID_JF);
            if (NULL != params) {
                credentialId = params->valueint;
                printf("Credential ID: [%d]\n", credentialId);
            }

            params = cJSON_GetObjectItem(cmd, DOORS_JF);
            if (NULL != params) {
                for (idx = 0; idx < cJSON_GetArraySize(params); idx++) {
                    if (doorListLen >= MAX_DOOR_LIST_LEN) {
                        doorListLen = dataFixUp(MAX_DOOR_LIST_LEN, doorListLen,
                                doorList);
                        break;
                    }
                    doorId = cJSON_GetArrayItem(params, idx);
                    doorListLen += sprintf(doorList+doorListLen,
                            "%d,",doorId->valueint);
                }
                if (doorListLen >= MAX_DOOR_LIST_LEN) {
                    doorListLen = dataFixUp(MAX_DOOR_LIST_LEN, doorListLen,
                            doorList);
                }
                doorList[--doorListLen] = '\0';
                printf("Door List: [%s]\n", doorList);
            }

            params = cJSON_GetObjectItem(cmd, USER_ID_JF);
            if (NULL != params) {
                userId = params->valueint;
                printf("User ID: [%d]\n", userId);
            }

            params = cJSON_GetObjectItem(cmd, FACILITY_CODE_JF);
            if (NULL != params) {
                facilityCode = params->valuestring;
                printf("Facility Code: [%s]\n", facilityCode);
            }

            params = cJSON_GetObjectItem(cmd, NFC_CODE_JF);
            if (NULL != params) {
                cardNumber = params->valuestring;
                printf("Card Number: [%s]\n", cardNumber);
            }

            params = cJSON_GetObjectItem(cmd, STATUS_JF);
            if (NULL != params) {
                nfcCodeStatus  = params->valueint;
                printf("Code Status: [%d]\n", nfcCodeStatus);
            }

            // SQL Insert
            sprintf(sqlString, "INSERT INTO nfc_access values (\"%s\",\"%s\","
                    "%d,%d,\"%s\",%d)", facilityCode, cardNumber, credentialId,
                    userId, doorList, nfcCodeStatus);
            executeSqlQuery(gDbHandle, sqlString);

            break;

        case EDIT_CMD:              // Edit
            params = cJSON_GetObjectItem(cmd, SERIAL_NUMBER_JF);
            if (NULL != params) {
                serialNumber = params->valuestring;
                printf("Serial Number: [%s]\n", serialNumber);
            }

            params = cJSON_GetObjectItem(cmd, CREDENTIAL_ID_JF);
            if (NULL != params) {
                credentialId = params->valueint;
                printf("Credential ID: [%d]\n", credentialId);
            }

            params = cJSON_GetObjectItem(cmd, DOORS_JF);
            if (NULL != params) {
                for (idx = 0; idx < cJSON_GetArraySize(params); idx++) {
                    if (doorListLen >= MAX_DOOR_LIST_LEN) {
                        doorListLen = dataFixUp(MAX_DOOR_LIST_LEN, doorListLen,
                                doorList);
                        break;
                    }
                    doorId = cJSON_GetArrayItem(params, idx);
                    doorListLen += sprintf(doorList+doorListLen,
                            "%d,",doorId->valueint);
                }
                if (doorListLen >= MAX_DOOR_LIST_LEN) {
                    doorListLen = dataFixUp(MAX_DOOR_LIST_LEN, doorListLen,
                            doorList);
                }
                doorList[--doorListLen] = '\0';
                printf("Door List: [%s]\n", doorList);
            }

            params = cJSON_GetObjectItem(cmd, USER_ID_JF);
            if (NULL != params) {
                userId = params->valueint;
                printf("User ID: [%d]\n", userId);
            }

            params = cJSON_GetObjectItem(cmd, FACILITY_CODE_JF);
            if (NULL != params) {
                facilityCode = params->valuestring;
                printf("Facility Code: [%s]\n", facilityCode);
            }

            params = cJSON_GetObjectItem(cmd, NFC_CODE_JF);
            if (NULL != params) {
                cardNumber = params->valuestring;
                printf("Card Number: [%s]\n", cardNumber);
            }

            params = cJSON_GetObjectItem(cmd, STATUS_JF);
            if (NULL != params) {
                nfcCodeStatus  = params->valueint;
                printf("Code Status: [%d]\n", nfcCodeStatus);
            }

            // SQL Update
            sqlStringLen = sprintf(sqlString+sqlStringLen, "UPDATE"
                    " nfc_access SET ");

            if (facilityCode) {
                valStringLen += sprintf(valString+valStringLen,
                        "%s=\"%s\",", "nfc_facility_code", facilityCode);
            }

            if (cardNumber) {
                valStringLen += sprintf(valString+valStringLen,
                        "%s=\"%s\",", "nfc_card_number", cardNumber);
            }

            if (doorList) {
                valStringLen += sprintf(valString+valStringLen,
                        "%s=\"%s\",", "door_id", doorList);
            }

            if (nfcCodeStatus != -1) {
                valStringLen += sprintf(valString+valStringLen, "%s=%d,",
                        "code_status", nfcCodeStatus);
            }

            valString[--valStringLen] = '\0';
            sqlStringLen += sprintf(sqlString+sqlStringLen, "%s WHERE"
                    " user_id=%d AND nfc_credential_id=%d", valString, userId,
                    credentialId);
            executeSqlQuery(gDbHandle, sqlString);

            break;

        case DELETE_CMD:                // Delete
            params = cJSON_GetObjectItem(cmd, SERIAL_NUMBER_JF);
            if (NULL != params) {
                serialNumber = params->valuestring;
                printf("Serial Number: [%s]\n", serialNumber);
            }

            params = cJSON_GetObjectItem(cmd, CREDENTIAL_ID_JF);
            if (NULL != params) {
                credentialId = params->valueint;
                printf("Credential ID: [%d]\n", credentialId);
            }

            // SQL Delete
            sprintf(sqlString, "DELETE FROM nfc_access WHERE"
                    " nfc_credential_id=%d", credentialId);
            executeSqlQuery(gDbHandle, sqlString);

            break;

        default:
            // Shouldn't reach here
            printf("Illegal\n\n");

            break;
    }
}

/**
 * @function     : rfidCodeCmd
 * @brief        : parses the RFID Code Command & performs the required
 *                operation on DB
 * @param1[in]   : cmd
 *                received RFID Code Access related data
 * @param2[in]   : cmdType
 *                whether it's an ADD, EDIT or DELETE operation
 * @return       : none
 */
void rfidCodeCmd(cJSON *cmd, int cmdType)
{
    int idx;
    int userId;
    int rfidCodeStatus;
    int sqlStringLen;
    int valStringLen;
    int credentialId;
    int doorListLen;
    char *serialNumber;
    char *facilityCode;
    char *cardNumber;
    char sqlString[MAX_SQL_STRING_LEN];
    char valString[MAX_SQL_VALUE_LEN];
    char doorList[MAX_DOOR_LIST_LEN];
    cJSON *params, *doorId;

    //Initialise
    userId = 0;
    credentialId = 0;
    rfidCodeStatus = -1;
    facilityCode = cardNumber = serialNumber = NULL;
    sqlStringLen = valStringLen = doorListLen = 0;

    memset(sqlString, '\0', MAX_SQL_STRING_LEN);
    memset(valString, '\0', MAX_SQL_VALUE_LEN);
    memset(doorList, '\0', MAX_SQL_VALUE_LEN);

    switch (cmdType) {
        case ADD_CMD:               // Add
            params = cJSON_GetObjectItem(cmd, SERIAL_NUMBER_JF);
            if (NULL != params) {
                serialNumber = params->valuestring;
                printf("Serial Number: [%s]\n", serialNumber);
            }

            params = cJSON_GetObjectItem(cmd, CREDENTIAL_ID_JF);
            if (NULL != params) {
                credentialId = params->valueint;
                printf("Credential ID: [%d]\n", credentialId);
            }

            params = cJSON_GetObjectItem(cmd, DOORS_JF);
            if (NULL != params) {
                for (idx = 0; idx < cJSON_GetArraySize(params); idx++) {
                    if (doorListLen >= MAX_DOOR_LIST_LEN) {
                        doorListLen = dataFixUp(MAX_DOOR_LIST_LEN, doorListLen,
                                doorList);
                        break;
                    }
                    doorId = cJSON_GetArrayItem(params, idx);
                    doorListLen += sprintf(doorList+doorListLen,
                            "%d,",doorId->valueint);
                }
                if (doorListLen >= MAX_DOOR_LIST_LEN) {
                    doorListLen = dataFixUp(MAX_DOOR_LIST_LEN, doorListLen,
                            doorList);
                }
                doorList[--doorListLen] = '\0';
                printf("Door List: [%s]\n", doorList);
            }

            params = cJSON_GetObjectItem(cmd, USER_ID_JF);
            if (NULL != params) {
                userId = params->valueint;
                printf("User ID: [%d]\n", userId);
            }

            params = cJSON_GetObjectItem(cmd, FACILITY_CODE_JF);
            if (NULL != params) {
                facilityCode = params->valuestring;
                printf("Facility Code: [%s]\n", facilityCode);
            }

            params = cJSON_GetObjectItem(cmd, CARD_NUMBER_JF);
            if (NULL != params) {
                cardNumber = params->valuestring;
                printf("Card Number: [%s]\n", cardNumber);
            }

            params = cJSON_GetObjectItem(cmd, STATUS_JF);
            if (NULL != params) {
                rfidCodeStatus  = params->valueint;
                printf("Code Status: [%d]\n", rfidCodeStatus);
            }

            // SQL Insert
            sprintf(sqlString, "INSERT INTO rfid_access values (\"%s\",\"%s\","
                    "%d,%d,\"%s\",%d)", facilityCode, cardNumber, credentialId,
                    userId, doorList, rfidCodeStatus);
            executeSqlQuery(gDbHandle, sqlString);

            break;

        case EDIT_CMD:              // Edit
            params = cJSON_GetObjectItem(cmd, SERIAL_NUMBER_JF);
            if (NULL != params) {
                serialNumber = params->valuestring;
                printf("Serial Number: [%s]\n", serialNumber);
            }

            params = cJSON_GetObjectItem(cmd, CREDENTIAL_ID_JF);
            if (NULL != params) {
                credentialId = params->valueint;
                printf("Credential ID: [%d]\n", credentialId);
            }

            params = cJSON_GetObjectItem(cmd, DOORS_JF);
            if (NULL != params) {
                for (idx = 0; idx < cJSON_GetArraySize(params); idx++) {
                    if (doorListLen >= MAX_DOOR_LIST_LEN) {
                        doorListLen = dataFixUp(MAX_DOOR_LIST_LEN, doorListLen,
                                doorList);
                        break;
                    }
                    doorId = cJSON_GetArrayItem(params, idx);
                    doorListLen += sprintf(doorList+doorListLen,
                            "%d,",doorId->valueint);
                }
                if (doorListLen >= MAX_DOOR_LIST_LEN) {
                    doorListLen = dataFixUp(MAX_DOOR_LIST_LEN, doorListLen,
                            doorList);
                }
                doorList[--doorListLen] = '\0';
                printf("Door List: [%s]\n", doorList);
            }

            params = cJSON_GetObjectItem(cmd, USER_ID_JF);
            if (NULL != params) {
                userId = params->valueint;
                printf("User ID: [%d]\n", userId);
            }

            params = cJSON_GetObjectItem(cmd, FACILITY_CODE_JF);
            if (NULL != params) {
                facilityCode = params->valuestring;
                printf("Facility Code: [%s]\n", facilityCode);
            }

            params = cJSON_GetObjectItem(cmd, CARD_NUMBER_JF);
            if (NULL != params) {
                cardNumber = params->valuestring;
                printf("Card Number: [%s]\n", cardNumber);
            }

            params = cJSON_GetObjectItem(cmd, STATUS_JF);
            if (NULL != params) {
                rfidCodeStatus  = params->valueint;
                printf("Code Status: [%d]", rfidCodeStatus);
            }

            // SQL Update
            sqlStringLen = sprintf(sqlString+sqlStringLen, "UPDATE"
                    " rfid_access SET ");
            if (facilityCode) {
                valStringLen += sprintf(valString+valStringLen,
                        "%s=\"%s\",", "rfid_facility_code", facilityCode);
            }

            if (cardNumber) {
                valStringLen += sprintf(valString+valStringLen,
                        "%s=\"%s\",", "rfid_card_number", cardNumber);
            }

            if (doorList) {
                valStringLen += sprintf(valString+valStringLen,
                        "%s=\"%s\",", "door_id", doorList);
            }

            if (rfidCodeStatus != -1) {
                valStringLen += sprintf(valString+valStringLen, "%s=%d,",
                        "code_status", rfidCodeStatus);
            }

            valString[--valStringLen] = '\0';
            sqlStringLen += sprintf(sqlString+sqlStringLen, "%s WHERE"
                    " user_id=%d AND rfid_credential_id=%d", valString,
                    userId, credentialId);

            executeSqlQuery(gDbHandle, sqlString);

            break;

        case DELETE_CMD:                // Delete
            params = cJSON_GetObjectItem(cmd, SERIAL_NUMBER_JF);
            if (NULL != params) {
                serialNumber = params->valuestring;
                printf("Serial Number: [%s]\n", serialNumber);
            }

            params = cJSON_GetObjectItem(cmd, CREDENTIAL_ID_JF);
            if (NULL != params) {
                credentialId = params->valueint;
                printf("Credential ID: [%d]\n", credentialId);
            }

            // SQL Delete
            sprintf(sqlString, "DELETE FROM rfid_access WHERE"
                    " rfid_credential_id=%d", credentialId);
            executeSqlQuery(gDbHandle, sqlString);

            break;

        default:
            // Shouldn't reach here
            printf("Illegal\n\n");

            break;
    }
}

/**
 * @function     : wiegandCodeCmd
 * @brief        : parses the wiegandCodeCmd Code Command & performs the required
 *                operation on DB
 * @param1[in]   : cmd
 *                received wiegand Code Access related data
 * @param2[in]   : cmdType
 *                whether it's an ADD, EDIT or DELETE operation
 * @return       : none
 */
void wiegandCodeCmd(cJSON *cmd, int cmdType)
{
    int idx;
    int userId;
    int wiegandCodeStatus;
    int sqlStringLen;
    int valStringLen;
    int credentialId;
    int doorListLen;
    char *serialNumber;
    char *facilityCode;
    char *cardNumber;
    char sqlString[MAX_SQL_STRING_LEN];
    char valString[MAX_SQL_VALUE_LEN];
    char doorList[MAX_DOOR_LIST_LEN];
    cJSON *params, *doorId;

    //Initialise
    userId = 0;
    credentialId = 0;
    wiegandCodeStatus = -1;
    facilityCode = cardNumber = serialNumber = NULL;
    sqlStringLen = valStringLen = doorListLen = 0;

    memset(sqlString, '\0', MAX_SQL_STRING_LEN);
    memset(valString, '\0', MAX_SQL_VALUE_LEN);
    memset(doorList, '\0', MAX_SQL_VALUE_LEN);

    switch (cmdType) {
        case ADD_CMD:               // Add
            params = cJSON_GetObjectItem(cmd, SERIAL_NUMBER_JF);
            if (NULL != params) {
                serialNumber = params->valuestring;
                printf("Serial Number: [%s]\n", serialNumber);
            }

            params = cJSON_GetObjectItem(cmd, CREDENTIAL_ID_JF);
            if (NULL != params) {
                credentialId = params->valueint;
                printf("Credential ID: [%d]\n", credentialId);
            }

            params = cJSON_GetObjectItem(cmd, DOORS_JF);
            if (NULL != params) {
                for (idx = 0; idx < cJSON_GetArraySize(params); idx++) {
                    if (doorListLen >= MAX_DOOR_LIST_LEN) {
                        doorListLen = dataFixUp(MAX_DOOR_LIST_LEN, doorListLen,
                                doorList);
                        break;
                    }
                    doorId = cJSON_GetArrayItem(params, idx);
                    doorListLen += sprintf(doorList+doorListLen,
                            "%d,",doorId->valueint);
                }
                if (doorListLen >= MAX_DOOR_LIST_LEN) {
                    doorListLen = dataFixUp(MAX_DOOR_LIST_LEN, doorListLen,
                            doorList);
                }
                doorList[--doorListLen] = '\0';
                printf("Door List: [%s]\n", doorList);
            }

            params = cJSON_GetObjectItem(cmd, USER_ID_JF);
            if (NULL != params) {
                userId = params->valueint;
                printf("User ID: [%d]\n", userId);
            }

            params = cJSON_GetObjectItem(cmd, FACILITY_CODE_JF);
            if (NULL != params) {
                facilityCode = params->valuestring;
                printf("Facility Code: [%s]\n", facilityCode);
            }

            params = cJSON_GetObjectItem(cmd, CARD_NUMBER_JF);
            if (NULL != params) {
                cardNumber = params->valuestring;
                printf("Card Number: [%s]\n", cardNumber);
            }

            params = cJSON_GetObjectItem(cmd, STATUS_JF);
            if (NULL != params) {
                wiegandCodeStatus  = params->valueint;
                printf("Code Status: [%d]\n", wiegandCodeStatus);
            }

            // SQL Insert
            sprintf(sqlString, "INSERT INTO wiegand_access values (\"%s\",\"%s\","
                    "%d,%d,\"%s\",%d)", facilityCode, cardNumber, credentialId,
                    userId, doorList, wiegandCodeStatus);
            executeSqlQuery(gDbHandle, sqlString);

            break;

        case EDIT_CMD:              // Edit
            params = cJSON_GetObjectItem(cmd, SERIAL_NUMBER_JF);
            if (NULL != params) {
                serialNumber = params->valuestring;
                printf("Serial Number: [%s]\n", serialNumber);
            }

            params = cJSON_GetObjectItem(cmd, CREDENTIAL_ID_JF);
            if (NULL != params) {
                credentialId = params->valueint;
                printf("Credential ID: [%d]\n", credentialId);
            }

            params = cJSON_GetObjectItem(cmd, DOORS_JF);
            if (NULL != params) {
                for (idx = 0; idx < cJSON_GetArraySize(params); idx++) {
                    if (doorListLen >= MAX_DOOR_LIST_LEN) {
                        doorListLen = dataFixUp(MAX_DOOR_LIST_LEN, doorListLen,
                                doorList);
                        break;
                    }
                    doorId = cJSON_GetArrayItem(params, idx);
                    doorListLen += sprintf(doorList+doorListLen,
                            "%d,",doorId->valueint);
                }
                if (doorListLen >= MAX_DOOR_LIST_LEN) {
                    doorListLen = dataFixUp(MAX_DOOR_LIST_LEN, doorListLen,
                            doorList);
                }
                doorList[--doorListLen] = '\0';
                printf("Door List: [%s]\n", doorList);
            }

            params = cJSON_GetObjectItem(cmd, USER_ID_JF);
            if (NULL != params) {
                userId = params->valueint;
                printf("User ID: [%d]\n", userId);
            }

            params = cJSON_GetObjectItem(cmd, FACILITY_CODE_JF);
            if (NULL != params) {
                facilityCode = params->valuestring;
                printf("Facility Code: [%s]\n", facilityCode);
            }

            params = cJSON_GetObjectItem(cmd, CARD_NUMBER_JF);
            if (NULL != params) {
                cardNumber = params->valuestring;
                printf("Card Number: [%s]\n", cardNumber);
            }

            params = cJSON_GetObjectItem(cmd, STATUS_JF);
            if (NULL != params) {
                wiegandCodeStatus  = params->valueint;
                printf("Code Status: [%d]\n", wiegandCodeStatus);
            }

            // SQL Update
            sqlStringLen = sprintf(sqlString+sqlStringLen, "UPDATE"
                    " wiegand_access SET ");
            if (facilityCode) {
                valStringLen += sprintf(valString+valStringLen,
                        "%s=\"%s\",", "wiegand_facility_code", facilityCode);
            }

            if (cardNumber) {
                valStringLen += sprintf(valString+valStringLen,
                        "%s=\"%s\",", "wiegand_card_number", cardNumber);
            }

            if (doorList) {
                valStringLen += sprintf(valString+valStringLen,
                        "%s=\"%s\",", "door_id", doorList);
            }

            if (wiegandCodeStatus != -1) {
                valStringLen += sprintf(valString+valStringLen, "%s=%d,",
                        "code_status", wiegandCodeStatus);
            }

            valString[--valStringLen] = '\0';
            sqlStringLen += sprintf(sqlString+sqlStringLen, "%s WHERE"
                    " user_id=%d AND wiegand_credential_id=%d", valString,
                    userId, credentialId);

            executeSqlQuery(gDbHandle, sqlString);

            break;

        case DELETE_CMD:                // Delete
            params = cJSON_GetObjectItem(cmd, SERIAL_NUMBER_JF);
            if (NULL != params) {
                serialNumber = params->valuestring;
                printf("Serial Number: [%s]\n", serialNumber);
            }

            params = cJSON_GetObjectItem(cmd, CREDENTIAL_ID_JF);
            if (NULL != params) {
                credentialId = params->valueint;
                printf("Credential ID: [%d]\n", credentialId);
            }

            // SQL Delete
            sprintf(sqlString, "DELETE FROM wiegand_access WHERE"
                    " wiegand_credential_id=%d", credentialId);
            executeSqlQuery(gDbHandle, sqlString);

            break;

        default:
            // Shouldn't reach here
            printf("Illegal\n\n");

            break;
    }
}

/**
 * @function     : userCmd
 * @brief        : parses the User related Command & performs the required
 *                operation on DB
 * @param1[in]   : cmd
 *                received User related data
 * @param2[in]   : cmdType
 *                whether it's an ADD, EDIT or DELETE operation
 * @return       : none
 */
void userCmd(cJSON *cmd, int cmdType)
{
    int userId;
    int status;
    int sqlStringLen;
    int valStringLen;
    long userAccessExpireTs;
    char *serialNumber;
    char *firstName;
    char *lastName;
    char *onScreenName;
    char sqlString[MAX_SQL_STRING_LEN];
    char valString[MAX_SQL_VALUE_LEN];
    cJSON *params;

    //Initialise
    userId = 0;
    status = -1;
    userAccessExpireTs = 0;
    sqlStringLen = valStringLen = 0;

    firstName = lastName = onScreenName = NULL;

    memset(sqlString, '\0', MAX_SQL_STRING_LEN);
    memset(valString, '\0', MAX_SQL_VALUE_LEN);

    switch (cmdType) {
        case ADD_CMD:               // Add
            params = cJSON_GetObjectItem(cmd, SERIAL_NUMBER_JF);
            if (NULL != params) {
                serialNumber = params->valuestring;
                printf("Serial Number: [%s]\n", serialNumber);
            }

            params = cJSON_GetObjectItem(cmd, USER_ID_JF);
            if (NULL != params) {
                userId = params->valueint;
                printf("User ID: [%d]\n", userId);
            }

            params = cJSON_GetObjectItem(cmd, STATUS_JF);
            if (NULL != params) {
                status = params->valueint;
                printf("User Status: [%d]\n", status);
            }

            params = cJSON_GetObjectItem(cmd, FIRST_NAME_JF);
            if (NULL != params) {
                firstName = params->valuestring;
                printf("First Name: [%s]\n", firstName);
            }

            params = cJSON_GetObjectItem(cmd, LAST_NAME_JF);
            if (NULL != params) {
                lastName = params->valuestring;
                printf("Last Name: [%s]\n", lastName);
            }

            params = cJSON_GetObjectItem(cmd, ON_SCREEN_NAME_JF);
            if (NULL != params) {
                onScreenName = params->valuestring;
                printf("On Screen Name: [%s]\n", onScreenName);
            }

            params = cJSON_GetObjectItem(cmd, EXPIRATION_DATE_JF);
            if (NULL != params) {
                userAccessExpireTs = params->valueint;
                printf("User Access Expiration Time [%ld]\n", userAccessExpireTs);
            }

            // SQL Insert
            sprintf(sqlString, "INSERT INTO user_status values"
                    "(%d,%d,\"%s\",\"%s\", \"%s\",%ld)", userId, status,
                    firstName, lastName, onScreenName,
                userAccessExpireTs);
            executeSqlQuery(gDbHandle, sqlString);

            break;

        case EDIT_CMD:              // Edit
            params = cJSON_GetObjectItem(cmd, SERIAL_NUMBER_JF);
            if (NULL != params) {
                serialNumber = params->valuestring;
                printf("Serial Number: [%s]\n", serialNumber);
            }

            params = cJSON_GetObjectItem(cmd, USER_ID_JF);
            if (NULL != params) {
                userId = params->valueint;
                printf("User ID: [%d]\n", userId);
            }

            params = cJSON_GetObjectItem(cmd, STATUS_JF);
            if (NULL != params) {
                status = params->valueint;
                printf("User Status: [%d]\n", status);
            }

            params = cJSON_GetObjectItem(cmd, FIRST_NAME_JF);
            if (NULL != params) {
                firstName = params->valuestring;
                printf("First Name: [%s]\n", firstName);
            }

            params = cJSON_GetObjectItem(cmd, LAST_NAME_JF);
            if (NULL != params) {
                lastName = params->valuestring;
                printf("Last Name: [%s]\n", lastName);
            }

            params = cJSON_GetObjectItem(cmd, ON_SCREEN_NAME_JF);
            if (NULL != params) {
                onScreenName = params->valuestring;
                printf("On Screen Name: [%s]\n", onScreenName);
            }

            params = cJSON_GetObjectItem(cmd, EXPIRATION_DATE_JF);
            if (NULL != params) {
                userAccessExpireTs = params->valueint;
                printf("User Access Expiration Time [%ld]\n", userAccessExpireTs);
            }

            // SQL Update
            sqlStringLen = sprintf(sqlString+sqlStringLen, "UPDATE"
                    " user_status SET");
            if (status != -1) {
                valStringLen += sprintf(valString+valStringLen, " %s=%d,",
                        "user_status", status);
            }

            if (firstName) {
                valStringLen += sprintf(valString+valStringLen, "%s=\"%s\",",
                        "user_first_name", firstName);
            }

            if (lastName) {
                valStringLen += sprintf(valString+valStringLen, "%s=\"%s\",",
                        "user_last_name", lastName);
            }

            if (onScreenName) {
                valStringLen += sprintf(valString+valStringLen, "%s=\"%s\",",
                        "user_on_screen_name", onScreenName);
            }

            if (userAccessExpireTs) {
                valStringLen += sprintf(valString+valStringLen, "%s=%ld,",
                        "user_access_expire", userAccessExpireTs);
            }
            valString[--valStringLen] = '\0';
            sqlStringLen += sprintf(sqlString+sqlStringLen, "%s WHERE"
                    " user_id=%d", valString, userId);
            executeSqlQuery(gDbHandle, sqlString);

            break;

        case DELETE_CMD:                // Delete
            params = cJSON_GetObjectItem(cmd, SERIAL_NUMBER_JF);
            if (NULL != params) {
                serialNumber = params->valuestring;
                printf("Serial Number: [%s]\n", serialNumber);
            }

            params = cJSON_GetObjectItem(cmd, USER_ID_JF);
            if (NULL != params) {
                userId = params->valueint;
                printf("User ID: [%d]\n", userId);
            }

            // SQL Delete (Delete User & all associated attributes)
            sprintf(sqlString, "DELETE FROM keypad_access WHERE user_id=%d",
                    userId);
            executeSqlQuery(gDbHandle, sqlString);

            memset(sqlString, '\0', MAX_SQL_STRING_LEN);
            sprintf(sqlString, "DELETE FROM phone_access WHERE user_id=%d",
                    userId);
            executeSqlQuery(gDbHandle, sqlString);

            memset(sqlString, '\0', MAX_SQL_STRING_LEN);
            sprintf(sqlString, "DELETE FROM ble_access WHERE user_id=%d",
                    userId);
            executeSqlQuery(gDbHandle, sqlString);

            memset(sqlString, '\0', MAX_SQL_STRING_LEN);
            sprintf(sqlString, "DELETE FROM nfc_access WHERE user_id=%d",
                    userId);
            executeSqlQuery(gDbHandle, sqlString);

            memset(sqlString, '\0', MAX_SQL_STRING_LEN);
            sprintf(sqlString, "DELETE FROM rfid_access WHERE user_id=%d",
                    userId);
            executeSqlQuery(gDbHandle, sqlString);

            memset(sqlString, '\0', MAX_SQL_STRING_LEN);
            sprintf(sqlString, "DELETE FROM wiegand_access WHERE user_id=%d",
                    userId);
            executeSqlQuery(gDbHandle, sqlString);

            memset(sqlString, '\0', MAX_SQL_STRING_LEN);
            sprintf(sqlString, "DELETE FROM access_schedule WHERE user_id=%d",
                    userId);
            executeSqlQuery(gDbHandle, sqlString);

            memset(sqlString, '\0', MAX_SQL_STRING_LEN);
            sprintf(sqlString, "DELETE FROM user_status WHERE user_id=%d",
                    userId);
            executeSqlQuery(gDbHandle, sqlString);

            break;

        default:
            // Shouldn't reach here
            printf("Illegal\n");

            break;
    }
}

/**
 * @function     : userDoorScheduleCmd
 * @brief        : parses the Door Schedule Command & performs the required
 *                operation on DB
 * @param1[in]   : cmd
 *                received Door-Schedule data
 * @return       : none
 */
void userDoorScheduleCmd(cJSON *cmd,int cmdType)
{
    int idxLevel1;
    int userId;
    int doorId;
    int scheduleId;
    int timeSlotLength;
    int sqlStringLen;
    int valStringLen;
    long scheduleStartDate;
    long scheduleEndDate;
    SCHEDULE_ACCESS_TYPE scheduleType;
    char sqlString[MAX_SCHEDULE_STRING_LEN];
    char valString[MAX_SCHEDULE_STRING_LEN];
    char firstDayOfWeek[MAX_SCHEDULE_LEN];
    char secondDayOfWeek[MAX_SCHEDULE_LEN];
    char thirdDayOfWeek[MAX_SCHEDULE_LEN];
    char fourthDayOfWeek[MAX_SCHEDULE_LEN];
    char fifthDayOfWeek[MAX_SCHEDULE_LEN];
    char sixthDayOfWeek[MAX_SCHEDULE_LEN];
    char seventhDayOfWeek[MAX_SCHEDULE_LEN];
    cJSON *params, *getcJSONArray, *getcJSONObject;
    cJSON *schedule, *scheduleArray, *scheduleObject;

    // Initialise
    userId = 0;
    doorId = 0;
    scheduleId = 0;
    timeSlotLength = 0;
    sqlStringLen = 0;
    valStringLen = 0;
    scheduleStartDate = 0;
    scheduleEndDate = 0;
    scheduleType = INVALID_ACCESS_SCHEDULE;

    memset(sqlString, '\0', MAX_SCHEDULE_STRING_LEN);
    memset(valString, '\0', MAX_SCHEDULE_STRING_LEN);
    memset(firstDayOfWeek, '\0', MAX_SCHEDULE_LEN);
    memset(secondDayOfWeek, '\0', MAX_SCHEDULE_LEN);
    memset(thirdDayOfWeek, '\0', MAX_SCHEDULE_LEN);
    memset(fourthDayOfWeek, '\0', MAX_SCHEDULE_LEN);
    memset(fifthDayOfWeek, '\0', MAX_SCHEDULE_LEN);
    memset(sixthDayOfWeek, '\0', MAX_SCHEDULE_LEN);
    memset(seventhDayOfWeek, '\0', MAX_SCHEDULE_LEN);

    params = cJSON_GetObjectItem(cmd, USER_ID_JF);
    if (NULL != params) {
        userId = params->valueint;
        printf("User ID: [%d]\n", userId);
    }

    params = cJSON_GetObjectItem(cmd, DOOR_SCHEDULE_ID_JF);
    if (NULL != params) {
        scheduleId = params->valueint;
        printf("Schedule ID: [%d]\n", scheduleId);
    }

    params = cJSON_GetObjectItem(cmd , DOOR_SCHEDULE_JF);
    if (NULL != params) {
        for (idxLevel1=0; idxLevel1<cJSON_GetArraySize(params); idxLevel1++) {
            getcJSONArray = cJSON_GetArrayItem(params, idxLevel1);
            if (!strcmp(getcJSONArray->string, DOOR_ID_JF)) {
                doorId = getcJSONArray->valueint;
                printf("Door ID: [%d]\n", doorId);
            } else if (!strcmp(getcJSONArray->string, SCHEDULE_ACCESS_TYPE_JF)) {
                if (getcJSONArray->valueint == RESTRICTED_ACCESS_SCHEDULE) {
                    scheduleType = RESTRICTED_ACCESS_SCHEDULE;
                } else if (getcJSONArray->valueint == FULL_ACCESS_SCHEDULE) {
                    scheduleType = FULL_ACCESS_SCHEDULE;
                } else if (getcJSONArray->valueint == NO_ACCESS_SCHEDULE) {
                    scheduleType = NO_ACCESS_SCHEDULE;
                } else {
                    scheduleType = INVALID_ACCESS_SCHEDULE;
                }
                printf("Schedule Access Type: [%d]\n", scheduleType);
            } else if (!strcmp(getcJSONArray->string, SCHEDULE_START_DATE_JF)) {
                scheduleStartDate = getcJSONArray->valueint;
                printf("Schedule Start Date [%ld]\n", scheduleStartDate);
            } else if (!strcmp(getcJSONArray->string, SCHEDULE_END_DATE_JF)) {
                scheduleEndDate = getcJSONArray->valueint;
                printf("Schedule End Date [%ld]\n", scheduleEndDate);
            } else if (scheduleType == RESTRICTED_ACCESS_SCHEDULE) {
                if (!strcmp(getcJSONArray->string, SUNDAY_SCHEDULE_JF)) {
                    timeSlots(getcJSONArray, timeSlotLength, firstDayOfWeek);
                    printf("First Day of Week [%s]\n", firstDayOfWeek);
                } else if (!strcmp(getcJSONArray->string, MONDAY_SCHEDULE_JF)) {
                    timeSlots(getcJSONArray, timeSlotLength, secondDayOfWeek);
                    printf("Second Day of Week [%s]\n", secondDayOfWeek);
                } else if (!strcmp(getcJSONArray->string, TUESDAY_SCHEDULE_JF)) {
                    timeSlots(getcJSONArray, timeSlotLength, thirdDayOfWeek);
                    printf("Third Day of Week [%s]\n", thirdDayOfWeek);
                } else if (!strcmp(getcJSONArray->string, WEDNESDAY_SCHEDULE_JF)) {
                    timeSlots(getcJSONArray, timeSlotLength, fourthDayOfWeek);
                    printf("Fourth Day of Week [%s]\n", fourthDayOfWeek);
                } else if (!strcmp(getcJSONArray->string, THURSEDAY_SCHEDULE_JF)) {
                    timeSlots(getcJSONArray, timeSlotLength, fifthDayOfWeek);
                    printf("Fifth Day of Week [%s]\n", fifthDayOfWeek);
                } else if (!strcmp(getcJSONArray->string, FRIDAY_SCHEDULE_JF)) {
                    timeSlots(getcJSONArray, timeSlotLength, sixthDayOfWeek);
                    printf("Sixth Day of Week [%s]\n", sixthDayOfWeek);
                } else if (!strcmp(getcJSONArray->string, SATURDAY_SCHEDULE_JF)) {
                    timeSlots(getcJSONArray, timeSlotLength, seventhDayOfWeek);
                    printf("Seventh Day of Week [%s]\n", seventhDayOfWeek);
                }
            }
        }
    }

    switch (cmdType) {
        case ADD_CMD:
            // SQL Insert
            sprintf(sqlString, "INSERT INTO access_schedule values (%d,%d,%d,"
                "%d,\"%s\",\"%s\",\"%s\",\"%s\",\"%s\",\"%s\",\"%s\",%ld,%ld)",
                    scheduleId, userId, doorId, scheduleType, firstDayOfWeek,
                    secondDayOfWeek, thirdDayOfWeek, fourthDayOfWeek,
                    fifthDayOfWeek, sixthDayOfWeek, seventhDayOfWeek,
                    scheduleStartDate, scheduleEndDate);
            executeSqlQuery(gDbHandle, sqlString);
            break;

        case EDIT_CMD:
            // SQL Update
            sqlStringLen += sprintf(sqlString+sqlStringLen, "UPDATE"
                    " access_schedule SET ");
            if (doorId) {
                valStringLen += sprintf(valString+valStringLen, "%s=%d,",
                        "door_id", doorId);
            }
            if (scheduleType != INVALID_ACCESS_SCHEDULE) {
                valStringLen += sprintf(valString+valStringLen, "%s=%d,",
                        "schedule_type", scheduleType);

                valStringLen += sprintf(valString+valStringLen, "%s=\"%s\",",
                        "first_dow", firstDayOfWeek);
                valStringLen += sprintf(valString+valStringLen, "%s=\"%s\",",
                        "second_dow", secondDayOfWeek);
                valStringLen += sprintf(valString+valStringLen, "%s=\"%s\",",
                        "third_dow", thirdDayOfWeek);
                valStringLen += sprintf(valString+valStringLen, "%s=\"%s\",",
                        "fourth_dow", fourthDayOfWeek);
                valStringLen += sprintf(valString+valStringLen, "%s=\"%s\",",
                        "fifth_dow", fifthDayOfWeek);
                valStringLen += sprintf(valString+valStringLen, "%s=\"%s\",",
                        "sixth_dow", sixthDayOfWeek);
                valStringLen += sprintf(valString+valStringLen, "%s=\"%s\",",
                        "seventh_dow", seventhDayOfWeek);
            }
            if (scheduleStartDate) {
                valStringLen += sprintf(valString+valStringLen, "%s=%ld,",
                        "schedule_start_date", scheduleStartDate);
            }
            if (scheduleEndDate) {
                valStringLen += sprintf(valString+valStringLen, "%s=%ld,",
                        "schedule_end_date", scheduleEndDate);
            }

            valString[--valStringLen] = '\0';
            sqlStringLen += sprintf(sqlString+sqlStringLen, "%s WHERE"
                    " schedule_id=%d", valString, scheduleId);
            executeSqlQuery(gDbHandle, sqlString);

            break;

        case DELETE_CMD:
            if (userId) {
                sprintf(sqlString, "DELETE FROM access_schedule where"
                        " user_id=%d AND schedule_id=%d", userId, scheduleId);
            } else {
                sprintf(sqlString, "DELETE FROM access_schedule where"
                        " schedule_id=%d", scheduleId);
            }
            executeSqlQuery(gDbHandle, sqlString);

            break;
    }
}

/**
 * @function     : timeSlots
 * @brief        : parses the Door Schedule Command & performs the required
 *                operation on DB
 * @param1[in]   : schedule
 *                schedule information
 * @param2[in]   : timeSlotLength
 *                length of timeslot
 * @param3[out]  : dayOfWeek
 *                created timeslot
 * @return       : none
 */
void timeSlots(cJSON *schedule, int timeSlotLength, char *dayOfWeek)
{
    int idx;
    cJSON *scheduleArray, *scheduleObject;

    //Initialise
    timeSlotLength = 0;

    for (idx=0; idx<cJSON_GetArraySize(schedule); idx++) {
        scheduleArray = cJSON_GetArrayItem(schedule, idx);
        scheduleObject = cJSON_GetObjectItem(scheduleArray, START_TIME_JF);
        if ((NULL != scheduleObject) &&
                (strcmp(scheduleObject->valuestring,""))) {
            timeSlotLength += sprintf(dayOfWeek+timeSlotLength,"%s-",
                    scheduleObject->valuestring);
        }
        scheduleObject = cJSON_GetObjectItem(scheduleArray, END_TIME_JF);
        if ((NULL != scheduleObject) &&
                (strcmp(scheduleObject->valuestring,""))) {
            timeSlotLength += sprintf(dayOfWeek+timeSlotLength,"%s ",
                    scheduleObject->valuestring);
        }
    }
    dayOfWeek[--timeSlotLength] = '\0';
}

/**
 * @function     : cameraCmd
 * @brief        : parses the Camera Command & stores the camera related
 *                information in DB
 * @param1[in]   : cmd
 *                received Camera related data
 * @param2[in]   : cmdType
 *                whether it's an ADD, EDIT or DELETE operation
 * @return       : none
 */
void cameraCmd(cJSON *cmd, int cmdType)
{
    int cameraPort;
    int sqlStringLen;
    int valStringLen;
    char *cameraId;
    char *cameraIp;
    char sqlString[MAX_SQL_STRING_LEN];
    char valString[MAX_SQL_VALUE_LEN];
    cJSON *params;

    // Initialise
    cameraPort = 0;
    sqlStringLen = valStringLen = 0;
    cameraId = cameraIp = NULL;

    memset(sqlString, '\0', MAX_SQL_STRING_LEN);
    memset(valString, '\0', MAX_SQL_STRING_LEN);

    switch (cmdType) {
        case ADD_CMD:               // Add
            params = cJSON_GetObjectItem(cmd, CAMERA_ID_JF);
            if (NULL != params) {
                cameraId = params->valuestring;
                printf("Camera ID: [%s]\n", cameraId);
            }

            params = cJSON_GetObjectItem(cmd, STATIC_IP_JF);
            if (NULL != params) {
                cameraIp = params->valuestring;
                printf("Camera IP: [%s]\n", cameraIp);
            }

            params = cJSON_GetObjectItem(cmd, PORT_JF);
            if (NULL != params) {
                cameraPort = params->valueint;
                printf("Camera Port: [%d]\n", cameraPort);
            }

            // SQL Insert
            sprintf(sqlString, "INSERT INTO camera_ip_mapping values (\"%s\",\""
                "%s\", %d)",
                    cameraId, cameraIp, cameraPort);
            executeSqlQuery(gDbHandle, sqlString);

            break;

        case EDIT_CMD:              // Edit
            params = cJSON_GetObjectItem(cmd, CAMERA_ID_JF);
            if (NULL != params) {
                cameraId = params->valuestring;
                printf("Camera ID: [%s]\n", cameraId);
            }

            params = cJSON_GetObjectItem(cmd, STATIC_IP_JF);
            if (NULL != params) {
                cameraIp = params->valuestring;
                printf("Camera IP: [%s]\n", cameraIp);
            }

            params = cJSON_GetObjectItem(cmd, PORT_JF);
            if (NULL != params) {
                cameraPort = params->valueint;
                printf("Camera Port: [%d]\n", cameraPort);
            }
            // SQL Update
            sqlStringLen = sprintf(sqlString+sqlStringLen, "UPDATE"
                    " camera_ip_mapping SET");
            if (cameraIp) {
                valStringLen += sprintf(valString+valStringLen, " %s=\"%s\",",
                        "camera_ip", cameraIp);
            }

            if (cameraPort) {
                valStringLen += sprintf(valString+valStringLen, " %s=%d,",
                        "camera_port", cameraPort);
            }

            valString[--valStringLen] = '\0';
            sqlStringLen += sprintf(sqlString+sqlStringLen, "%s WHERE"
                    " camera_id=\"%s\"", valString, cameraId);
            executeSqlQuery(gDbHandle, sqlString);

            break;

        case DELETE_CMD:                // Delete
            params = cJSON_GetObjectItem(cmd, CAMERA_ID_JF);
            if (NULL != params) {
                cameraId = params->valuestring;
                printf("Camera ID: [%s]\n", cameraId);
            }

            // SQL Delete
            sprintf(sqlString, "DELETE FROM camera_ip_mapping WHERE camera_id="
                    "\"%s\"", cameraId);
            executeSqlQuery(gDbHandle, sqlString);

            break;

        default:
            // Shouldn't reach here
            printf("Illegal\n");

            break;
    }
}

/**
 * @function     : relayMappingCmd
 * @brief        : parses the Relay Mapping command & invokes appropriate
 *               function to handle the specified relay
 * @param1[in]   : cmd
 *                received Relay related data
 * @return       : none
 */
void relayMappingCmd(cJSON *cmd)
{
    int relayMappingIdx;
    char *serialNumber;

    cJSON *relayMappingArray, *params;

    //Initialise
    relayMappingIdx = 0;
    serialNumber = NULL;

    params = cJSON_GetObjectItem(cmd, SERIAL_NUMBER_JF);
    serialNumber = params->valuestring;
    printf("Serial Number [%s]\n", serialNumber);

    params = cJSON_GetObjectItem(cmd, RELAYS_JF);

    for (relayMappingIdx=0; relayMappingIdx<cJSON_GetArraySize(params);
            relayMappingIdx++) {
        relayMappingArray = cJSON_GetArrayItem(params, relayMappingIdx);
        printf("Relay Command %s\n", relayMappingArray->string);

        // TODO: Increase Relay Count to handle maximum relays
        if (!strcmp(relayMappingArray->string, FIRST_RELAY_JF)) {
            relayMapping(relayMappingArray, FIRST_RELAY);
        } else if (!strcmp(relayMappingArray->string, SECOND_RELAY_JF)) {
            relayMapping(relayMappingArray, SECOND_RELAY);
        } else if (!strcmp(relayMappingArray->string, THIRD_RELAY_JF)) {
            relayMapping(relayMappingArray, THIRD_RELAY);
        } else if (!strcmp(relayMappingArray->string, FOURTH_RELAY_JF)) {
            relayMapping(relayMappingArray, FOURTH_RELAY);
        }
    }
}

/**
 * @function     : relayMapping
 * @brief        : parses the Relay Mapping Command & stores the relay mapping
 *                information in the DB
 * @param1[in]   : cmd
 *                received Relay related data
 * @param2[in]   : relayId
 *                relay identifier
 * @return       : none
 */
void relayMapping(cJSON *cmd, int relayId)
{
    int doorId;
    int sqlStringLen;
    int valStringLen;
    int relayStrikeTime;
    RELAY_TYPE relayInterfaceType;
    char sqlString[MAX_SQL_STRING_LEN];
    char valString[MAX_SQL_VALUE_LEN];
    char sqlQuery[MAX_SQL_QUERY_SZ];
    char sqlQueryRes[MAX_SQL_QUERY_RES_SZ];

    cJSON *params;

    //Initialise
    relayStrikeTime = 0;
    sqlStringLen = valStringLen = 0;
    doorId = 0;
    relayInterfaceType = INVALID_RELAY;

    memset(sqlQuery, '\0', MAX_SQL_QUERY_SZ);
    memset(sqlQueryRes, '\0', MAX_SQL_QUERY_RES_SZ);
    memset(sqlString, '\0', MAX_SQL_STRING_LEN);
    memset(valString, '\0', MAX_SQL_STRING_LEN);

    params = cJSON_GetObjectItem(cmd, DOOR_ID_JF);
    if (NULL != params) {
        doorId = params->valueint;
        printf("Door ID [%d]\n", doorId);
    }

    params = cJSON_GetObjectItem(cmd, STRIKE_TIME_JF);
    if (NULL != params) {
        relayStrikeTime = params->valueint;
        printf("Strike Time [%d]\n", relayStrikeTime);
    }

    params = cJSON_GetObjectItem(cmd,
            INTERFACE_TYPE_JF);
    if (NULL != params) {
        relayInterfaceType = params->valueint;
        printf("Relay Interface Type [%d]\n", relayInterfaceType);
    }

    // SQL SELECT
    sprintf(sqlQuery, "SELECT relay_id FROM door_relay_mapping WHERE "
            "relay_id=%d", relayId);
    printf("SQL Query [%s]\n", sqlQuery);
    executeSqlToStr(gDbHandle, sqlQuery, MAX_SQL_QUERY_RES_SZ, sqlQueryRes);
    if (!sqlQueryRes[0]) {
        // SQL INSERT
        sprintf(sqlString, "INSERT INTO door_relay_mapping VALUES"
                "(%d,%d,%d,%d)",
                relayId, doorId, relayInterfaceType, relayStrikeTime);
        executeSqlQuery(gDbHandle, sqlString);
    } else {
        // SQL UPDATE
        sqlStringLen = sprintf(sqlString+sqlStringLen, "UPDATE"
                " door_relay_mapping SET ");
        if (doorId) {
            valStringLen += sprintf(valString+valStringLen, " %s=%d,",
                    "door_id", doorId);
        }

        if (relayStrikeTime) {
            valStringLen += sprintf(valString+valStringLen, "%s=%d,",
                    "relay_strike_time", relayStrikeTime);
        }

        if (relayInterfaceType) {
            valStringLen += sprintf(valString+valStringLen, "%s=%d,",
                    "relay_type", relayInterfaceType);
        }

        valString[--valStringLen] = '\0';
        sqlStringLen += sprintf(sqlString+sqlStringLen, "%s WHERE"
                " relay_id=%d", valString, relayId);
        executeSqlQuery(gDbHandle, sqlString);
    }
}

/**
 * @function     : deviceSettingsCmd
 * @brief        : parses the Device Settings/Configuration & stores them in
 *                shared memory for other applications/managers
 * @param1[in]   : cmd
 *                received Device Settings
 * @return       : none
 */
void deviceSettingsCmd(cJSON *cmd)
{
    int idx;
    int res;
    int cmdIdx;
    int doorId;
    int accessServicePid;

    cJSON *deviceSetting;
    cJSON *deviceSettingsArray;

    // Schedule Settings
    cJSON *scheduleVideoSetting;

    // Initialize
    idx = cmdIdx = doorId = res = accessServicePid = 0;

    basicSettings_t basicSettings = BASIC_SETTINGS_INIT;
    cameraSettings_t cameraSettings = CAMERA_SETTINGS_INIT;
    videoSettings_t videoSettings = VIDEO_SETTINGS_INIT;
    snapshotSettings_t snapshotSettings =  SNAPSHOT_SETTINGS_INIT;
    notificationSettings_t notificationSettings =
        NOTIFICATION_SETTINGS_INIT;
    phoneCodeSettings_t phoneCodeSettings = PHONECODE_SETTINGS_INIT;
    bleSettings_t bleSettings = BLE_SETTINGS_INIT;
    motionSettings_t motionSettings = MOTION_SETTINGS_INIT;
    cloudSettings_t cloudSettings = CLOUD_SETTINGS_INIT;
    keypadSettings_t keypadSettings = KEYPAD_SETTINGS_INIT;
    displaySettings_t displaySettings = DISPLAY_SETTINGS_INIT;
    lockoutSettings_t lockoutSettings = LOCKOUT_SETTINGS_INIT;
    networkSettings_t networkSettings = NETWORK_SETTINGS_INIT;
    courtesyLightSettings_t courtesyLightSettings =
        COURTESYLIGHT_SETTINGS_INIT;
    scheduleSettings_t scheduleSettings = SCHEDULE_SETTINGS_INIT;
    sipSettings_t sipSettings = SIP_SETTINGS_INIT;

    // Process Device Setting Command
    for (idx=0; idx<cJSON_GetArraySize(cmd); idx++) {
        deviceSettingsArray = cJSON_GetArrayItem(cmd, idx);
        printf("Device Settings Cmd %s\n", deviceSettingsArray->string);
        if (!strcmp(deviceSettingsArray->string, BASIC_SETTINGS_JF)) {
            // Basic Setting
            deviceSetting = cJSON_GetObjectItem(deviceSettingsArray, NAME_JF);
            if (deviceSetting != NULL) {
                strcpy(basicSettings.deviceName, deviceSetting->valuestring);
                printf("Device Name [%s]:[%s]\n", deviceSetting->valuestring,
                        basicSettings.deviceName);
            }
            deviceSetting = cJSON_GetObjectItem(deviceSettingsArray,
                    SERIAL_NUMBER_JF);
            if (deviceSetting != NULL) {
                strcpy(basicSettings.serialNumber, deviceSetting->valuestring);
                printf("Device Serial Number [%s]:[%s]\n",
                        deviceSetting->valuestring, basicSettings.serialNumber);
            }
            deviceSetting = cJSON_GetObjectItem(deviceSettingsArray, STATUS_JF);
            if (deviceSetting != NULL) {
                if (deviceSetting->valueint == DEVICE_STATUS_DISABLE) {
                    basicSettings.deviceStatus = DEVICE_STATUS_DISABLE;
                } else if (deviceSetting->valueint == DEVICE_STATUS_ENABLE) {
                    basicSettings.deviceStatus = DEVICE_STATUS_ENABLE;
                } else {
                    basicSettings.deviceStatus = DEVICE_STATUS_INVALID;
                }
                printf("Device Status [%d]:[%d]\n", deviceSetting->valueint,
                        basicSettings.deviceStatus);
            }
            deviceSetting = cJSON_GetObjectItem(deviceSettingsArray,
                    DEVICE_TYPE_JF);
            if (deviceSetting != NULL) {
                if (deviceSetting->valueint == MASTER_DEVICE) {
                    basicSettings.deviceType = MASTER_DEVICE;
                } else if (deviceSetting->valueint == SLAVE_DEVICE) {
                    basicSettings.deviceType = SLAVE_DEVICE;
                } else {
                    basicSettings.deviceType = INVALID_DEVICE;
                }
                printf("Device Type [%d]:[%d]\n", deviceSetting->valueint,
                        basicSettings.deviceType);
            }
            deviceSetting = cJSON_GetObjectItem(deviceSettingsArray,
                    TAMPER_DETECTION_JF);
            if (deviceSetting != NULL) {
                if (deviceSetting->valueint == TAMPER_DETECTION_ENABLE) {
                    basicSettings.tamperDetection = TAMPER_DETECTION_ENABLE;
                } else if (deviceSetting->valueint ==
                        TAMPER_DETECTION_DISABLE) {
                    basicSettings.tamperDetection = TAMPER_DETECTION_DISABLE;
                } else {
                    basicSettings.tamperDetection = TAMPER_DETECTION_INVALID;
                }
                printf("Tamper Detection Status [%d]:[%d]\n",
                        deviceSetting->valueint, basicSettings.tamperDetection);
            }
            deviceSetting = cJSON_GetObjectItem(deviceSettingsArray,
                    SPEAKER_VOLUME_JF);
            if (deviceSetting != NULL) {
                if (deviceSetting->valueint == SPEAKER_VOLUME_LOW) {
                    basicSettings.speakerVolume = SPEAKER_VOLUME_LOW;
                } else if (deviceSetting->valueint == SPEAKER_VOLUME_MEDIUM) {
                    basicSettings.speakerVolume = SPEAKER_VOLUME_MEDIUM;
                } else if (deviceSetting->valueint == SPEAKER_VOLUME_HIGH) {
                    basicSettings.speakerVolume = SPEAKER_VOLUME_HIGH;
                } else {
                    basicSettings.speakerVolume = SPEAKER_VOLUME_INVALID;
                }
                printf("Speaker Volume [%d]:[%d]\n", deviceSetting->valueint,
                        basicSettings.speakerVolume);
            }
            deviceSetting = cJSON_GetObjectItem(deviceSettingsArray,
                    MICROPHONE_SENSITIVITY_JF);
            if (deviceSetting != NULL) {
                if (deviceSetting->valueint ==
                        MICROPHONE_SENSITIVITY_LOW) {
                    basicSettings.microphoneSensitivity =
                        MICROPHONE_SENSITIVITY_LOW;
                } else if (deviceSetting->valueint ==
                        MICROPHONE_SENSITIVITY_MEDIUM) {
                    basicSettings.microphoneSensitivity =
                        MICROPHONE_SENSITIVITY_MEDIUM;
                } else if (deviceSetting->valueint ==
                        MICROPHONE_SENSITIVITY_HIGH) {
                    basicSettings.microphoneSensitivity =
                        MICROPHONE_SENSITIVITY_HIGH;
                } else {
                    basicSettings.microphoneSensitivity =
                        MICROPHONE_SENSITIVITY_INVALID;
                }
                printf("Microphone Sensitivity [%d]:[%d]\n",
                        deviceSetting->valueint,
                        basicSettings.microphoneSensitivity);
            }
            deviceSetting = cJSON_GetObjectItem(deviceSettingsArray,
                    TIMESZONE_JF);
            if (deviceSetting != NULL) {
                if (!strcmp(deviceSetting->valuestring, "USA (Pacific)")) {
                    basicSettings.rtcTimezone = USA_TIMEZONE;
                } else {
                    basicSettings.rtcTimezone = INVALID_TIMEZONE;
                }
                printf("RTC Timezone [%s]:[%d]\n", deviceSetting->valuestring,
                        basicSettings.rtcTimezone);
            }
            deviceSetting = cJSON_GetObjectItem(deviceSettingsArray, BEEPER_JF);
            if (deviceSetting != NULL) {
                if (deviceSetting->valueint == BEEPER_ALWAYS_OFF) {
                    basicSettings.beeper = BEEPER_ALWAYS_OFF;
                } else if (deviceSetting->valueint == BEEPER_KEYPAD_ONLY) {
                    basicSettings.beeper = BEEPER_KEYPAD_ONLY;
                } else if (deviceSetting->valueint == BEEPER_ACCESS_ONLY) {
                    basicSettings.beeper = BEEPER_ACCESS_ONLY;
                } else if (deviceSetting->valueint == BEEPER_KEYPAD_ACCESS_BOTH) {
                    basicSettings.beeper = BEEPER_KEYPAD_ACCESS_BOTH;
                } else if (deviceSetting->valueint == BEEPER_ALL) {
                    basicSettings.beeper = BEEPER_ALL;
                } else {
                    basicSettings.beeper = BEEPER_INVALID;
                }
                printf("Beeper [%d]:[%d]\n", deviceSetting->valueint,
                        basicSettings.beeper);
            }
            processRequest(DEVICE_BASIC_SETTING, &basicSettings);
        } else if (!strcmp(deviceSettingsArray->string, CAMERA_SETTINGS_JF)) {
            deviceSetting = cJSON_GetObjectItem(deviceSettingsArray,
                    USERNAME_JF);
            if (deviceSetting != NULL) {
                strcpy(cameraSettings.rtpUsername, deviceSetting->valuestring);
                printf("RTP USername [%s]:[%s]\n", deviceSetting->valuestring,
                        cameraSettings.rtpUsername);
            }
            deviceSetting = cJSON_GetObjectItem(deviceSettingsArray,
                    PASSWORD_JF);
            if (deviceSetting != NULL) {
                strcpy(cameraSettings.rtpPassword, deviceSetting->valuestring);
                printf("RTP Password [%s]:[%s]\n", deviceSetting->valuestring,
                        cameraSettings.rtpPassword);
            }
            deviceSetting = cJSON_GetObjectItem(deviceSettingsArray, DOORS_JF);
            if (deviceSetting != NULL) {
                processDoorArray(deviceSetting, cameraSettings.doors);
            }
            processRequest(DEVICE_CAMERA_SETTING, &cameraSettings);

        } else if (!strcmp(deviceSettingsArray->string, VIDEO_SETTINGS_JF)) {
            // Video Setting
            deviceSetting = cJSON_GetObjectItem(deviceSettingsArray, QUALITY_JF);
            if (deviceSetting != NULL) {
                if (deviceSetting->valueint == RES_480P) {
                    videoSettings.videoQuality = RES_480P;
                } else if (deviceSetting->valueint == RES_720P) {
                    videoSettings.videoQuality = RES_720P;
                } else if (deviceSetting->valueint == RES_1080P) {
                    videoSettings.videoQuality = RES_1080P;
                } else {
                    videoSettings.videoQuality = RES_INVALID;
                }
                printf("Video Quality [%d]:[%d]\n", deviceSetting->valueint,
                        videoSettings.videoQuality);
            }
            deviceSetting = cJSON_GetObjectItem(deviceSettingsArray,
                    FRAME_RATE_JF);
            if (deviceSetting != NULL) {
                if (deviceSetting->valueint == FPS_30) {
                    videoSettings.videoFrameRate = FPS_30;
                } else {
                    videoSettings.videoFrameRate = FPS_INVALID;
                }
                printf("Video Framerate [%d]:[%d]\n", deviceSetting->valueint,
                        videoSettings.videoFrameRate);
            }
            deviceSetting = cJSON_GetObjectItem(deviceSettingsArray,
                    RECORD_DURATION_JF);
            if (deviceSetting != NULL) {
                videoSettings.videoRecordDuration = deviceSetting->valueint;
                printf("Video Recording Duration [%d]:[%d]\n",
                        deviceSetting->valueint,
                        videoSettings.videoRecordDuration);
            }
            deviceSetting = cJSON_GetObjectItem(deviceSettingsArray,
                    RECORD_EVENT_JF);
            if (deviceSetting != NULL) {
                processAccessEvent(deviceSetting,
                        videoSettings.videoRecordEventType);
            }
            processRequest(DEVICE_VIDEO_SETTING, &videoSettings);
        } else if (!strcmp(deviceSettingsArray->string, SNAPSHOT_SETTINGS_JF)) {
            // Snapshot Setting
            deviceSetting = cJSON_GetObjectItem(deviceSettingsArray, FORMAT_JF);
            if (deviceSetting != NULL) {
                if (!strcmp(deviceSetting->valuestring, JPEG_JF)) {
                    snapshotSettings.imageFormat = JPEG_FORMAT;
                } else {
                    snapshotSettings.imageFormat = INVALID_FORMAT;
                }
                printf("Snapshot Format [%s]:[%d]\n", deviceSetting->valuestring,
                        snapshotSettings.imageFormat);
            }
            deviceSetting = cJSON_GetObjectItem(deviceSettingsArray,
                    CAPTURE_EVENT_JF);
            if (deviceSetting != NULL) {
                processAccessEvent(deviceSetting,
                        snapshotSettings.captureEventType);
            }
            processRequest(DEVICE_SNAPSHOT_SETTING, &snapshotSettings);
        } else if (!strcmp(deviceSettingsArray->string,
                    NOTIFICATION_SETTINGS_JF)) {
            // Notification Setting
            deviceSetting = cJSON_GetObjectItem(deviceSettingsArray,
                    EVENTS_JF);
            if (deviceSetting != NULL) {
                processAccessEvent(deviceSetting,
                        notificationSettings.notificationEventType);
            }
            processRequest(DEVICE_NOTIFICATION_SETTING, &notificationSettings);
        } else if (!strcmp(deviceSettingsArray->string, PHONE_CODE_SETTINGS_JF)) {
            // Phone Code Setting
            deviceSetting = cJSON_GetObjectItem(deviceSettingsArray,
                    CALL_TYPE_JF);
            if (deviceSetting != NULL) {
                if (deviceSetting->valueint == CELLULAR_CALL) {
                    phoneCodeSettings.callType = CELLULAR_CALL;
                } else if (deviceSetting->valueint == VOIP_CALL) {
                    phoneCodeSettings.callType = VOIP_CALL;
                } else {
                    phoneCodeSettings.callType = INVALID_CALL;
                }
                printf("Call Type [%d]:[%d]\n", deviceSetting->valueint,
                        phoneCodeSettings.callType);
            }
            deviceSetting = cJSON_GetObjectItem(deviceSettingsArray,
                    CALL_MODE_JF);
            if (deviceSetting != NULL) {
                if (deviceSetting->valueint == VIDEO_MODE) {
                    phoneCodeSettings.callMode = VIDEO_MODE;
                } else if (deviceSetting->valueint == AUDIO_MODE) {
                    phoneCodeSettings.callMode = AUDIO_MODE;
                } else {
                    phoneCodeSettings.callMode = INVALID_MODE;
                }
                printf("Call Mode [%d]:[%d]\n", deviceSetting->valueint,
                        phoneCodeSettings.callMode);
            }
            deviceSetting = cJSON_GetObjectItem(deviceSettingsArray,
                    CALL_DURATION_JF);
            if (deviceSetting != NULL) {
                phoneCodeSettings.callDuration = deviceSetting->valueint;
                printf("Call Duration [%d]:[%d]\n", deviceSetting->valueint,
                        phoneCodeSettings.callDuration);
            }
            deviceSetting = cJSON_GetObjectItem(deviceSettingsArray,
                    ACCESS_GRANTED_KEY_JF);
            if (deviceSetting != NULL) {
                if (!strcmp(deviceSetting->valuestring, "1")) {
                    phoneCodeSettings.accessGrantKey = KEY_1;
                } else if (!strcmp(deviceSetting->valuestring, "2")) { 
                    phoneCodeSettings.accessGrantKey = KEY_2;
                } else if (!strcmp(deviceSetting->valuestring, "3")) {
                    phoneCodeSettings.accessGrantKey = KEY_3;
                } else if (!strcmp(deviceSetting->valuestring, "4")) {
                    phoneCodeSettings.accessGrantKey = KEY_4;
                } else if (!strcmp(deviceSetting->valuestring, "5")) {
                    phoneCodeSettings.accessGrantKey = KEY_5;
                } else if (!strcmp(deviceSetting->valuestring, "6")) {
                    phoneCodeSettings.accessGrantKey = KEY_6;
                } else if (!strcmp(deviceSetting->valuestring, "7")) {
                    phoneCodeSettings.accessGrantKey = KEY_7;
                } else if (!strcmp(deviceSetting->valuestring, "8")) {
                    phoneCodeSettings.accessGrantKey = KEY_8;
                } else if (!strcmp(deviceSetting->valuestring, "9")) {
                    phoneCodeSettings.accessGrantKey = KEY_9;
                } else if (!strcmp(deviceSetting->valuestring, "0")) {
                    phoneCodeSettings.accessGrantKey = KEY_0;
                } else if (!strcmp(deviceSetting->valuestring, "*")) {
                    phoneCodeSettings.accessGrantKey = KEY_ASTERISK;
                } else if (!strcmp(deviceSetting->valuestring, "#")) {
                    phoneCodeSettings.accessGrantKey = KEY_HASH;
                } else {
                    phoneCodeSettings.accessGrantKey = INVALID_KEY;
                }
                printf("Access Grant Key [%s]:[%d]\n", deviceSetting->valuestring,
                        phoneCodeSettings.accessGrantKey);
            }
            processRequest(DEVICE_PHONE_CODE_SETTING, &phoneCodeSettings);
        } else if (!strcmp(deviceSettingsArray->string, BLE_SETTINGS_JF)) {
            // BLE Setting
            deviceSetting = cJSON_GetObjectItem(deviceSettingsArray, BLE_JF);
            if (deviceSetting != NULL) {
                if (deviceSetting->valueint == BLE_DISABLE) {
                    bleSettings.bleStatus = BLE_DISABLE;
                } else if (deviceSetting->valueint == BLE_ENABLE) {
                    bleSettings.bleStatus = BLE_ENABLE;
                } else {
                    bleSettings.bleStatus = BLE_INVALID;
                }
                printf("BLE Status [%d]:[%d]\n", deviceSetting->valueint,
                        bleSettings.bleStatus);
            }
            deviceSetting = cJSON_GetObjectItem(deviceSettingsArray, NAME_JF);
            if (deviceSetting != NULL) {
                strcpy(bleSettings.bleName, deviceSetting->valuestring);
                printf("BLE Name [%s]:[%s]\n", deviceSetting->valuestring,
                        bleSettings.bleName);
            }
            deviceSetting = cJSON_GetObjectItem(deviceSettingsArray,
                    TIMEOUT_JF);
            if (deviceSetting != NULL) {
                bleSettings.timeout = deviceSetting->valueint;
                printf("BLE Timeout [%d]:[%d]\n", deviceSetting->valueint,
                        bleSettings.timeout);
            }
            processRequest(DEVICE_BLE_SETTING, &bleSettings);
        } else if (!strcmp(deviceSettingsArray->string, MOTION_SETTINGS_JF)) {
            // Motion Setting
            deviceSetting = cJSON_GetObjectItem(deviceSettingsArray,
                    MOTION_DETECT_JF);
            if (deviceSetting != NULL) {
                if (deviceSetting->valueint == MOTION_DETECTION_DISABLE) {
                    motionSettings.motionDetectionStatus =
                        MOTION_DETECTION_DISABLE;
                } else if (deviceSetting->valueint == MOTION_DETECTION_ENABLE) {
                    motionSettings.motionDetectionStatus =
                        MOTION_DETECTION_ENABLE;
                } else {
                    motionSettings.motionDetectionStatus =
                        MOTION_DETECTION_INVALID;
                }
                printf("Motion Detection Status [%d]:[%d]\n",
                        deviceSetting->valueint,
                        motionSettings.motionDetectionStatus);
            }
            deviceSetting = cJSON_GetObjectItem(deviceSettingsArray, RANGE_JF);
            if (deviceSetting != NULL) {
                motionSettings.motionDetectionRange = deviceSetting->valueint;
                printf("Motion Detection Range [%d]:[%d]\n",
                        deviceSetting->valueint,
                        motionSettings.motionDetectionRange);
            }
            deviceSetting = cJSON_GetObjectItem(deviceSettingsArray, ZONE_JF);
            if (deviceSetting != NULL) {
                motionSettings.motionDetectionZone =
                    atoi(deviceSetting->valuestring);
                printf("Motion Detection Zone [%s][%d]\n",
                        deviceSetting->valuestring,
                        motionSettings.motionDetectionZone);
            }
            processRequest(DEVICE_MOTION_DETECTION_SETTING, &motionSettings);
        } else if (!strcmp(deviceSettingsArray->string, CLOUD_SETTINGS_JF)) {
            // Storage Setting
            deviceSetting = cJSON_GetObjectItem(deviceSettingsArray, STORAGE_JF);
            if (deviceSetting != NULL) {
                if (deviceSetting->valueint == CLOUD_STORAGE_DISABLE) {
                    cloudSettings.cloudStorageStatus = CLOUD_STORAGE_DISABLE;
                } else if (deviceSetting->valueint == CLOUD_STORAGE_ENABLE) {
                    cloudSettings.cloudStorageStatus = CLOUD_STORAGE_ENABLE;
                } else {
                    cloudSettings.cloudStorageStatus = CLOUD_STORAGE_INVALID;
                }
                printf("Cloud Storage [%d]:[%d]\n", deviceSetting->valueint,
                        cloudSettings.cloudStorageStatus);
            }
            deviceSetting = cJSON_GetObjectItem(deviceSettingsArray,
                    BUCKET_NAME_JF);
            if (deviceSetting != NULL) {
                if (strlen(deviceSetting->valuestring) >= MAX_SIZE) {
                    // TODO: Handle (Notify Cloud)?
                } else {
                    strncpy(cloudSettings.cloudBucketName,
                            deviceSetting->valuestring,
                            MAX_SIZE);
                }
                printf("Cloud Bucket Name [%s]:[%s]\n", deviceSetting->valuestring,
                        cloudSettings.cloudBucketName);
            }
            deviceSetting = cJSON_GetObjectItem(deviceSettingsArray,
                    CLOUD_ACCESS_KEY_JF);
            if (deviceSetting != NULL) {
                if (strlen(deviceSetting->valuestring) >= MAX_SIZE) {
                    // TODO: Handle (Notify Cloud)?
                } else {
                    strncpy(cloudSettings.cloudAccessKey,
                            deviceSetting->valuestring,
                            MAX_SIZE);
                }
                printf("Cloud Access Key [%s]:[%s]\n", deviceSetting->valuestring,
                        cloudSettings.cloudAccessKey);
            }
            deviceSetting = cJSON_GetObjectItem(deviceSettingsArray,
                    CLOUD_ACCESS_SECRET_JF);
            if (deviceSetting != NULL) {
                if (strlen(deviceSetting->valuestring) >= MAX_SIZE) {
                    // TODO: Handle (Notify Cloud)?
                } else {
                    strncpy(cloudSettings.cloudAccessSecret,
                            deviceSetting->valuestring,
                            MAX_SIZE);
                }
                printf("Cloud Access Secret [%s]:[%s]\n",
                        deviceSetting->valuestring,
                        cloudSettings.cloudAccessSecret);
            }
            deviceSetting = cJSON_GetObjectItem(deviceSettingsArray,
                    RECORD_VIDEO_FILE_PATH_JF);
            if (deviceSetting != NULL) {
                if (strlen(deviceSetting->valuestring) >= MAX_FILE_PATH_SIZE) {
                    // TODO: Handle (Notify Cloud)?
                } else {
                    strncpy(cloudSettings.recordVideoFilePath,
                            deviceSetting->valuestring, MAX_FILE_PATH_SIZE);
                }
                printf("Record Video File Path [%s]:[%s]\n", deviceSetting->valuestring,
                        cloudSettings.recordVideoFilePath);
            }
            deviceSetting = cJSON_GetObjectItem(deviceSettingsArray,
                    SNAPSHOT_FILE_PATH_JF);
            if (deviceSetting != NULL) {
                if (strlen(deviceSetting->valuestring) >= MAX_FILE_PATH_SIZE) {
                    // TODO: Handle (Notify Cloud)?
                } else {
                    strncpy(cloudSettings.snapshotFilePath,
                            deviceSetting->valuestring, MAX_FILE_PATH_SIZE);
                }
                printf("Snapshot File Path [%s]:[%s]\n",
                        deviceSetting->valuestring,
                        cloudSettings.snapshotFilePath);
            }
            deviceSetting = cJSON_GetObjectItem(deviceSettingsArray,
                    DIAGNOSTIC_FILE_PATH_JF);
            if (deviceSetting != NULL) {
                if (strlen(deviceSetting->valuestring) >= MAX_FILE_PATH_SIZE) {
                    // TODO: Handle (Notify Cloud)?
                } else {
                    strncpy(cloudSettings.diagnosticFilePath,
                            deviceSetting->valuestring, MAX_FILE_PATH_SIZE);
                }
                printf("Diagnostic File Path [%s]:[%s]\n",
                        deviceSetting->valuestring,
                        cloudSettings.diagnosticFilePath);
            }
            deviceSetting = cJSON_GetObjectItem(deviceSettingsArray,
                    MOTION_VIDEO_FILE_PATH_JF);
            if (deviceSetting != NULL) {
                if (strlen(deviceSetting->valuestring) >= MAX_FILE_PATH_SIZE) {
                    // TODO: Handle (Notify Cloud)?
                } else {
                    strncpy(cloudSettings.motionVideoFilePath,
                            deviceSetting->valuestring, MAX_FILE_PATH_SIZE);
                }
                printf("Motion Video File Path [%s]:[%s]\n",
                        deviceSetting->valuestring,
                        cloudSettings.motionVideoFilePath);
            }
            processRequest(DEVICE_CLOUD_SETTING, &cloudSettings);
        } else if (!strcmp(deviceSettingsArray->string, KEYPAD_SETTINGS_JF)) {
            // Keypad Setting
            deviceSetting = cJSON_GetObjectItem(deviceSettingsArray,
                    CALL_BUTTON_JF);
            if (deviceSetting != NULL) {
                if (!strcmp(deviceSetting->valuestring, CALL_JF)) {
                    keypadSettings.callButton = CALL_BUTTON_CALL;
                } else if (!strcmp(deviceSetting->valuestring, ENTER_JF)) {
                    keypadSettings.callButton = CALL_BUTTON_ENTER;
                } else {
                    keypadSettings.callButton = CALL_BUTTON_INVALID;
                }
                printf("Call Button [%s]:[%d]\n", deviceSetting->valuestring,
                        keypadSettings.callButton);
            }
            deviceSetting = cJSON_GetObjectItem(deviceSettingsArray,
                    HASH_BUTTON_JF);
            if (deviceSetting != NULL) {
                if (!strcmp(deviceSetting->valuestring, ENTER_JF)) {
                    keypadSettings.specialButton = SPECIAL_BUTTON_ENTER;
                } else {
                    keypadSettings.specialButton = SPECIAL_BUTTON_INVALID;
                }
                printf("Hash Button [%s]:[%d]\n", deviceSetting->valuestring,
                        keypadSettings.specialButton);
            }
            deviceSetting = cJSON_GetObjectItem(deviceSettingsArray,
                    MASTER_CODE_JF);
            if (deviceSetting != NULL) {
                keypadSettings.masterCode = deviceSetting->valueint;
                printf("Master Code [%d]:[%d]\n", deviceSetting->valueint,
                        keypadSettings.masterCode);
            }
            deviceSetting = cJSON_GetObjectItem(deviceSettingsArray,
                    LIGHT_BRIGHTNESS_JF);
            if (deviceSetting != NULL) {
                keypadSettings.keypadLightBrightness = deviceSetting->valueint;
                printf("Light Brightness [%d]:[%d]\n", deviceSetting->valueint,
                        keypadSettings.keypadLightBrightness);
            }
            deviceSetting = cJSON_GetObjectItem(deviceSettingsArray,
                    LIGHT_MODE_JF);
            if (deviceSetting != NULL) {
                if (deviceSetting->valueint == KEYPAD_LIGHT_ALWAYS_OFF) {
                    keypadSettings.keypadLightIlluminationMode =
                        KEYPAD_LIGHT_ALWAYS_OFF;
                } else if (deviceSetting->valueint == KEYPAD_LIGHT_ALWAYS_ON) {
                    keypadSettings.keypadLightIlluminationMode =
                        KEYPAD_LIGHT_ALWAYS_ON;
                } else if (deviceSetting->valueint ==
                        KEYPAD_LIGHT_OFF_IN_DAY_LIGHT) {
                    keypadSettings.keypadLightIlluminationMode =
                        KEYPAD_LIGHT_OFF_IN_DAY_LIGHT;
                } else {
                    keypadSettings.keypadLightIlluminationMode =
                        KEYPAD_LIGHT_INVALID;
                }
                printf("Light Mode [%d]:[%d]\n", deviceSetting->valueint,
                        keypadSettings.keypadLightIlluminationMode);
            }
            processRequest(DEVICE_KEYPAD_SETTING, &keypadSettings);
        } else if (!strcmp(deviceSettingsArray->string, DISPLAY_SETTINGS_JF)) {
            // Display Setting
            deviceSetting = cJSON_GetObjectItem(deviceSettingsArray,
                    WELCOME_MESSAGE_JF);
            if (deviceSetting != NULL) {
                if (strlen(deviceSetting->valuestring) >= MAX_MESSAGE_SIZE) {
                    // TODO: Handle (Notify Cloud)?
                } else {
                    strncpy(displaySettings.welcomeMessage,
                            deviceSetting->valuestring, MAX_MESSAGE_SIZE);
                }
                printf("Welcome Message [%s]:[%s]\n", deviceSetting->valuestring,
                        displaySettings.welcomeMessage);
            }
            deviceSetting = cJSON_GetObjectItem(deviceSettingsArray,
                    FONT_TYPE_JF);
            if (deviceSetting != NULL) {
                if (!strcmp(deviceSetting->valuestring, ARIAL_JF)) {
                    displaySettings.fontType = ARIAL_FONT;
                }
                printf("Font Type [%s]:[%d]\n", deviceSetting->valuestring,
                        displaySettings.fontType);
            }
            deviceSetting = cJSON_GetObjectItem(deviceSettingsArray,
                    FONT_SIZE_JF);
            if (deviceSetting != NULL) {
                displaySettings.fontSize = deviceSetting->valueint;
                printf("Font Size [%d]:[%d]\n", deviceSetting->valueint,
                        displaySettings.fontSize);
            }
            processRequest(DEVICE_DISPLAY_SETTING, &displaySettings);
        } else if (!strcmp(deviceSettingsArray->string, LOCKOUT_SETTINGS_JF)) {
            // Lockout Setting
            deviceSetting = cJSON_GetObjectItem(deviceSettingsArray,
                    MAX_ATTEMPTS_JF);
            if (deviceSetting != NULL) {
                lockoutSettings.lockoutMaxAttempts = deviceSetting->valueint;
                printf("Lockout Max Attempts [%d]:[%d]\n", deviceSetting->valueint,
                        lockoutSettings.lockoutMaxAttempts);
            }
            deviceSetting = cJSON_GetObjectItem(deviceSettingsArray,
                    DURATION_JF);
            if (deviceSetting != NULL) {
                lockoutSettings.lockoutDuration = deviceSetting->valueint;
                printf("Lockout Duration [%d]:[%d]\n", deviceSetting->valueint,
                        lockoutSettings.lockoutDuration);
            }
            deviceSetting = cJSON_GetObjectItem(deviceSettingsArray,
                    ACCESS_MODES_JF);
            if (deviceSetting != NULL) {
                processLockoutMode(deviceSetting, lockoutSettings.lockoutMode);
            }
            processRequest(DEVICE_LOCKOUT_SETTING, &lockoutSettings);
        } else if (!strcmp(deviceSettingsArray->string, NETWORK_SETTINGS_JF)) {
            // Network Setting
            deviceSetting = cJSON_GetObjectItem(deviceSettingsArray,
                    PREFERENCE_ONE_JF);
            if (deviceSetting != NULL) {
                if (deviceSetting->valueint == WIFI_NETWORK) {
                    networkSettings.networkPref1 = WIFI_NETWORK;
                } else if (deviceSetting->valueint == ETHERNET_NETWORK) {
                    networkSettings.networkPref1 = ETHERNET_NETWORK;
                } else if (deviceSetting->valueint == CELLULAR_NETWORK) {
                    networkSettings.networkPref1 = CELLULAR_NETWORK;
                } else {
                    networkSettings.networkPref1 = INVALID_NETWORK;
                }
                printf("Network Preference 1 [%d]:[%d]\n", deviceSetting->valueint,
                        networkSettings.networkPref1);
            }
            deviceSetting = cJSON_GetObjectItem(deviceSettingsArray,
                    PREFERENCE_TWO_JF);
            if (deviceSetting != NULL) {
                if (deviceSetting->valueint == WIFI_NETWORK) {
                    networkSettings.networkPref2 = WIFI_NETWORK;
                } else if (deviceSetting->valueint == ETHERNET_NETWORK) {
                    networkSettings.networkPref2 = ETHERNET_NETWORK;
                } else if (deviceSetting->valueint == CELLULAR_NETWORK) {
                    networkSettings.networkPref2 = CELLULAR_NETWORK;
                } else {
                    networkSettings.networkPref2 = INVALID_NETWORK;
                }
                printf("Network Preference 2 [%d]:[%d]\n", deviceSetting->valueint,
                        networkSettings.networkPref2);
            }
            deviceSetting = cJSON_GetObjectItem(deviceSettingsArray,
                    PREFERENCE_THREE_JF);
            if (deviceSetting != NULL) {
                if (deviceSetting->valueint == WIFI_NETWORK) {
                    networkSettings.networkPref3 = WIFI_NETWORK;
                } else if (deviceSetting->valueint == ETHERNET_NETWORK) {
                    networkSettings.networkPref3 = ETHERNET_NETWORK;
                } else if (deviceSetting->valueint == CELLULAR_NETWORK) {
                    networkSettings.networkPref3 = CELLULAR_NETWORK;
                } else {
                    networkSettings.networkPref3 = INVALID_NETWORK;
                }
                printf("Network Preference 3 [%d]:[%d]\n", deviceSetting->valueint,
                        networkSettings.networkPref3);
            }
            processRequest(DEVICE_NETWORK_SETTING, &networkSettings);
        } else if (!strcmp(deviceSettingsArray->string,
                    COURTESY_LIGHT_SETTINGS_JF)) {
            // Courtesy Light Setting
            deviceSetting = cJSON_GetObjectItem(deviceSettingsArray,
                    BRIGHTNESS_JF);
            if (deviceSetting != NULL) {
                courtesyLightSettings.brightness = deviceSetting->valueint;
                printf("Courtesy Light Brightness [%d]:[%d]\n",
                        deviceSetting->valueint,
                        courtesyLightSettings.brightness);
            }
            deviceSetting = cJSON_GetObjectItem(deviceSettingsArray,
                    LIGHT_MODE_JF);
            if (deviceSetting != NULL) {
                if (deviceSetting->valueint == COURTESY_LIGHT_OFF) {
                    courtesyLightSettings.courtesyLightMode = COURTESY_LIGHT_OFF;
                } else if (deviceSetting->valueint == COURTESY_LIGHT_ON) {
                    courtesyLightSettings.courtesyLightMode = COURTESY_LIGHT_ON;
                } else if (deviceSetting->valueint ==
                        COURTESY_LIGHT_OFF_DURING_DAY) {
                    courtesyLightSettings.courtesyLightMode =
                        COURTESY_LIGHT_OFF_DURING_DAY;
                } else {
                    courtesyLightSettings.courtesyLightMode =
                        COURTESY_LIGHT_INVALID;
                }
                printf("Courtesy Light Mode [%d]:[%d]\n", deviceSetting->valueint,
                        courtesyLightSettings.courtesyLightMode);
            }
            processRequest(DEVICE_COURTESY_LIGHT_SETTING, &courtesyLightSettings);
        } else if (!strcmp(deviceSettingsArray->string, SCHEDULE_SETTINGS_JF)) {
            // Schedule Setting
            deviceSetting = cJSON_GetObjectItem(deviceSettingsArray,
                    DIAGNOSTIC_JF);
            if (deviceSetting != NULL) {
                scheduleSettings.scheduleDiagnositc = deviceSetting->valueint;
                printf("Diagnostic [%d]:[%d]\n", deviceSetting->valueint,
                        scheduleSettings.scheduleDiagnositc);
            }
            deviceSetting = cJSON_GetObjectItem(deviceSettingsArray,
                    SNAPSHOT_JF);
            if (deviceSetting != NULL) {
                scheduleSettings.scheduleSnapshot = deviceSetting->valueint;
                printf("Schedule Snapshot [%d]:[%d]\n", deviceSetting->valueint,
                        scheduleSettings.scheduleSnapshot);
            }
            deviceSetting = cJSON_GetObjectItem(deviceSettingsArray,
                    VIDEO_JF);
            if (deviceSetting != NULL) {
                scheduleVideoSetting = cJSON_GetObjectItem(deviceSetting,
                        SCHEDULE_JF);
                if (scheduleVideoSetting != NULL) {
                    scheduleSettings.scheduleVideo =
                        scheduleVideoSetting->valueint;
                    printf("Schedule Video [%d]:[%d]\n",
                            scheduleVideoSetting->valueint,
                            scheduleSettings.scheduleVideo);
                }
                scheduleVideoSetting = cJSON_GetObjectItem(deviceSetting,
                        RECORD_DURATION_JF);
                if (scheduleVideoSetting != NULL) {
                    scheduleSettings.scheduleVideoRecordDuration =
                        scheduleVideoSetting->valueint;
                    printf("Video Record Duration [%d]:[%d]\n",
                        scheduleVideoSetting->valueint,
                        scheduleSettings.scheduleVideoRecordDuration);
                }
            }
            processRequest(DEVICE_SCHEDULE_SETTING, &scheduleSettings);
        } else if (!strcmp(deviceSettingsArray->string, SIP_SETTINGS_JF)) {
            deviceSetting = cJSON_GetObjectItem(deviceSettingsArray,
                    SIP_USERNAME_JF);
            if (deviceSetting != NULL) {
                strcpy(sipSettings.sipUsername, deviceSetting->valuestring);
                printf("SIP Username  [%s]:[%s]\n", deviceSetting->valuestring,
                        sipSettings.sipUsername);
            }
            deviceSetting = cJSON_GetObjectItem(deviceSettingsArray,
                    SIP_PASSWORD_JF);
            if (deviceSetting != NULL) {
                strcpy(sipSettings.sipPassword, deviceSetting->valuestring);
                printf("SIP Password  [%s]:[%s]\n", deviceSetting->valuestring,
                        sipSettings.sipPassword);
            }
            deviceSetting = cJSON_GetObjectItem(deviceSettingsArray,
                    SIP_SERVER_JF);
            if (deviceSetting != NULL) {
                strcpy(sipSettings.sipDomain, deviceSetting->valuestring);
                printf("SIP Domain  [%s]:[%s]\n", deviceSetting->valuestring,
                        sipSettings.sipDomain);
            }

            processRequest(DEVICE_SIP_SETTING, &sipSettings);
            // Trigger Access Service Application to read the SIP settings
            accessServicePid = getPid(ACCESS_PROCESS_NAME);
            if (accessServicePid) {
                printf("PID %d\n", accessServicePid);
                kill(accessServicePid, SIGUSR1);
            }
        }
    }
}


/**
* @function     : processAccessEvent
* @brief        : enable/disable event flags
* @param1[in]   : deviceSetting
*                device Setting JSON
* @param2[out]  : recordEvent
*                event array
* @return       : none
*/
void processAccessEvent(cJSON *deviceSetting, EVENT_TYPE *recordEvent)
{
    int eventIdx;
    int accessGrant, accessDeny, latchCode, motionDetect,
        masterKey, lockout, keypadPlay, tamperDetect;

    cJSON *recordEventArray;

    // Initialize
    accessGrant = accessDeny = latchCode = motionDetect = EVENT_INVALID;
    masterKey = lockout = keypadPlay = tamperDetect = EVENT_INVALID;

    for (eventIdx=0; eventIdx<cJSON_GetArraySize(deviceSetting); eventIdx++) {
        recordEventArray = cJSON_GetArrayItem(deviceSetting, eventIdx);
        printf("Record Events [%d]\n", recordEventArray->valueint);

        if ((recordEventArray->valueint + 1) == EVENT_ACCESS_GRANT) {
            accessGrant = EVENT_FLAG_ENABLE;
        } else if ((recordEventArray->valueint + 1) == EVENT_ACCESS_DENY) {
            accessDeny = EVENT_FLAG_ENABLE;
        } else if ((recordEventArray->valueint + 1) == EVENT_LATCH_CODE) {
            latchCode = EVENT_FLAG_ENABLE;
        } else if ((recordEventArray->valueint + 1) == EVENT_MOTION_DETECTION) {
            motionDetect = EVENT_FLAG_ENABLE;
        } else if ((recordEventArray->valueint + 1) == EVENT_MASTER_KEY) {
            masterKey = EVENT_FLAG_ENABLE;
        } else if ((recordEventArray->valueint + 1) == EVENT_LOCKOUT) {
            lockout = EVENT_FLAG_ENABLE;
        } else if ((recordEventArray->valueint + 1) == EVENT_KEYPAD_PLAY) {
            keypadPlay = EVENT_FLAG_ENABLE;
        } else if ((recordEventArray->valueint + 1) == EVENT_TAMPER_DETECTION) {
            tamperDetect = EVENT_FLAG_ENABLE;
        }
    }

    if (accessGrant != EVENT_INVALID) {
        recordEvent[EVENT_ACCESS_GRANT] = EVENT_ACCESS_GRANT;
    } else {
        recordEvent[EVENT_ACCESS_GRANT] = EVENT_FLAG_DISABLE;
    }
    printf("Record Events Value: [%d]\n", recordEvent[EVENT_ACCESS_GRANT]);

    if (accessDeny != EVENT_INVALID) {
        recordEvent[EVENT_ACCESS_DENY] = EVENT_ACCESS_DENY;
    } else {
        recordEvent[EVENT_ACCESS_DENY] = EVENT_FLAG_DISABLE;
    }
    printf("Record Events Value: [%d]\n", recordEvent[EVENT_ACCESS_DENY]);

    if (latchCode != EVENT_INVALID) {
        recordEvent[EVENT_LATCH_CODE] = EVENT_LATCH_CODE;
    } else {
        recordEvent[EVENT_LATCH_CODE] = EVENT_FLAG_DISABLE;
    }
    printf("Record Events Value: [%d]\n", recordEvent[EVENT_LATCH_CODE]);

    if (motionDetect != EVENT_INVALID) {
        recordEvent[EVENT_MOTION_DETECTION] = EVENT_MOTION_DETECTION;
    } else {
        recordEvent[EVENT_MOTION_DETECTION] = EVENT_FLAG_DISABLE;
    }
    printf("Record Events Value: [%d]\n", recordEvent[EVENT_MOTION_DETECTION]);

    if (masterKey != EVENT_INVALID) {
        recordEvent[EVENT_MASTER_KEY] = EVENT_MASTER_KEY;
    } else {
        recordEvent[EVENT_MASTER_KEY] = EVENT_FLAG_DISABLE;
    }
    printf("Record Events Value: [%d]\n", recordEvent[EVENT_MASTER_KEY]);

    if (lockout != EVENT_INVALID) {
        recordEvent[EVENT_LOCKOUT] = EVENT_LOCKOUT;
    } else {
        recordEvent[EVENT_LOCKOUT] = EVENT_FLAG_DISABLE;
    }
    printf("Record Events Value: [%d]\n", recordEvent[EVENT_LOCKOUT]);

    if (keypadPlay != EVENT_INVALID) {
        recordEvent[EVENT_KEYPAD_PLAY] = EVENT_KEYPAD_PLAY;
    } else {
        recordEvent[EVENT_KEYPAD_PLAY] = EVENT_FLAG_DISABLE;
    }
    printf("Record Events Value: [%d]\n", recordEvent[EVENT_KEYPAD_PLAY]);

    if (tamperDetect != EVENT_INVALID) {
        recordEvent[EVENT_TAMPER_DETECTION] = EVENT_TAMPER_DETECTION;
    } else {
        recordEvent[EVENT_TAMPER_DETECTION] = EVENT_FLAG_DISABLE;
    }
    printf("Record Events Value: [%d]\n", recordEvent[EVENT_TAMPER_DETECTION]);
}

/**
* @function     : processLockoutMode
* @brief        : enable/disable lockout flags
* @param1[in]   : deviceSetting
*                device Setting JSON
* @param2[out]  : lockoutMode
*                lockoutMode array
* @return       : none
*/
void processLockoutMode(cJSON *deviceSetting, LOCKOUT_MODE *lockoutMode)
{
    int lockoutIdx;
    int lockoutKeypad, lockoutBle, lockoutNfc, lockoutRfid;

    cJSON *lockoutAccessModeArray;

    // Initialize
    lockoutKeypad = lockoutBle = lockoutNfc = lockoutRfid = LOCKOUT_INVALID;

    for (lockoutIdx=0; lockoutIdx<cJSON_GetArraySize(deviceSetting);
            lockoutIdx++) {
        lockoutAccessModeArray = cJSON_GetArrayItem(deviceSetting,
                lockoutIdx);
        printf("Lockout Access Mode [%d]\n", lockoutAccessModeArray->valueint);

        if ((lockoutAccessModeArray->valueint + 1) == LOCKOUT_KEYPAD) {
            lockoutKeypad = EVENT_FLAG_ENABLE;
        } else if ((lockoutAccessModeArray->valueint + 1) == LOCKOUT_BLE) {
            lockoutBle = EVENT_FLAG_ENABLE;
        } else if ((lockoutAccessModeArray->valueint + 1) == LOCKOUT_NFC) {
            lockoutNfc = EVENT_FLAG_ENABLE;
        } else if ((lockoutAccessModeArray->valueint + 1) == LOCKOUT_RFID) {
            lockoutRfid = EVENT_FLAG_ENABLE;
        }
    }

    if (lockoutKeypad != LOCKOUT_INVALID) {
        lockoutMode[LOCKOUT_KEYPAD] = LOCKOUT_KEYPAD;
    } else {
        lockoutMode[LOCKOUT_KEYPAD] = EVENT_FLAG_DISABLE;
    }
    printf("Lockout Mode Value: [%d]\n", lockoutMode[LOCKOUT_KEYPAD]);

    if (lockoutBle != LOCKOUT_INVALID) {
        lockoutMode[LOCKOUT_BLE] = LOCKOUT_BLE;
    } else {
        lockoutMode[LOCKOUT_BLE] = EVENT_FLAG_DISABLE;
    }
    printf("Lockout Mode Value: [%d]\n", lockoutMode[LOCKOUT_BLE]);

    if (lockoutNfc != LOCKOUT_INVALID) {
        lockoutMode[LOCKOUT_NFC] = LOCKOUT_NFC;
    } else {
        lockoutMode[LOCKOUT_NFC] = EVENT_FLAG_DISABLE;
    }
    printf("Lockout Mode Value: [%d]\n", lockoutMode[LOCKOUT_NFC]);

    if (lockoutRfid != LOCKOUT_INVALID) {
        lockoutMode[LOCKOUT_RFID] = LOCKOUT_RFID;
    } else {
        lockoutMode[LOCKOUT_RFID] = EVENT_FLAG_DISABLE;
    }
    printf("Lockout Mode Value: [%d]\n", lockoutMode[LOCKOUT_RFID]);
}

/**
* @function     : processDoorArray
* @brief        : parse door list
* @param1[in]   : deviceSetting
*                device Setting JSON
* @param2[out]  : doors
*                doors array
* @return       : none
*/
void processDoorArray(cJSON *deviceSetting, int *doors)
{
    int doorIdx;

    cJSON *doorArray;

    // Initialize

    for (doorIdx=0; doorIdx<cJSON_GetArraySize(deviceSetting);
            doorIdx++) {
        doorArray = cJSON_GetArrayItem(deviceSetting,
                doorIdx);
        printf("door Id [%d]\n", doorArray->valueint);
        doors[doorIdx] = doorArray->valueint;
    }
}


/**
* @function     : dataFixUp
* @brief        : fix the data when length exceeds the datalength
* @param1[in]   : dataLen
*                data length
* @param2[in]   : length
*                calculated length
* @param3[in]   : data
*                required data
* @return       : return calculated length
*/
int dataFixUp(int dataLen, int length, char *data)
{
    length = dataLen;
    while (data[length] != ',') {
        length--;
    }
    if (length == dataLen) {
        length--;
        while (data[length] != ',') {
            length--;
        }
    }
    length++;

    return length;
}

/**
 * @function     : getPid
 * @brief        : get process ID
 * @param1[in]   : processName
 *                process name
 * @return       : returns process ID
 */
int getPid(const char* processName)
{
    int pid = 0;
    DIR *dir = NULL;
    FILE *fp = NULL;
    struct dirent *entry;
    struct stat statBuf;
    char procPath[MAX_FILENAME_LEN], fileName[MAX_FILENAME_LEN];

    if ((dir=opendir(PROC_PATH)) == NULL) {
        printf("Can't open \"/proc\" directory\n\n");
        return pid;
    }

    memset(procPath, 0, sizeof(MAX_FILENAME_LEN));
    memset(fileName, 0, sizeof(MAX_FILENAME_LEN));

    while ((entry=readdir(dir)) != NULL) {
        lstat(entry->d_name, &statBuf);
        if (S_ISDIR(statBuf.st_mode) &&
                isdigit(entry->d_name[ZEROTH_POSITION])) {
            sprintf(procPath, "/proc/%s/status", entry->d_name);

            fp = fopen(procPath, "r");
            if (!fp) {
                continue;
            }
            fscanf(fp, "%s", fileName);
            memset(fileName, 0, sizeof(MAX_FILENAME_LEN));
            fscanf(fp, "%s", fileName);

            fclose(fp);

            if (!strncmp(processName, fileName, MAX_FILENAME_COMPARE)) {
                pid = atoi(entry->d_name);
                break;
            }
        }
    }
    closedir(dir);

    return pid;
}

/**
* @thread       : WDTClientThread
* @brief        : watchdog client thread
* @param1[in]   : arg
*                thread argument
* @return       : none
*/
void *WDTClientThread(void *arg)
{
    while(1) {
        // Wait for ALIVE_Q message
        if (!wdmQueryMsgRecv(WDM_CCA_MSG)) {
            // Send ALIVE message as a response to ALIVE_Q
            wdmMessage(ALIVE, CLOUD_COMMUNICATION_APP, 0, 0);
        }
    }
}


/**
* NOTES: (TODO)
* 1. Need to write a logic to send command to dependent device on the basis of
*   Serial Number
* 2. Need to update logic to increase the number of relays to maximum possible
*/
