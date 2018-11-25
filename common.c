#include "common.h"

void connect_callback(struct mosquitto* mosq, void* obj, int result)
{
    syslog(LOG_NOTICE, "%s rc=%d\n", __func__, result);
}

void mqttqos_connect_callback(struct mosquitto* mosq, void* obj, int result)
{
    syslog(LOG_NOTICE, "%s rc=%d\n", __func__, result);
}

void mqttqos_disconnect_callback(struct mosquitto* mosq, void* obj, int result)
{
    syslog(LOG_NOTICE, "%s rc=%d\n", __func__, result);
    if (result)
    {
        syslog(LOG_WARNING, "Reconnect...\n");
        mosquitto_reconnect(mosq);
    }
    // connected = false;
}

void mqttqos_message_callback(struct mosquitto* mosq, void* obj,
                                     const struct mosquitto_message* message)
{
    bool match = 0;
    syslog(LOG_NOTICE, "got message '%.*s' for topic '%s'\n",
           message->payloadlen, (char*)message->payload, message->topic);

    mosquitto_topic_matches_sub("/devices/wb-adc/controls/+", message->topic,
                                &match);
    if (match)
    {
        syslog(LOG_NOTICE, "got message for ADC topic\n");
    }
}

void mqttqos_publish_callback(struct mosquitto* mosq, void* obj, int mid)
{
    syslog(LOG_NOTICE, "%s mid=%d\n", __func__, mid);
    mosquitto_disconnect((struct mosquitto*)obj);
}

void message_callback(struct mosquitto* mosq, void* obj,
                             const struct mosquitto_message* message)
{
    bool match = 0;
    syslog(LOG_NOTICE, "got message '%.*s' for topic '%s'\n",
           message->payloadlen, (char*)message->payload, message->topic);

    mosquitto_topic_matches_sub("/devices/wb-adc/controls/+", message->topic,
                                &match);
    if (match)
    {
        syslog(LOG_NOTICE, "got message for ADC topic\n");
    }
}

void mqttqos_log_callback(struct mosquitto* mosq, void* userdata,
                                 int level, const char* str)
{
    /* Pring all log messages regardless of level. */

    switch (level)
    {
        case MOSQ_LOG_DEBUG:
        case MOSQ_LOG_INFO:
        case MOSQ_LOG_NOTICE:
        case MOSQ_LOG_WARNING:
        case MOSQ_LOG_ERR:
        default:
            // syslog(LOG_NOTICE, "%s level=0x%x:[%s]\n", __func__, level, str);
            break;
    }
}


/*
+mqtt_publish_message Version 1.4.10
+mqtt_publish_message topic=[topic] message=[message]
mqttqos_log_callback level=16:[Client 33_973 sending CONNECT]
publish message=[message] len=7 size=4
mqttqos_log_callback level=16:[Client 33_973 sending PUBLISH (d0, q0, r0, m1,
'topic', ... (7 bytes))]
mqttqos_log_callback level=16:[Client 33_973 sending DISCONNECT]
mqttqos_publish_callback rc=1
mqttqos_log_callback level=16:[Client 33_973 sending DISCONNECT]
mqttqos_disconnect_callback rc=0
-mqtt_publish_message ret=0

                                 */
// mosquitto_sub  -t "#" -v
int mqtt_publish_message(char* topic, char* message)
{
    char msg[256];
    char clientid[255];
    struct mosquitto* mosq = NULL;
    int ret = 0;
    char command[255];
    char hostname[64];
    bool clean_session = true;
    int major, minor, revision;

    mosquitto_lib_version(&major, &minor, &revision);

    syslog(LOG_NOTICE, "+%s Version %d.%d.%d\n", __func__, major, minor,
           revision);

    syslog(LOG_NOTICE, "%s topic=[%s] message=[%s]\n", __func__, topic,
           message);

    gethostname(hostname, sizeof(hostname));

    mosquitto_lib_init();

    memset(clientid, 0, sizeof(clientid));
    snprintf(clientid, sizeof(clientid) - 1, "%s_%d", hostname, getpid());

    mosq = mosquitto_new(clientid, clean_session, 0);

    if (mosq)
    {
        mosquitto_log_callback_set(mosq, mqttqos_log_callback);
        mosquitto_connect_callback_set(mosq, mqttqos_connect_callback);
        mosquitto_disconnect_callback_set(mosq, mqttqos_disconnect_callback);
        mosquitto_message_callback_set(mosq, mqttqos_message_callback);
        mosquitto_publish_callback_set(mosq, mqttqos_publish_callback);

        ret = mosquitto_connect(mosq, MQTT_HOST, MQTT_PORT, 60);
        if (ret)
        {
            syslog(LOG_ERR, "MQTT %s connect error! %d %s\n", MQTT_HOST, ret,
                   strerror(errno));
            mosquitto_destroy(mosq);
            mosquitto_lib_cleanup();
            return -1;
        }

        ret = mosquitto_loop_start(mosq);
        if (ret != MOSQ_ERR_SUCCESS)
        {
            syslog(LOG_ERR, "LOOP fail!\n");
            mosquitto_destroy(mosq);
            mosquitto_lib_cleanup();
            return -1;
        }

        syslog(LOG_NOTICE, "publish message=[%s] len=%lu size=%lu\n", message,
               strlen(message), sizeof(message));

        ret = mosquitto_publish(mosq, NULL, topic, strlen(message), message, 0,
                                0);

        // mosquitto_disconnect(mosq);
        mosquitto_loop_stop(mosq, false);
        mosquitto_destroy(mosq);
    }
    else
    {
        syslog(LOG_ERR, "mosq new fail! %s\n", strerror(errno));
        ret = -1;
    }

    mosquitto_lib_cleanup();

    syslog(LOG_NOTICE, "-%s ret=%d\n", __func__, ret);

    return -ret;
}

