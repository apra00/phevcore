#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#include <stdint.h>
#include "phev_pipe.h"
#include "phev_service.h"
#include "msg_utils.h"
#include "logger.h"
#ifdef __XTENSA__
#include "cJSON.h"
#else
#include <cjson/cJSON.h>
#endif
const static char *TAG = "PHEV_SERVICE";

const static uint8_t *DEFAULT_MAC[6] = {0, 0, 0, 0, 0, 0};

int phev_service_eventHandler(phev_pipe_ctx_t *ctx, phevPipeEvent_t *event)
{
    LOG_V(TAG, "START - eventHandler");
    switch (event->event)
    {
        case PHEV_PIPE_REGISTRATION_COMPLETE:
        {
            LOG_I(TAG,"Registration completed");
            phevServiceCtx_t * srvCtx = (phevServiceCtx_t *) ctx->ctx;
            if(srvCtx->registrationCompleteCallback != NULL)
            {
                LOG_D(TAG,"Calling registration callback");
                srvCtx->registrationCompleteCallback(ctx);
            }
            break;
        }
        case PHEV_PIPE_GOT_VIN:
        {
            phevServiceCtx_t * srvCtx = (phevServiceCtx_t *) ctx->ctx;

            if(ctx->registerDevice)
            {
                LOG_D(TAG, "Got VIN and in registration mode so sending register device request");
                phev_pipe_sendRegister(ctx);
            }
            break;
        }
        case PHEV_PIPE_REG_UPDATE_ACK:
        {
            phevMessage_t * message = (phevMessage_t *) event->data;
            LOG_I(TAG,"Register ACK %02X", message->reg);
        }
    }
    LOG_V(TAG, "END - eventHandler");
    return 0;
}
phevServiceCtx_t *phev_service_create(phevServiceSettings_t settings)
{
    LOG_V(TAG, "START - create");
    phevServiceCtx_t *ctx = NULL;

    if(settings.my18)
    {
        phev_core_my18 = true;
    }

    ctx = phev_service_init(settings.in, settings.out,settings.registerDevice);

    ctx->yieldHandler = settings.yieldHandler;
    ctx->exit = false;
    ctx->ctx = settings.ctx;
    ctx->registrationCompleteCallback = NULL;
    if (settings.mac)
    {
        memcpy(ctx->mac, settings.mac, 6);
    }
    else
    {
        memcpy(ctx->mac, DEFAULT_MAC, 6);
    }

    if (settings.eventHandler)
    {
        LOG_D(TAG,"Settings event handler %p",settings.eventHandler);
        phev_pipe_registerEventHandler(ctx->pipe, settings.eventHandler);
    }

    if(settings.registerDevice)
    {
        LOG_D(TAG,"Settings registration event handler %p",phev_service_eventHandler);
        phev_pipe_registerEventHandler(ctx->pipe, phev_service_eventHandler);
    }

    LOG_V(TAG, "END - create");

    return ctx;
}

void phev_service_start(phevServiceCtx_t *ctx)
{
    LOG_V(TAG, "START - start");

    phev_pipe_start(ctx->pipe, ctx->mac);

    while (!ctx->exit)
    {
        phev_service_loop(ctx);
        if (ctx->yieldHandler)
        {
            ctx->yieldHandler(ctx);
        }
    }
    LOG_V(TAG, "END - start");
}
phevServiceCtx_t *phev_service_init(messagingClient_t *in, messagingClient_t *out, bool registerDevice)
{
    LOG_V(TAG, "START - init");

    phevServiceCtx_t *ctx = malloc(sizeof(phevServiceCtx_t));

    LOG_D(TAG, "Creating model and pipe");
    ctx->model = phev_model_create();
    ctx->registerDevice = registerDevice;
    ctx->pipe = phev_service_createPipe(ctx, in, out);
    ctx->pipe->ctx = ctx;

    LOG_V(TAG, "END - init");
    return ctx;
}
messageBundle_t *phev_service_inputSplitter(void *ctx, message_t *message)
{
    LOG_V(TAG, "START - inputSplitter");

    messageBundle_t *messages = malloc(sizeof(messageBundle_t));
    messages->numMessages = 0;
    cJSON *command = NULL;

    cJSON *json = cJSON_Parse((char *)message->data);

    if (!json)
    {
        LOG_W(TAG, "Not valid JSON");
        return NULL;
    }

    cJSON *requests = cJSON_GetObjectItemCaseSensitive(json, "requests");

    if (!requests)
    {
        LOG_W(TAG, "Not valid JSON requests");
        return NULL;
    }

    cJSON_ArrayForEach(command, requests)
    {
        const char *out = cJSON_Print(command);
        messages->messages[messages->numMessages++] = msg_utils_createMsg((uint8_t *)out, strlen(out) + 1);
    }

    LOG_V(TAG, "END - inputSplitter");

    return messages;
}
void phev_service_bufferDump(const uint8_t * buffer, const size_t length)
{
    if(length <= 0 || buffer == NULL) return;

    char out[17];
    memset(&out,'\0',17);

    printf("%s: ",TAG);
    int i = 0;
    for(i=0;i<length;i++)
    {
        printf("%02x ",buffer[i]);
        out[i % 16] = (isprint(buffer[i]) ? buffer[i] : '.');
        if((i+1) % 8 == 0) printf(" ");
        if((i+1) % 16 ==0) {
            out[16] = '\0';
            printf(" | %s |\n%s: ",out,TAG);
        }
    }
    if((i % 16) + 1 != 0)
    {
        int num = (16 - (i % 16)) * 3;
        num = ((i % 16) < 8 ? num + 1 : num);
        out[(i % 16)] = '\0';
        char padding[(16 * 3) + 2];
        memset(&padding,' ',num+1);
        padding[(16-i)*3] = '\0';
        printf("%s | %s |\n",padding,out);
    }
    printf("\n");
}
bool phev_service_outputFilter(void *ctx, message_t *message)
{
    LOG_V(TAG, "START - outputFilter");
    LOG_D(TAG, "Incoming Message");

    LOG_BUFFER_HEXDUMP(TAG,message->data,message->length,LOG_DEBUG);

    phevServiceCtx_t *serviceCtx = ((phev_pipe_ctx_t *)ctx)->ctx;

    phevMessage_t phevMessage;

    phev_core_decodeMessage(message->data, message->length, &phevMessage);

    if ((phevMessage.command == PING_RESP_CMD )|| (phevMessage.command == START_RESP))
    {
        LOG_D(TAG, "Not sending ping or start response");
        free(phevMessage.data);
        return true;
    }
    LOG_D(TAG, "Reg %d", phevMessage.reg);

    if (phevMessage.command == RESP_CMD && phevMessage.type == REQUEST_TYPE)
    {
        phevRegister_t *reg = phev_model_getRegister(serviceCtx->model, phevMessage.reg);

        if (reg)
        {
            LOG_D(TAG, "Register has previously been set Reg %02X",phevMessage.reg);
            LOG_D(TAG,"Register Data len is %d and data",reg->length);
            LOG_BUFFER_HEXDUMP(TAG,reg->data,reg->length,LOG_DEBUG);

            int same = phev_model_compareRegister(serviceCtx->model, phevMessage.reg, phevMessage.data);
            if (same != 0)
            {
                LOG_D(TAG, "Setting Reg %d", phevMessage.reg);

                phev_model_setRegister(serviceCtx->model, phevMessage.reg, phevMessage.data, phevMessage.length);

                free(phevMessage.data);

                return true;
            }
            LOG_D(TAG, "Is same %d", same);
            free(phevMessage.data);
            phevPipeEvent_t *event = malloc(sizeof(phevPipeEvent_t));
            event->data = NULL;
            event->event = PHEV_PIPE_FILTERED_MESSAGE;
            event->length = 0;
            phev_pipe_sendEventToHandlers((phev_pipe_ctx_t *) ctx, event);

            return false;
        }
        else
        {
            LOG_D(TAG, "Setting Reg %d", phevMessage.reg);

            phev_model_setRegister(serviceCtx->model, phevMessage.reg, phevMessage.data, phevMessage.length);
        }
    }
    free(phevMessage.data);

    LOG_V(TAG, "END - outputFilter");

    return true;
}
phev_pipe_ctx_t *phev_service_createPipe(phevServiceCtx_t *ctx, messagingClient_t *in, messagingClient_t *out)
{
    LOG_V(TAG, "START - createPipe");

    phev_pipe_settings_t settings = {
        .ctx = ctx,
        .in = in,
        .out = out,
        .inputInputTransformer = phev_service_jsonInputTransformer,
        .inputOutputTransformer = NULL,
        .inputSplitter = phev_service_inputSplitter,
        .inputAggregator = NULL,
        .outputAggregator = phev_service_jsonResponseAggregator,
        .outputSplitter = phev_pipe_outputSplitter,
        .outputFilter = phev_service_outputFilter,
        .inputResponder = NULL,
        .outputResponder = phev_pipe_commandResponder,
        .preConnectHook = NULL,
        .outputInputTransformer = phev_pipe_outputChainInputTransformer,
        .outputOutputTransformer = phev_service_jsonOutputTransformer,
        .registerDevice = ctx->registerDevice,
    };

    phev_pipe_ctx_t *pipe = phev_pipe_createPipe(settings);

    LOG_V(TAG, "END - createPipe");
    return pipe;
}
bool phev_service_checkByte(uint16_t num)
{
    return (num >= 0 && num < 256);
}
uint8_t phev_service_getJsonByte(cJSON *json, char *option)
{
    cJSON *value = cJSON_GetObjectItemCaseSensitive(json, option);

    if (value != NULL && cJSON_IsNumber(value))
    {
        return ((uint8_t)value->valueint) & 0xff;
    }
    return 0;
}
uint8_t *phev_service_getJsonByteArray(cJSON *json, char *option, uint8_t **data)
{
    cJSON *value = cJSON_GetObjectItemCaseSensitive(json, option);

    if (value)
    {
        if (cJSON_IsArray(value))
        {
            return NULL;
        }
        else
        {
            if (cJSON_IsNumber(value))
            {
                if (phev_service_checkByte(value->valueint))
                {
                    *data = malloc(1);
                    *data[0] = value->valueint;
                    return *data;
                }
                else
                {
                    return NULL;
                }
            }
            else
            {
                return NULL;
            }
        }
    }
    return NULL;
}
uint16_t phev_service_getJsonInt(cJSON *json, char *option)
{
    cJSON *value = cJSON_GetObjectItemCaseSensitive(json, option);

    if (value != NULL)
    {
        return value->valueint;
    }
    return 0;
}
bool phev_service_validateCheckOnOrOff(const char *str)
{
    if (str)
    {
        if (strcmp(str, PHEV_SERVICE_ON_JSON) == 0)
        {
            return true;
        }

        if (strcmp(str, PHEV_SERVICE_OFF_JSON) == 0)
        {
            return true;
        }
    }

    return false;
}
bool phev_service_validateCommand(const char *command)
{
    cJSON *json = cJSON_Parse(command);

    if (json == NULL)
    {
        return false;
    }

    cJSON *update = cJSON_GetObjectItemCaseSensitive(json, PHEV_SERVICE_UPDATE_REGISTER_JSON);

    cJSON *operation = cJSON_GetObjectItemCaseSensitive(json, PHEV_SERVICE_OPERATION_JSON);

    if (update != NULL)
    {
        cJSON *reg = cJSON_GetObjectItemCaseSensitive(update, PHEV_SERVICE_UPDATE_REGISTER_REG_JSON);

        cJSON *value = cJSON_GetObjectItemCaseSensitive(update, PHEV_SERVICE_UPDATE_REGISTER_VALUE_JSON);

        if (value != NULL && reg != NULL)
        {
            if (phev_service_checkByte(reg->valueint) == false)
            {
                return false;
            }
            if (cJSON_IsArray(value))
            {
                cJSON *val = NULL;
                cJSON_ArrayForEach(val, value)
                {
                    if (cJSON_IsNumber(val))
                    {
                        if (phev_service_checkByte(val->valueint) == false)
                        {
                            return false;
                        }
                    }
                    else
                    {
                        return false;
                    }
                }
                return true;
            }
            else
            {
                return phev_service_checkByte(value->valueint);
            }
        }
    }

    if (operation != NULL)
    {
        cJSON *headLights = cJSON_GetObjectItemCaseSensitive(operation, PHEV_SERVICE_OPERATION_HEADLIGHTS_JSON);

        if (headLights)
        {
            return phev_service_validateCheckOnOrOff(headLights->valuestring);
        }
        cJSON *airCon = cJSON_GetObjectItemCaseSensitive(operation, PHEV_SERVICE_OPERATION_AIRCON_JSON);

        if (airCon)
        {
            return phev_service_validateCheckOnOrOff(airCon->valuestring);
        }
    }

    return false;
}
phevMessage_t *phev_service_updateRegisterHandler(cJSON *update)
{
    if (update == NULL)
    {
        LOG_W(TAG, "Update register called with null json object");
        return NULL;
    }
    cJSON *reg = cJSON_GetObjectItemCaseSensitive(update, PHEV_SERVICE_UPDATE_REGISTER_REG_JSON);
    cJSON *value = cJSON_GetObjectItemCaseSensitive(update, PHEV_SERVICE_UPDATE_REGISTER_VALUE_JSON);

    if (!reg)
    {
        LOG_W(TAG, "Register not in update request");
        return NULL;
    }
    if (!value)
    {
        LOG_W(TAG, "Value not in update request");
        return NULL;
    }
    if (cJSON_IsArray(value))
    {
        //printf("Is array");
        cJSON *val = NULL;
        size_t size = cJSON_GetArraySize(value);
        uint8_t *data = malloc(size);
        int i = 0;
        cJSON_ArrayForEach(val, value)
        {
            if (cJSON_IsNumber(val))
            {
                //printf("Is number %d\n",val->valueint);

                data[i++] = val->valueint;
                //printf("\nData %d\n",data[i-1]);
            }
            else
            {
                LOG_W(TAG, "Update register has invalid value");
                return NULL;
            }
        }

        return phev_core_commandMessage(reg->valueint, data, size);
    }
    else
    {
        if (cJSON_IsNumber(value))
        {
            return phev_core_simpleRequestCommandMessage(reg->valueint & 0xff, value->valueint & 0xff);
        }
    }
    return NULL;
}
phevMessage_t *phev_service_operationHandler(cJSON *operation)
{
    cJSON *headLights = cJSON_GetObjectItemCaseSensitive(operation, PHEV_SERVICE_OPERATION_HEADLIGHTS_JSON);

    if (headLights)
    {
        if (strcmp(headLights->valuestring, PHEV_SERVICE_ON_JSON) == 0)
        {
            LOG_D(TAG, "Sending head lights on command");

            return phev_core_simpleRequestCommandMessage(KO_WF_H_LAMP_CONT_SP, 1);
        }
        if (strcmp(headLights->valuestring, PHEV_SERVICE_OFF_JSON) == 0)
        {
            LOG_D(TAG, "Sending head lights off command");

            return phev_core_simpleRequestCommandMessage(KO_WF_H_LAMP_CONT_SP, 2);
        }
        return NULL;
    }

    cJSON *airCon = cJSON_GetObjectItemCaseSensitive(operation, PHEV_SERVICE_OPERATION_AIRCON_JSON);

    if (airCon)
    {
        if (strcmp(airCon->valuestring, PHEV_SERVICE_ON_JSON) == 0)
        {
            LOG_I(TAG, "Sending air con on command");

            return phev_core_simpleRequestCommandMessage(KO_WF_MANUAL_AC_ON_RQ_SP, 2);
        }
        if (strcmp(airCon->valuestring, PHEV_SERVICE_OFF_JSON) == 0)
        {
            LOG_I(TAG, "Sending air con off command");
            return phev_core_simpleRequestCommandMessage(KO_WF_MANUAL_AC_ON_RQ_SP, 1);
        }
        return NULL;
    }

    cJSON *update = cJSON_GetObjectItemCaseSensitive(operation, PHEV_SERVICE_OPERATION_UPDATE_JSON);

    if (update)
    {
        if (cJSON_IsBool(update) && cJSON_IsTrue(update))
        {
            LOG_I(TAG, "Sending update command");

            return phev_core_simpleRequestCommandMessage(KO_WF_EV_UPDATE_SP, 3);
        }
        return NULL;
    }


    return NULL;
}
phevMessage_t *phev_service_jsonCommandToPhevMessage(const char *command)
{
    if (phev_service_validateCommand(command))
    {
        cJSON *json = cJSON_Parse(command);

        cJSON *update = cJSON_GetObjectItemCaseSensitive(json, PHEV_SERVICE_UPDATE_REGISTER_JSON);

        cJSON *operation = cJSON_GetObjectItemCaseSensitive(json, PHEV_SERVICE_OPERATION_JSON);

        if (update)
        {
            return phev_service_updateRegisterHandler(update);
        }
        if (operation)
        {
            return phev_service_operationHandler(operation);
        }
    }
    else
    {
        return NULL;
    }
    return NULL;
}

message_t *phev_service_jsonInputTransformer(void *ctx, message_t *message)
{
    phev_pipe_ctx_t * pipeCtx = (phev_pipe_ctx_t *) ctx;

    if (message)
    {
        if(!pipeCtx->connected)
        {
            LOG_W(TAG,"Not sending command as not connected");
            return NULL;
        }
        phevMessage_t *phevMessage = phev_service_jsonCommandToPhevMessage((char *)message->data);
        if (phevMessage)
        {
            //message_t *out = phev_core_convertToMessage(phevMessage);
            //message_t *encoded = phev_core_XOROutboundMessage(out,pipeCtx->commandXOR);
            //if (encoded)
            //{

            LOG_I(TAG,"Phev message command %02X reg %02X type %02X length %02X",phevMessage->command,phevMessage->reg,phevMessage->type,phevMessage->length);
            if(phevMessage->length == 1)
            {
                phev_pipe_updateRegister(pipeCtx,phevMessage->reg,phevMessage->data[0]);
            }
            else
            {
                phev_pipe_updateComplexRegister(pipeCtx,phevMessage->reg,phevMessage->data,phevMessage->length);
            }
            return NULL;//encoded;
        //    }
        }
    }
    return NULL;
}

cJSON *phev_service_updatedRegister(cJSON *json, phevMessage_t *phevMessage)
{
    cJSON *updatedRegister = cJSON_CreateObject();
    if (updatedRegister == NULL)
    {
        return NULL;
    }

    cJSON_AddItemToObject(json, PHEV_SERVICE_UPDATED_REGISTER_JSON, updatedRegister);

    cJSON *reg = cJSON_CreateNumber(phevMessage->reg);
    if (reg == NULL)
    {
        return NULL;
    }

    cJSON_AddItemToObject(updatedRegister, "register", reg);

    cJSON *length = cJSON_CreateNumber(phevMessage->length);
    if (length == NULL)
    {
        return NULL;
    }

    cJSON_AddItemToObject(updatedRegister, "length", length);

    cJSON *data = cJSON_CreateArray();
    if (data == NULL)
    {
        return NULL;
    }
    cJSON_AddItemToObject(updatedRegister, "data", data);

    for (int i = 0; i < phevMessage->length; i++)
    {
        cJSON *item = cJSON_CreateNumber(phevMessage->data[i]);
        if (item == NULL)
        {
            return NULL;
        }
        cJSON_AddItemToArray(data, item);
    }
    cJSON *xor = cJSON_CreateNumber(phevMessage->XOR);
    if (xor == NULL)
    {
        return NULL;
    }

    cJSON_AddItemToObject(updatedRegister, "xor", xor);


    return json;
}
cJSON *phev_service_updateRegisterAck(cJSON *json, phevMessage_t *phevMessage)
{
    cJSON *updatedRegisterAck = cJSON_CreateObject();
    if (updatedRegisterAck == NULL)
    {
        return NULL;
    }

    cJSON_AddItemToObject(json, PHEV_SERVICE_UPDATED_REGISTER_ACK_JSON, updatedRegisterAck);

    cJSON *reg = cJSON_CreateNumber(phevMessage->reg);
    if (reg == NULL)
    {
        return NULL;
    }

    cJSON_AddItemToObject(updatedRegisterAck, "register", reg);

    cJSON *xor = cJSON_CreateNumber(phevMessage->XOR);
    if (xor == NULL)
    {
        return NULL;
    }

    cJSON_AddItemToObject(updatedRegisterAck, "xor", xor);

    return json;
}
cJSON * phev_service_sendStart(cJSON *json, phevMessage_t *phevMessage)
{
    cJSON *startMessage = cJSON_CreateObject();
    if (startMessage == NULL)
    {
        return NULL;
    }

    cJSON_AddItemToObject(json, PHEV_SERVICE_START_MESSAGE_JSON, startMessage);

    cJSON *length = cJSON_CreateNumber(phevMessage->length);
    if (length == NULL)
    {
        return NULL;
    }
    cJSON_AddItemToObject(startMessage, "length", length);

    cJSON *data = cJSON_CreateArray();
    if (data == NULL)
    {
        return NULL;
    }
    cJSON_AddItemToObject(startMessage, "data", data);

    for (int i = 0; i < phevMessage->length; i++)
    {
        cJSON *item = cJSON_CreateNumber(phevMessage->data[i]);
        if (item == NULL)
        {
            return NULL;
        }
        cJSON_AddItemToArray(data, item);
    }

    return json;
}
message_t *phev_service_jsonOutputTransformer(void *ctx, message_t *message)
{
    LOG_V(TAG, "START - jsonOutputTransformer");

    phevServiceCtx_t *serviceCtx = NULL;

    if (ctx != NULL)
    {
        serviceCtx = ((phev_pipe_ctx_t *)ctx)->ctx;

        message_t * ret = phev_pipe_outputEventTransformer(ctx, message);
        msg_utils_destroyMsg(ret);
    }
    phevMessage_t *phevMessage = malloc(sizeof(phevMessage_t));

    phev_core_decodeMessage(message->data, message->length, phevMessage);
    char *output;
    cJSON *out = NULL;

    //msg_utils_destroyMsg(message);
    cJSON *response = cJSON_CreateObject();

    if (response == NULL)
    {
        phev_core_destroyMessage(phevMessage);
        return NULL;
    }

    switch(phevMessage->command)
    {
    case 0x4e:
    case 0x5e:
    {
        out = phev_service_sendStart(response, phevMessage);
        break;
    }
    case 0x6f:
    {
        if (phevMessage->type == REQUEST_TYPE)
        {
            out = phev_service_updatedRegister(response, phevMessage);
        }
        else
        {
            out = phev_service_updateRegisterAck(response, phevMessage);
        }
        break;
    }
    default:
    {
        cJSON_Delete(response);
        phev_core_destroyMessage(phevMessage);
        return NULL;
    }
    }

    if (!out)
    {
        cJSON_Delete(response);
        phev_core_destroyMessage(phevMessage);
        return NULL;
    }

    time_t now;
    struct tm *timeinfo;
    time(&now);

    char buf[21];
    strftime(buf, sizeof buf, "%FT%TZ", gmtime(&now));

    cJSON * time = cJSON_CreateString(buf);

    cJSON_AddItemToObject(out, "time", time);

    output = cJSON_Print(out);

    message_t *outputMessage = msg_utils_createMsg((uint8_t *)output, strlen(output) );
    LOG_BUFFER_HEXDUMP(TAG, outputMessage->data, outputMessage->length, LOG_DEBUG);
    cJSON_Delete(response);
    phev_core_destroyMessage(phevMessage);
    free(output);
    LOG_V(TAG, "END - jsonOutputTransformer");

    return outputMessage;
}
int phev_service_getBatteryLevel(phevServiceCtx_t *ctx)
{
    LOG_V(TAG, "START - getBatteryLevel");

    phevRegister_t *reg = phev_model_getRegister(ctx->model, KO_WF_BATT_LEVEL_INFO_REP_EVR);

    LOG_V(TAG, "END - getBatteryLevel");
    return (reg ? (int )reg->data[0] : -1);
}

int phev_service_getBatteryWarning(phevServiceCtx_t *ctx)
{
    LOG_V(TAG, "START - getBatteryWarning");

    phevRegister_t *reg = phev_model_getRegister(ctx->model, KO_WF_CHG_GUN_STATUS_EVR);

    LOG_V(TAG, "END - getBatteryWarning");
    return (reg ? (int )reg->data[2] : -1);
}

int phev_service_doorIsLocked(phevServiceCtx_t *ctx)
{
    LOG_V(TAG, "START - doorIsLocked");

    phevRegister_t *reg = phev_model_getRegister(ctx->model, KO_WF_DOOR_STATUS_INFO_REP_EVR);

    LOG_V(TAG, "END - doorIsLocked");
    return (reg ? (int )reg->data[0] : -1);
}
char *phev_service_statusAsJson(phevServiceCtx_t *ctx)
{

    LOG_V(TAG, "START - statusAsJson");
    cJSON *json = cJSON_CreateObject();
    cJSON *status = cJSON_CreateObject();
    cJSON *battery = cJSON_CreateObject();
    cJSON *date = cJSON_CreateObject();

    if (json && status && battery)
    {
        LOG_I(TAG, "Battery Request");

        int battLevel = phev_service_getBatteryLevel(ctx);

        LOG_I(TAG, "Battery level %d", battLevel);

        if (battLevel >= 0)
        {
            cJSON *level = cJSON_CreateNumber((double)battLevel);
            cJSON_AddItemToObject(battery, PHEV_SERVICE_BATTERY_SOC_JSON, level);
        }
        cJSON_AddItemToObject(status, PHEV_SERVICE_BATTERY_JSON, battery);
        cJSON_AddItemToObject(json, PHEV_SERVICE_STATUS_JSON, status);

        char * dateStr = phev_service_getDateSync(ctx);

        if(dateStr)
        {
            date->valuestring = dateStr;
            cJSON_AddStringToObject(status, PHEV_SERVICE_DATE_SYNC_JSON, dateStr);
        }

        if(phev_service_getChargingStatus(ctx))
        {
            cJSON * chargingRemain = cJSON_CreateNumber((double) phev_service_getRemainingChargeTime(ctx));
            cJSON * charging = phev_service_getChargingStatus(ctx) ? cJSON_CreateTrue() : cJSON_CreateFalse();
            cJSON_AddItemToObject(battery,PHEV_SERVICE_CHARGE_REMAIN_JSON, chargingRemain);
            cJSON_AddItemToObject(battery,PHEV_SERVICE_CHARGING_STATUS_JSON,charging);
        }

        phevServiceHVAC_t * hvac = phev_service_getHVACStatus(ctx);

        if(hvac)
        {
            cJSON * hvacStatus = cJSON_CreateObject();
            cJSON_AddItemToObject(hvacStatus, PHEV_SERVICE_HVAC_OPERATING_JSON, hvac->operating ? cJSON_CreateTrue() : cJSON_CreateFalse());
            cJSON * mode = cJSON_CreateNumber((double) ((uint8_t) hvac->mode & 0x0f));
            cJSON * time = cJSON_CreateNumber((double) ((uint8_t) (hvac->mode & 0xf0) >> 4));
            cJSON_AddItemToObject(hvacStatus, PHEV_SERVICE_HVAC_MODE_JSON, mode);
            cJSON_AddItemToObject(hvacStatus, PHEV_SERVICE_HVAC_TIME_JSON, time);
            cJSON_AddItemToObject(status,PHEV_SERVICE_HVAC_STATUS_JSON,hvacStatus);

        }

        char *out = cJSON_Print(json);

        cJSON_Delete(json);
        //LOG_I(TAG, "Return json %s", out);
        LOG_V(TAG, "END - statusAsJson");

        return out;
    }
    else
    {
        LOG_E(TAG, "Error creating status json obejcts");
        LOG_V(TAG, "END - statusAsJson");

        return NULL;
    }
}

void phev_service_loop(phevServiceCtx_t *ctx)
{
    //LOG_V(TAG, "START - loop");

    phev_pipe_loop(ctx->pipe);

    //LOG_V(TAG, "END - loop");
}

message_t *phev_service_jsonResponseAggregator(void *ctx, messageBundle_t *bundle)
{
    cJSON *out = cJSON_CreateObject();

    cJSON *responses = cJSON_CreateArray();

    cJSON_AddItemToObject(out, "responses", responses);

    for (int i = 0; i < bundle->numMessages; i++)
    {
        cJSON *next = cJSON_Parse((char *)bundle->messages[i]->data);

        cJSON_AddItemToArray(responses, next);
    }

    char *str = cJSON_Print(out);

    cJSON_Delete(out);
    if(str)
    {
        message_t *message = msg_utils_createMsg((uint8_t *)str, strlen(str) );
        return message;
    }
    else
    {
        return NULL;
    }
}

void phev_service_errorHandler(phevError_t *error)
{
}
void phev_service_registrationCompleteCallback(phev_pipe_ctx_t *ctx)
{
    phevServiceCtx_t *serviceCtx = (phevServiceCtx_t *)ctx->ctx;

    if (serviceCtx->registrationCompleteCallback)
    {
        serviceCtx->registrationCompleteCallback(ctx);
    }
}

phevServiceCtx_t *phev_service_resetPipeAfterRegistration(phevServiceCtx_t *ctx)
{
    phev_pipe_ctx_t *pipe = phev_service_createPipe(ctx, ctx->pipe->pipe->in, ctx->pipe->pipe->out);

    ctx->pipe = pipe;

    return ctx;
}
void phev_service_register(const char *mac, phevServiceCtx_t *ctx, phevRegistrationComplete_t complete)
{

    ctx->registrationCompleteCallback = complete;

    return;
}

phevRegister_t *phev_service_getRegister(const phevServiceCtx_t *ctx, const uint8_t reg)
{
    LOG_V(TAG, "START - getRegister");

    phevRegister_t *out = phev_model_getRegister(ctx->model, reg);

    if (out == NULL)
    {
        LOG_D(TAG, "getRegister - register not found");
    }
    LOG_V(TAG, "END - getRegister");
    return out;
}
char *phev_service_getRegisterJson(const phevServiceCtx_t *ctx, const uint8_t reg)
{
    LOG_V(TAG, "START - getRegisterJson");
    cJSON *json = NULL;

    if (ctx)
    {
        phevRegister_t *out = phev_model_getRegister(ctx->model, reg);

        if (out == NULL)
        {
            LOG_I(TAG, "getRegister - register not found");
            return NULL;
        }

        json = cJSON_CreateObject();
        cJSON *regJson = cJSON_CreateNumber((double)reg);
        cJSON *data = cJSON_CreateArray();

        for (int i = 0; i < out->length; i++)
        {
            cJSON *item = cJSON_CreateNumber(out->data[i]);
            cJSON_AddItemToArray(data, item);
        }

        cJSON_AddItemToObject(json, PHEV_SERVICE_REGISTER_JSON, regJson);
        cJSON_AddItemToObject(json, PHEV_SERVICE_REGISTER_DATA_JSON, data);

        char *ret = cJSON_PrintUnformatted(json);
        cJSON_Delete(json);
        LOG_V(TAG, "END - getRegisterJson");
        return ret;
    }
    else
    {
        LOG_E(TAG, "Phev service context not set");
        return NULL;
    }
}
void phev_service_setRegister(const phevServiceCtx_t *ctx, const uint8_t reg, const uint8_t *data, const size_t length)
{
    LOG_V(TAG, "START - setRegister");
    if (phev_model_setRegister(ctx->model, reg, data, length) != 1)
    {
        LOG_E(TAG, "Failed to set register %d", reg);
    }
    LOG_V(TAG, "END - setRegister");
    return;
}
char * phev_service_getDateSync(const phevServiceCtx_t * ctx)
{
    LOG_V(TAG,"START - getDateSync");
    phevRegister_t * reg = phev_model_getRegister(ctx->model,KO_WF_DATE_INFO_SYNC_EVR);
    if(reg)
    {
        char * date;
        asprintf(&date,"20%02d-%02d-%02dT%02d:%02d:%02dZ",reg->data[0],reg->data[1],reg->data[2],reg->data[3],reg->data[4],reg->data[5]);
        return date;
    }
    return NULL;
}
bool phev_service_getChargingStatus(const phevServiceCtx_t * ctx)
{
    LOG_V(TAG,"START - getChargingStatus");
    phevRegister_t * reg = phev_model_getRegister(ctx->model,KO_WF_OBCHG_OK_ON_INFO_REP_EVR);
    if(reg)
    {
        LOG_V(TAG,"END- getChargingStatus");

        return reg->data[0] == 1;
    }
    LOG_V(TAG,"END - getChargingStatus");

    return false;
}
int phev_service_getRemainingChargeTime(const phevServiceCtx_t * ctx)
{
    LOG_V(TAG,"START - getRemainingChargingTime");
    phevRegister_t * reg = phev_model_getRegister(ctx->model, KO_WF_OBCHG_OK_ON_INFO_REP_EVR);
    if(reg && reg->data[2] != 255)
    {
        uint8_t high = reg->data[1];
        uint8_t low = reg->data[2];

        return ((low < 0 ? low + 0x100 : low) * 0x100) + (high < 0 ? high + 0x100 : high);
    }

    return 0;
}

phevServiceHVAC_t * phev_service_getHVACStatus(const phevServiceCtx_t * ctx)
{
    phevRegister_t * acOperatingReg = phev_model_getRegister(ctx->model, KO_AC_MANUAL_SW_EVR);

    phevRegister_t * acModeReg = phev_model_getRegister(ctx->model, KO_WF_TM_AC_STAT_INFO_REP_EVR);

    if(acOperatingReg || acModeReg)
    {
        phevServiceHVAC_t * hvac = malloc(sizeof(phevServiceHVAC_t));
        if(acOperatingReg)
        {
            hvac->operating = acOperatingReg->data[1] == true;
            hvac->operating = acOperatingReg->data[1] == true;
        } else {
            hvac->operating = false;
        }
        if(acModeReg)
        {
            hvac->mode = acModeReg->data[0];
        } else {
            hvac->mode = 0;
        }
        return hvac;
    }
    return NULL;
}
void phev_service_disconnectInput(phevServiceCtx_t * ctx)
{
    LOG_V(TAG,"START - disconnectInput");

    phev_pipe_disconnectInput(ctx->pipe);

    LOG_V(TAG,"END - disconnectInput");
}
void phev_service_disconnectOutput(phevServiceCtx_t * ctx)
{
    LOG_V(TAG,"START - disconnectOutput");

    phev_pipe_disconnectOutput(ctx->pipe);

    LOG_V(TAG,"END - disconnectOutput");
}
void phev_service_disconnect(phevServiceCtx_t * ctx)
{
    LOG_V(TAG,"START - disconnect");

    phev_service_disconnectInput(ctx);
    phev_service_disconnectOutput(ctx);

    LOG_V(TAG,"END - disconnect");

}
