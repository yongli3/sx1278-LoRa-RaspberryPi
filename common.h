#ifndef COMMON_H
#define COMMON_H

#include <stdio.h>
#include <sys/types.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <mosquitto.h>
#include <syslog.h>

#define DB_NAME "/var/rawdata.db"

#define IP_REPORT_INTERNAL 240
#define MQTT_HOST "202.120.26.119"
#define MQTT_PORT 1883

void connect_callback(struct mosquitto* mosq, void* obj, int result);

void mqttqos_connect_callback(struct mosquitto* mosq, void* obj, int result);
void mqttqos_disconnect_callback(struct mosquitto* mosq, void* obj, int result);

void mqttqos_message_callback(struct mosquitto* mosq, void* obj,
                                     const struct mosquitto_message* message);

void mqttqos_publish_callback(struct mosquitto* mosq, void* obj, int mid);

void message_callback(struct mosquitto* mosq, void* obj,
                             const struct mosquitto_message* message);

void mqttqos_log_callback(struct mosquitto* mosq, void* userdata,
                                 int level, const char* str);

int mqtt_publish_message(char* topic, char* message);

#endif
