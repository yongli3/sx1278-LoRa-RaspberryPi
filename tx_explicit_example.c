#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <mosquitto.h>
#include <net/if.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sqlite3.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/sysinfo.h>
#include <sys/time.h>
#include <sys/types.h>
#include <syslog.h>
#include <termios.h>
#include <unistd.h>
#include "LoRa.h"

#define WWW_INTERNAL_SECONDS 120
#define MQTT_HOST "202.120.26.119"
#define MQTT_PORT 1883

#define DB_NAME "/var/rawdata.db"

#define TYPE_BOARDCAST 0x3a
#define TYPE_REPORT 0x3b
#define TYPE_ACK 0x3c

#define MOBILE_REPORT_EVENT_1 1
#define MOBILE_REPORT_EVENT_2 2
#define MOBILE_REPORT_EVENT_3 3
#define MOBILE_REPORT_EVENT_4 4

typedef struct _AP_BROADCAST_EMPTYPACKAGE
{
    uint8_t startMark; //起始符号
    uint16_t length;   // fix 0x0a
    uint16_t AP_Address;
    uint32_t time_Count; // 1970年1月1日0点到现在的秒数
    uint8_t LRC;         //类型
    uint8_t reserved;    //固定00
} __attribute__((packed)) AP_BROADCAST_EMPTYPACKAGE;

typedef struct _UM_REPORT_PACKAGE
{
    uint8_t package_StartMark; //起始符号 0x3b = TYPE_REPORT
    uint16_t length;
    uint16_t SendUnitAddress;
    uint16_t SendUnit_SeqCounter; //发出单元序号
    uint8_t Event_Type;
    uint32_t Event_timeStamp;
    uint16_t TrainID;     //列车号
    uint8_t TrainNumber1; // 车次号 number1 << 16| number2 << 8 | number3
    uint8_t TrainNumber2;
    uint8_t TrainNumber3;
    uint8_t UserName[9];  //="user1234";
    uint8_t UserIdNum[9]; //="12345678";
    uint16_t AP_ID;       // station_id
    uint8_t BatVoltage;
    uint8_t reserved;
} __attribute__((packed)) UM_REPORT_PACKAGE;

typedef struct _AP_ACK_PACKAGE
{
    uint8_t ACKstartMark; //起始符号
    uint16_t length;      // fix 0x0a
    uint16_t AP_Address;  //发出单元号
    uint16_t SendUnitAddress;
    uint16_t SendUnit_SeqCounter;
    uint16_t crc;
} __attribute__((packed)) AP_ACK_PACKAGE;

static char txbuf[LORA_TX_LEN];
static char rxbuf[LORA_RX_LEN];

static AP_BROADCAST_EMPTYPACKAGE boardcast_packet;
static UM_REPORT_PACKAGE report_packet;
static AP_ACK_PACKAGE ack_packet;

static bool lora_tx_done = false;
static bool lora_rx_done = false;
static bool connected = true;
static sqlite3* local_db = NULL;
static char* db_create_string =
    "CREATE TABLE `rawdata` (`ID`    INTEGER NOT "
    "NULL PRIMARY KEY AUTOINCREMENT UNIQUE, `TIME`  "
    "INTEGER, `TOPIC` TEXT, `MESSAGE`   "
    "TEXT,`LOCAL` INTEGER)";
static void rx_f(rxData* rx)
{

    int i = 0;

    if (rx->CRC)
    {
        // crc error, discard
        rx->size = 0;
        syslog(LOG_DEBUG, ">>RXCRCERR\n");
    }
    else
    {
        syslog(LOG_DEBUG, ">>RXdone %llu CRC=%d size=%d RSSI=%d SNR=%f\n",
               current_timestamp(), rx->CRC, rx->size, rx->RSSI, rx->SNR);
    }
#if 0
	for (i = 0; i < rx->size; i++) {
	    printf("%x\n", rx->buf[i]);
	}
	printf("%s\n", rx->buf);
#endif
    lora_rx_done = true;
}

static void tx_f(txData* tx)
{
    // printf("<<TXdone %llu\n", current_timestamp());
    lora_tx_done = true;
}

static void connect_callback(struct mosquitto* mosq, void* obj, int result)
{
    syslog(LOG_NOTICE, "%s rc=%d\n", __func__, result);
}

static void mqttqos_connect_callback(struct mosquitto* mosq, void* obj,
                                     int result)
{
    syslog(LOG_NOTICE, "%s rc=%d\n", __func__, result);
}

static void mqttqos_disconnect_callback(struct mosquitto* mosq, void* obj,
                                        int result)
{
    syslog(LOG_NOTICE, "%s rc=%d\n", __func__, result);
    if (result)
    {
        syslog(LOG_WARNING, "Reconnect...\n");
        mosquitto_reconnect(mosq);
    }
    // connected = false;
}

static void mqttqos_message_callback(struct mosquitto* mosq, void* obj,
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

static void mqttqos_publish_callback(struct mosquitto* mosq, void* obj, int mid)
{
    syslog(LOG_NOTICE, "%s mid=%d\n", __func__, mid);
    mosquitto_disconnect((struct mosquitto*)obj);
}

static void message_callback(struct mosquitto* mosq, void* obj,
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

static void mqttqos_log_callback(struct mosquitto* mosq, void* userdata,
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
static int mqtt_publish_message(char* topic, char* message)
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

static int callbackCreate(void* NotUsed, int argc, char** argv, char** colName)
{
    syslog(LOG_NOTICE, "%s\n", __func__);
    return 0;
}

static int callbackInsert(void* NotUsed, int argc, char** argv, char** colName)
{
    syslog(LOG_NOTICE, "%s argc=%d\n", __func__, argc);
    return 0;
}

static int callbackUpdate(void* NotUsed, int argc, char** argv, char** colName)
{
    int i = 0;

    syslog(LOG_NOTICE, "+%s argc=%d\n", __func__, argc);
    for (i = 0; i < argc; i++)
    {
        syslog(LOG_NOTICE, "%s=%s\n", colName[i], argv[i]);
    }
    return 0;
}

// Select the older local =1, publish and update to local = 0
static int callbackSelect(void* NotUsed, int argc, char** argv, char** colName)
{
    char sqlString[256];
    char* errMsg = NULL;
    int ret = -1;
    int i = 0;
    char buf[256];
    // argc= column number argv=column value
    syslog(LOG_NOTICE, "+%s argc=%d\n", __func__, argc);
    for (i = 0; i < argc; i++)
    {
        syslog(LOG_NOTICE, "%s=%s\n", colName[i], argv[i]);
    }

    sprintf(buf, "%s-%s", argv[1], argv[3]);
    ret = mqtt_publish_message(argv[2], buf);
    if (ret)
    {
        syslog(LOG_ERR, "%s publish fail!\n", __func__);
        return 0;
    }

    // Publish okay send this record to MQTT server, and set local = 0
    sqlite3_mutex_enter(sqlite3_db_mutex(local_db));
    sprintf(sqlString, "UPDATE `rawdata` SET `LOCAL`=0 WHERE `%s`=%s",
            colName[0], argv[0]);
    syslog(LOG_NOTICE, "%s execute [%s]", __func__, sqlString);

    ret = sqlite3_exec(local_db, sqlString, callbackUpdate, NULL, &errMsg);
    if (ret != SQLITE_OK)
    {
        syslog(LOG_NOTICE, "Can't exec %s: %s\n", sqlString,
               sqlite3_errmsg(local_db));
    }
    else
    {
        syslog(LOG_NOTICE, "[%s] okay!\n", sqlString);
    }
    sqlite3_mutex_leave(sqlite3_db_mutex(local_db));

#if 0
    for (i = 0; i < argc; i++) {
        printf("%d:%s-%s\n", i, argv[i], colName[i]);
    }
#endif
    syslog(LOG_NOTICE, "-%s argc=%d\n", __func__, argc);
    return 0;
}

/*
 CREATE TABLE `rawdata` (`ID`
 INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT UNIQUE,
 `TIME`  INTEGER, `TOPIC` TEXT, `MESSAGE`   TEXT,`LOCAL` INTEGER)
 */
void* threadInsert(void* ptr)
{
    char sqlString[256];
    char* errMsg = NULL;
    int ret = -1;
    int type = *(int*)ptr;
    int i = 0;

    syslog(LOG_NOTICE, "+%s %d\n", __FUNCTION__, type);

    while (i++ < 10)
    {
        sqlite3_mutex_enter(sqlite3_db_mutex(local_db));
        sprintf(sqlString,
                "INSERT INTO `rawdata`(`TIME`, `TOPIC`, `MESSAGE`, `LOCAL`) "
                "VALUES (%llu,'%s', '%s', 1)",
                current_timestamp(), "topic", "message");
        ret = sqlite3_exec(local_db, sqlString, callbackInsert, NULL, &errMsg);
        if (ret != SQLITE_OK)
        {
            syslog(LOG_NOTICE, "Can't exec %s: %s\n", sqlString,
                   sqlite3_errmsg(local_db));
        }
        else
        {
            syslog(LOG_NOTICE, "[%s] okay!\n", sqlString);
        }

        sqlite3_mutex_leave(sqlite3_db_mutex(local_db));
        usleep(1000 * 500);
    }
    return ptr;
}

/// select the oldest local=1, publish to MQTT server, and change local to 0
void* threadUpdate(void* ptr)
{
    char sqlString[256];
    int ret = -1;
    int i = 0;
    char* errMsg = NULL;
    int type = *(int*)ptr;

    syslog(LOG_NOTICE, "+%s %d\n", __FUNCTION__, type);
    while (1)
    {
        sqlite3_mutex_enter(sqlite3_db_mutex(local_db));
        // query local=1; send out and set local=0
        sprintf(sqlString,
                "SELECT * from rawdata where local=1 order by id limit 1");
        ret = sqlite3_exec(local_db, sqlString, callbackSelect, NULL, &errMsg);
        if (ret != SQLITE_OK)
        {
            syslog(LOG_ERR, "Can't exec %s: %s\n", sqlString,
                   sqlite3_errmsg(local_db));
        }
        else
        {
            syslog(LOG_NOTICE, "[%s] okay!\n", sqlString);
            // delete old items
            sprintf(sqlString, "DELETE FROM `rawdata`"
                               "WHERE LOCAL = 0 AND TIME < %llu",
                    current_timestamp() - 3600 * 24 * 7 * 1000);
            ret = sqlite3_exec(local_db, sqlString, NULL, NULL, &errMsg);
            syslog(LOG_NOTICE, "exec [%s] %d\n", sqlString, ret);
        }

        sqlite3_mutex_leave(sqlite3_db_mutex(local_db));
        usleep(1000 * 1000);
    }
    syslog(LOG_NOTICE, "-%s %d\n", __FUNCTION__, type);
    return ptr;
}

static int set_interface_attribs(int fd, int speed, int parity)
{
    struct termios tty;
    memset(&tty, 0, sizeof tty);
    if (tcgetattr(fd, &tty) != 0)
    {
        printf("error %d from tcgetattr", errno);
        return -1;
    }

    cfsetospeed(&tty, speed);
    cfsetispeed(&tty, speed);

    tty.c_cflag = (tty.c_cflag & ~CSIZE) | CS8; // 8-bit chars
    // disable IGNBRK for mismatched speed tests; otherwise receive break
    // as \000 chars
    tty.c_iflag &= ~IGNBRK; // disable break processing
    tty.c_lflag = 0;        // no signaling chars, no echo,
                            // no canonical processing
    tty.c_oflag = 0;        // no remapping, no delays
    tty.c_cc[VMIN] = 0;     // read doesn't block
    tty.c_cc[VTIME] = 5;    // 0.5 seconds read timeout

    tty.c_iflag &= ~(IXON | IXOFF | IXANY); // shut off xon/xoff ctrl

    tty.c_cflag |= (CLOCAL | CREAD);   // ignore modem controls,
                                       // enable reading
    tty.c_cflag &= ~(PARENB | PARODD); // shut off parity
    tty.c_cflag |= parity;
    tty.c_cflag &= ~CSTOPB;
    tty.c_cflag &= ~CRTSCTS;

    if (tcsetattr(fd, TCSANOW, &tty) != 0)
    {
        printf("error %d from tcsetattr", errno);
        return -1;
    }
    return 0;
}

static void set_blocking(int fd, int should_block)
{
    struct termios tty;
    memset(&tty, 0, sizeof tty);
    if (tcgetattr(fd, &tty) != 0)
    {
        printf("error %d from tggetattr", errno);
        return;
    }

    tty.c_cc[VMIN] = should_block ? 1 : 0;
    tty.c_cc[VTIME] = 10; // 0.5 seconds read timeout

    if (tcsetattr(fd, TCSANOW, &tty) != 0)
        printf("error %d setting term attributes", errno);
}

static int get_ipaddress(char* ipaddress, int size)
{

    int fd = 1;
    struct ifreq ifr;

    char iface[] = "eth1";

    fd = socket(AF_INET, SOCK_DGRAM, 0);

    // Type of address to retrieve - IPv4 IP address
    ifr.ifr_addr.sa_family = AF_INET;

    // Copy the interface name in the ifreq structure
    strncpy(ifr.ifr_name, iface, IFNAMSIZ - 1);

    ioctl(fd, SIOCGIFADDR, &ifr);

    close(fd);

    // display result
    strcpy(ipaddress,
           inet_ntoa(((struct sockaddr_in*)&ifr.ifr_addr)->sin_addr));
    printf("%s - %s\n", iface, ipaddress);

    return 0;
}

int time_test()
{
    time_t rawtime;
    struct tm* timeinfo;

    time(&rawtime);

    printf("%s\n", ctime(&rawtime));

    timeinfo = localtime(&rawtime);

    printf("%04d-%02d-%02d %02d:%02d:%02d\n", timeinfo->tm_year + 1900,
           timeinfo->tm_mon + 1, timeinfo->tm_mday, timeinfo->tm_hour,
           timeinfo->tm_min, timeinfo->tm_sec);

    printf("Current local time and date: %s", asctime(timeinfo));

    return 0;
}

#if 0
static int uart_test()
{
    struct termios SerialPortSettings;
    int fd = -1;
    int ret = -1;
    int size = 0;
    int i = 0;
    int repeat_count = 0;
    char write_buffer[256];
    char read_buffer[256];

    fd = open("/dev/ttyUSB0", O_RDWR| O_NOCTTY|O_SYNC);

    if (fd == -1) {
        printf("uart open error! %s\n", strerror(errno));
        return -1;
    }

    ret = tcgetattr(fd, &SerialPortSettings);
    if (ret) {
        printf("%s\n", strerror(errno));
        goto cleanup;
    }

    cfsetispeed(&SerialPortSettings, B9600);
    cfsetospeed(&SerialPortSettings, B9600);

#if 1
    SerialPortSettings.c_cflag &= ~PARENB;   /* Disables the Parity Enable bit(PARENB),So No Parity   */
		SerialPortSettings.c_cflag &= ~CSTOPB;   /* CSTOPB = 2 Stop bits,here it is cleared so 1 Stop bit */
		SerialPortSettings.c_cflag &= ~CSIZE;	 /* Clears the mask for setting the data size             */
		SerialPortSettings.c_cflag |=  CS8;      /* Set the data bits = 8                                 */
	
		SerialPortSettings.c_cflag &= ~CRTSCTS;       /* No Hardware flow Control                         */
		SerialPortSettings.c_cflag |= CREAD | CLOCAL; /* Enable receiver,Ignore Modem Control lines       */ 
		
		
		SerialPortSettings.c_iflag &= ~(IXON | IXOFF | IXANY);          /* Disable XON/XOFF flow control both i/p and o/p */
		SerialPortSettings.c_iflag &= ~(ICANON | ECHO | ECHOE | ISIG);  /* Non Cannonical mode                            */

		SerialPortSettings.c_oflag &= ~OPOST;/*No Output Processing*/
#endif
		/* Setting Time outs */
		SerialPortSettings.c_cc[VMIN] = 1; /* Read at least 10 characters */
		SerialPortSettings.c_cc[VTIME] = 0; /* Wait indefinetly   */

		if((tcsetattr(fd,TCSANOW,&SerialPortSettings)) != 0) /* Set the attributes to the termios structure*/ {
		    printf("\n  ERROR ! in Setting attributes");
            goto cleanup;
        }

#if 0
    memset(read_buffer, 0, sizeof(read_buffer));
    size = read(fd, read_buffer, sizeof(read_buffer));
    printf("read %d [%s]\n", size, read_buffer);
            for (i = 0; i < size; i++)
                printf("%x\n", read_buffer[i]);
    tcflush(fd, TCIOFLUSH);
    // cleanup modem
#endif    
    repeat_count = 0;
    while (1) {
#if 0
        while (1) 
       {
        memset(write_buffer, 0, sizeof(write_buffer));
        sprintf(write_buffer, "%s", "AT\r\n");
        size = write(fd,write_buffer,strlen(write_buffer));
        sleep(1);
        tcflush(fd, TCOFLUSH);
        printf("write %s %d\n", write_buffer, size);
        //usleep (size * 1000);
        sleep(1);
        memset(read_buffer, 0, sizeof(read_buffer));
        size = read(fd, read_buffer, sizeof(read_buffer));
        printf("read %d [%s]\n", size, read_buffer);

        }
#endif        
        memset(write_buffer, 0, sizeof(write_buffer));
        sprintf(write_buffer, "%s", "ATE0\r\n");
        size = write(fd,write_buffer,strlen(write_buffer));
        tcflush(fd, TCIOFLUSH);
        printf("write %s %d\n", write_buffer, size);
        //usleep (size * 1000);
        sleep(1);
        memset(read_buffer, 0, sizeof(read_buffer));
        size = read(fd, read_buffer, sizeof(read_buffer));
        printf("read %d [%s]\n", size, read_buffer);
        for (i = 0; i < size; i++)
            printf("%x\n", read_buffer[i]);
       
        if (strstr(read_buffer, "OK")) {
            printf("Modem OK\n");
            break;
        } else {
            if (repeat_count > 5) {
                printf("MODEM ATE0 fail!\n");
                goto cleanup;
            }
        };
        repeat_count++;
        sleep(1);
    }

    memset(write_buffer, 0, sizeof(write_buffer));
    sprintf(write_buffer, "%s", "AT+CFUN=0\r\n");
    size = write(fd,write_buffer,strlen(write_buffer));
    tcflush(fd, TCOFLUSH);
    printf("write %s %d\n", write_buffer, size);
    sleep(6);
    memset(read_buffer, 0, sizeof(read_buffer));
    size = read(fd, read_buffer, sizeof(read_buffer));
    printf("read %d [%s]\n", size, read_buffer);    
    if (NULL == strstr(read_buffer, "OK")) {
        printf("command error !\n");
        goto cleanup;
    };

    memset(write_buffer, 0, sizeof(write_buffer));
    sprintf(write_buffer, "%s", "AT+CFUN=1\r\n");
    size = write(fd,write_buffer,strlen(write_buffer));
    tcflush(fd, TCOFLUSH);
    printf("write %s %d\n", write_buffer, size);
    sleep(6);
    memset(read_buffer, 0, sizeof(read_buffer));
    size = read(fd, read_buffer, sizeof(read_buffer));
    printf("read %d [%s]\n", size, read_buffer);    
    if (NULL == strstr(read_buffer, "OK")) {
        printf("command error !\n");
        goto cleanup;
    };
#if 0
    memset(write_buffer, 0, sizeof(write_buffer));
    sprintf(write_buffer, "%s", "AT+CGDCONT=1,\"IP\"\r\n");
    size = write(fd,write_buffer,strlen(write_buffer));
    printf("write %d\n", size);
    sleep(5);
    memset(read_buffer, 0, sizeof(read_buffer));
    size = read(fd, read_buffer, sizeof(read_buffer));
    printf("read %d [%s]\n", size, read_buffer);    
    if (NULL == strstr(read_buffer, "OK")) {
        printf("command error !\n");
        goto cleanup;
    };
#endif

    memset(write_buffer, 0, sizeof(write_buffer));
    sprintf(write_buffer, "%s", "AT+CGACT=1,1\r\n");
    tcflush(fd, TCOFLUSH);
    size = write(fd,write_buffer,strlen(write_buffer));
    printf("write %s %d\n", write_buffer, size);
    sleep(5);
    memset(read_buffer, 0, sizeof(read_buffer));
    size = read(fd, read_buffer, sizeof(read_buffer));
    printf("read %d [%s]\n", size, read_buffer);    
    if (NULL == strstr(read_buffer, "OK")) {
        printf("command error !\n");
        goto cleanup;
    };

    memset(write_buffer, 0, sizeof(write_buffer));
    sprintf(write_buffer, "%s", "AT+ZGACT=1,1\r\n");
    size = write(fd,write_buffer,strlen(write_buffer));
    tcflush(fd, TCOFLUSH);
    printf("write %s %d\n", write_buffer, size);
    sleep(5);
    memset(read_buffer, 0, sizeof(read_buffer));
    size = read(fd, read_buffer, sizeof(read_buffer));
    printf("read %d [%s]\n", size, read_buffer);    

    // push the buffer to mqtt

// +ZGIPDNS: 1,1,"IP","10.101.206.237","0.0.0.0","116.116.116.116","221.5.88.88"

    if (NULL == strstr(read_buffer, "OK")) {
        printf("command error !\n");
        goto cleanup;
    };

        ret = system("ifconfig eth0 down");
    printf("system return %d\n", ret);
    
    ret = system("udhcpc -i eth1");
    printf("system return %d\n", ret);

    

cleanup:
    close(fd);
    return ret;
}

#endif

// a thread to query local DB and publish to MQTT server
static int thread_db()
{
    int i = 0;
    int ret = 0;
    pthread_t thread1, thread2;
    int thr = 1;
    int thr2 = 2;
    char* errMsg = NULL;
    sqlite3_mutex* mutex;
    char sqlString[256];

    syslog(LOG_NOTICE, "+%s\n", __FUNCTION__);

    i = 0;

    ret = sqlite3_open_v2(DB_NAME, &local_db, SQLITE_OPEN_READWRITE, NULL);
    if (ret != SQLITE_OK)
    {
        syslog(LOG_ERR, "Can't open database: %s\n", sqlite3_errmsg(local_db));
        sqlite3_close(local_db);
        // create the DB
        ret = sqlite3_open_v2(DB_NAME, &local_db,
                              SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL);
        if (ret != SQLITE_OK)
        {
            syslog(LOG_ERR, "Can't create database: %s\n",
                   sqlite3_errmsg(local_db));
            return -1;
        }
        sqlite3_mutex_enter(sqlite3_db_mutex(local_db));
        ret = sqlite3_exec(local_db, db_create_string, callbackCreate, NULL,
                           &errMsg);
        if (ret != SQLITE_OK)
        {
            syslog(LOG_ERR, "Can't exec %s: %s %s\n", db_create_string,
                   sqlite3_errmsg(local_db), errMsg);
            sqlite3_free(errMsg);
        }
        else
        {
            syslog(LOG_NOTICE, "[%s] okay!\n", db_create_string);
        }
        sqlite3_mutex_leave(sqlite3_db_mutex(local_db));
    }
    else
    {
        syslog(LOG_NOTICE, "Open %s successfully\n", DB_NAME);
    }

#if 1
    // start the threads for sqlite3 test
    // pthread_create(&thread1, NULL, *threadInsert, (void*)&thr);
    pthread_create(&thread2, NULL, *threadUpdate, (void*)&thr2);
    // wait for threads to finish
    // pthread_join(thread1, NULL);
    pthread_join(thread2, NULL);
#endif

    sqlite3_close(local_db);
    return 0;
}

static uint32_t g_t = 0;

static void* threadF1(void* ptr)
{
    syslog(LOG_NOTICE, "+%s\n", __FUNCTION__);
    while (g_t < 50)
    {
        usleep(1000 * 100);
        syslog(LOG_NOTICE, "%s %d\n", __FUNCTION__, g_t);
        g_t++;
    }
    return NULL;
}

static void* threadF2(void* ptr)
{
    syslog(LOG_NOTICE, "+%s\n", __FUNCTION__);

    while (g_t < 50)
    {
        usleep(1000 * 90);
        syslog(LOG_NOTICE, "%s %d\n", __FUNCTION__, g_t);
        g_t++;
    }

    syslog(LOG_NOTICE, "-%s\n", __FUNCTION__);

    return NULL;
}

static int thread_test()
{
    syslog(LOG_NOTICE, "+%s\n", __FUNCTION__);

    pthread_t thread1, thread2;
    int thr = 1;
    int thr2 = 2;

    pthread_create(&thread1, NULL, *threadF1, (void*)&thr);
    pthread_create(&thread2, NULL, *threadF2, (void*)&thr2);
    // wait for threads to finish
    pthread_join(thread1, NULL);
    pthread_join(thread2, NULL);

    syslog(LOG_NOTICE, "-%s g_t=%u\n", __FUNCTION__, g_t);
    return 0;
}

int main()
{
    char sqlString[256];
    char mqtt_message[512];
    char mqtt_topic[256];
    char* errMsg = NULL;
    // unsigned char send_len = 0;
    unsigned int send_seq = 0;
    int i = 0;
    int ret = 0;
    char hostname[64];
    char current_date[64];
    time_t rawtime;
    struct tm* timeinfo;
    LoRa_ctl modem;

    openlog("LORA", LOG_CONS | LOG_PID | LOG_NDELAY, LOG_LOCAL1);

    syslog(LOG_INFO, "INFO Built on %s %s", __DATE__, __TIME__);
    syslog(LOG_DEBUG, "DBG: Built on %s %s", __DATE__, __TIME__);
    syslog(LOG_ERR, "ERR: Built on %s %s", __DATE__, __TIME__);

    gethostname(hostname, sizeof(hostname));
    srand((unsigned)time(NULL));

    thread_db();

    // printf("%d-%s\n", strtol(hostname, NULL, 10), hostname);

    // printf("%d-%d-%d-%d\n", sizeof(uint8_t), sizeof(uint16_t),
    // sizeof(uint32_t), sizeof(unsigned long));
    // return 0;

    syslog(LOG_DEBUG, "Boardcat packet=%lu report packet=%lu\n",
           sizeof(boardcast_packet), sizeof(report_packet));
    memset(&boardcast_packet, 0, sizeof(boardcast_packet));
    memset(&report_packet, 0, sizeof(report_packet));

    boardcast_packet.AP_Address = strtol(hostname, NULL, 10);
    boardcast_packet.LRC = 0;
    boardcast_packet.startMark = TYPE_BOARDCAST;
    boardcast_packet.time_Count = time(NULL);
    boardcast_packet.length = sizeof(boardcast_packet) - 1;
    boardcast_packet.reserved = 0;

    gethostname(hostname, sizeof(hostname));

    // See for typedefs, enumerations and there values in LoRa.h header file
    modem.spiCS = 0; // Raspberry SPI CE pin number
    modem.tx.callback = tx_f;
    modem.tx.data.buf = (char*)&boardcast_packet;

    modem.rx.callback = rx_f;
    modem.rx.data.buf = rxbuf;

    // send_len = sizeof(boardcast_packet);

    // sprintf(txbuf, "LoraLongTest%u", (unsigned)time(NULL));

    // printf("%s %d\n", modem.tx.data.buf, strlen(modem.tx.data.buf));
    // memcpy(modem.tx.data.buf, "LoRa", 5);//copy data we'll sent to buffer

    modem.tx.data.size = sizeof(boardcast_packet) + 1; // Payload len
    modem.eth.preambleLen = 6;
    // data speed for 64 bytes
    // BW500+SF7=87ms  DIO high=21.7ms 106ms
    // BW500+SF12 = 900ms

    modem.eth.bw = BW500;       // Bandwidth 62.5KHz
    modem.eth.sf = SF8;         // Spreading Factor 12
    modem.eth.ecr = CR8;        // Error coding rate CR4/8
    modem.eth.CRC = 1;          // Turn on CRC checking
    modem.eth.freq = 434800000; // 434.8MHz
    modem.eth.resetGpioN = 23;  // GPIO4 on lora RESET pin
    modem.eth.dio0GpioN =
        24; // GPIO17 on lora DIO0 pin to control Rxdone and Txdone interrupts
    modem.eth.outPower = OP20;        // Output power
    modem.eth.powerOutPin = PA_BOOST; // Power Amplifire pin
    modem.eth.AGC = 1;                // Auto Gain Control
    modem.eth.OCP = 240;              // 45 to 240 mA. 0 to turn off protection
    modem.eth.implicitHeader = 0;     // Explicit header mode
    modem.eth.syncWord = 0x12;
    // For detail information about SF, Error Coding Rate, Explicit header,
    // Bandwidth, AGC, Over current protection and other features refer to
    // sx127x datasheet
    // https://www.semtech.com/uploads/documents/DS_SX1276-7-8-9_W_APP_V5.pdf
    send_seq = 0;
    ret = LoRa_begin(&modem);
    if (ret < 0)
    {
        syslog(LOG_ERR, "Lora begin error! %d", ret);
        return -1;
    }

    while (1)
    {
        // memset(txbuf, 'a', sizeof(txbuf));
        // sprintf(txbuf, "send %d", send_seq);

        // time (&rawtime);
        // timeinfo = localtime(&rawtime);
        // snprintf(txbuf, sizeof(txbuf), "BOARDCAST %u %s TX %04d-%02d-%02d
        // %02d:%02d:%02d", send_seq,
        //	hostname, timeinfo->tm_year + 1900, timeinfo->tm_mon + 1,
        // timeinfo->tm_mday,
        //  timeinfo->tm_hour, timeinfo->tm_min, timeinfo->tm_sec);

        memset(&boardcast_packet, 0, sizeof(boardcast_packet));
        boardcast_packet.AP_Address = strtol(hostname, NULL, 10);
        boardcast_packet.LRC = 0;
        boardcast_packet.startMark = TYPE_BOARDCAST;
        boardcast_packet.time_Count = time(NULL);
        boardcast_packet.length = sizeof(boardcast_packet) - 1;
        boardcast_packet.reserved = 0;

        modem.tx.data.buf = (char*)&boardcast_packet;
        modem.tx.data.size = sizeof(boardcast_packet) + 1; // Payload len

// sprintf(txbuf, "LoraLongTest%u", (unsigned)time(NULL));

// printf("%s %d\n", modem.tx.data.buf, strlen(modem.tx.data.buf));

#if 1
        // send out boardcast packet [BOARDCAST 103 test5 TX 2018-07-30
        // 21:44:56]
        lora_tx_done = false;
        LoRa_send(&modem);
#if 1
        syslog(LOG_DEBUG, "TX size=%d", modem.tx.data.size);
        for (i = 0; i < modem.tx.data.size; i++)
        {
            syslog(LOG_DEBUG, "%02x ", modem.tx.data.buf[i]);
        }
// printf("\n");
#endif
        syslog(LOG_DEBUG, "<<%llu %u CAST Tsym=%f Tpkt=%f payloadSymbNb=%u\n",
               current_timestamp(), boardcast_packet.time_Count,
               modem.tx.data.Tsym, modem.tx.data.Tpkt,
               modem.tx.data.payloadSymbNb);

        // printf("<<sleep %u ms to transmitt complete %llu\n", (unsigned
        // long)modem.tx.data.Tpkt, current_timestamp());
        usleep((unsigned long)(modem.tx.data.Tpkt * 1000));

        while (!lora_tx_done)
        {
            syslog(LOG_DEBUG, "<<wait for tx_done..\n");
            usleep(1000 * 40);
        }

// usleep(1000*500);
// sleep(1);
// continue;
#endif

#if 1
        // tx done, start to receive
        syslog(LOG_DEBUG, "wait for report packet ...\n");
        // usleep(LORA_WAIT_FOR_RECEIVE_MS*100);
        memset(rxbuf, 0, sizeof(rxbuf));
        lora_rx_done = false;
        LoRa_receive(&modem);

        // sleep(100);
        i = 0;
        while (!lora_rx_done)
        {
            // printf("wait rpt %d\n", i);
            if (i++ > LORA_WAIT_FOR_RECEIVE_COUNT)
            {
                // printf("no response, boardcast again %d %u\n", i,
                // time(NULL));
                break;
            }
            usleep(LORA_WAIT_FOR_RECEIVE_MS * 1000); // 150ms * 20
        }

        if (lora_rx_done)
        {
            // get data, send out ACK
            if (modem.rx.data.CRC)
            {
                syslog(LOG_DEBUG, ">>rx crc error\n");
            }
            else
            {
// printf(">>rxbuf=[%s]-%d %llu\n", rxbuf, strlen(rxbuf), current_timestamp());
// data crc okay, check packet type
#if 0
			for (i = 0; i < modem.rx.data.size; i++) {
				printf("%d:%x\n", i, modem.rx.data.buf[i]);
			}
#endif
                memset(&report_packet, 0, sizeof(report_packet));
                memcpy(&report_packet, rxbuf, sizeof(report_packet));

                if (report_packet.package_StartMark == TYPE_REPORT)
                {
                    // send out ACK
                    syslog(LOG_DEBUG, ">>RPT: %lu ", time(NULL));
                    for (i = 0; i < sizeof(report_packet); i++)
                    {
                        syslog(LOG_DEBUG, "%02x ", rxbuf[i]);
                    }
                    syslog(LOG_DEBUG, "\n");
#if 1
                    syslog(LOG_DEBUG, "startmark=0x%x ",
                           report_packet.package_StartMark);
                    syslog(LOG_DEBUG, "len=%d ", report_packet.length);
                    syslog(LOG_DEBUG, "unitAddr=0x%x ",
                           report_packet.SendUnitAddress);
                    syslog(LOG_DEBUG, "send_seq=0x%x ",
                           report_packet.SendUnit_SeqCounter);
                    syslog(LOG_DEBUG, "evnt_type=0x%x ",
                           report_packet.Event_Type);
                    syslog(LOG_DEBUG, "timestamp=%u ",
                           report_packet.Event_timeStamp);
                    syslog(LOG_DEBUG, "trainID=0x%x ", report_packet.TrainID);
                    syslog(LOG_DEBUG, "%x ", report_packet.TrainNumber1);
                    syslog(LOG_DEBUG, "%x ", report_packet.TrainNumber2);
                    syslog(LOG_DEBUG, "%x ", report_packet.TrainNumber3);
                    syslog(LOG_DEBUG, "Username=[%s] ", report_packet.UserName);
                    syslog(LOG_DEBUG, "UserID=[%s] ", report_packet.UserIdNum);
                    syslog(LOG_DEBUG, "APP_ID=0x%x ", report_packet.AP_ID);
                    syslog(LOG_DEBUG, "BatVol=%d ", report_packet.BatVoltage);
                    syslog(LOG_DEBUG, "resve=0x%x\n", report_packet.reserved);
#endif
                    if (MOBILE_REPORT_EVENT_1 == report_packet.Event_Type)
                    {
#if 1
                        memset(mqtt_topic, 0, sizeof(mqtt_topic));
                        sprintf(mqtt_topic, "%s", "rawdata");
                        memset(mqtt_message, 0, sizeof(mqtt_message));
                        sprintf(mqtt_message, "event_type=%u",
                                report_packet.Event_Type);
                        sprintf(mqtt_message, "%s;src_apstation=%lu",
                                mqtt_message, strtol(hostname, NULL, 10));
                        sprintf(mqtt_message, "%s;seqno_apstation=%u",
                                mqtt_message, send_seq);

                        time(&rawtime);
                        timeinfo = localtime(&rawtime);
                        memset(current_date, 0, sizeof(current_date));
                        sprintf(current_date, "%04d-%02d-%02d-%02d:%02d:%02d",
                                timeinfo->tm_year + 1900, timeinfo->tm_mon + 1,
                                timeinfo->tm_mday, timeinfo->tm_hour,
                                timeinfo->tm_min, timeinfo->tm_sec);
                        sprintf(mqtt_message, "%s;timestamp_apstation=%s",
                                mqtt_message, current_date);

                        sprintf(mqtt_message, "%s;src_mobile=%u", mqtt_message,
                                report_packet.SendUnitAddress);
                        sprintf(mqtt_message, "%s;seqno_mobile=%u",
                                mqtt_message,
                                report_packet.SendUnit_SeqCounter);

                        rawtime = report_packet.Event_timeStamp;
                        timeinfo = localtime(&rawtime);
                        memset(current_date, 0, sizeof(current_date));
                        sprintf(current_date, "%04d-%02d-%02d-%02d:%02d:%02d",
                                timeinfo->tm_year + 1900, timeinfo->tm_mon + 1,
                                timeinfo->tm_mday, timeinfo->tm_hour,
                                timeinfo->tm_min, timeinfo->tm_sec);
                        sprintf(mqtt_message, "%s;timestamp_mobile=%s",
                                mqtt_message, current_date);
                        sprintf(mqtt_message, "%s;train_id=%u", mqtt_message,
                                report_packet.TrainID);
                        sprintf(mqtt_message, "%s;train_seq=%u", mqtt_message,
                                report_packet.TrainNumber1 << 16 |
                                    report_packet.TrainNumber2 << 8 |
                                    report_packet.TrainNumber3);
                        sprintf(mqtt_message, "%s;driver_name=%s", mqtt_message,
                                report_packet.UserName);
                        sprintf(mqtt_message, "%s;driver_id=%lu", mqtt_message,
                                strtol(report_packet.UserIdNum, NULL, 10));
                        sprintf(mqtt_message, "%s;station_id=%u", mqtt_message,
                                report_packet.AP_ID);
                        sprintf(mqtt_message, "%s;batt_vol=%u", mqtt_message,
                                report_packet.BatVoltage);
                        sprintf(mqtt_message, "%s;reserved=%u", mqtt_message,
                                report_packet.reserved);
#endif
                        syslog(LOG_DEBUG, "%llu +MQTT %s-%s \n",
                               current_timestamp(), mqtt_topic, mqtt_message);

                        // mqtt_publish_message(mqtt_topic, mqtt_message);
                        sqlite3_mutex_enter(sqlite3_db_mutex(local_db));
                        sprintf(sqlString, "INSERT INTO `rawdata`(`TIME`, "
                                           "`TOPIC`, `MESSAGE`, `LOCAL`) "
                                           "VALUES (%llu,'%s', '%s', 1)",
                                current_timestamp(), mqtt_topic, mqtt_message);
                        ret = sqlite3_exec(local_db, sqlString, callbackInsert,
                                           NULL, &errMsg);
                        if (ret != SQLITE_OK)
                        {
                            syslog(LOG_NOTICE, "Can't exec %s: %s\n", sqlString,
                                   sqlite3_errmsg(local_db));
                        }
                        else
                        {
                            syslog(LOG_NOTICE, "[%s] okay!\n", sqlString);
                        }

                        sqlite3_mutex_leave(sqlite3_db_mutex(local_db));

                        syslog(LOG_DEBUG, "%llu -MQTT\n", current_timestamp());
                    }
                    else
                    {
                        syslog(LOG_DEBUG, "Unsupport RPT type %d\n",
                               report_packet.Event_Type);
                    }

                    memset(&ack_packet, 0, sizeof(ack_packet));
                    ack_packet.ACKstartMark = TYPE_ACK;
                    ack_packet.length = sizeof(ack_packet) - 1;
                    ack_packet.AP_Address = strtol(hostname, NULL, 10);
                    ack_packet.SendUnitAddress = report_packet.SendUnitAddress;
                    ack_packet.SendUnit_SeqCounter =
                        report_packet.SendUnit_SeqCounter;
                    ack_packet.crc = 0;
                    modem.tx.data.buf = (char*)&ack_packet;
                    // while (1) {
                    // ack_packet.AP_Address++;
                    modem.tx.data.size = sizeof(ack_packet) + 1; // Payload len
#if 1
                    syslog(LOG_DEBUG, "<<ACK:");
                    for (i = 0; i < sizeof(ack_packet); i++)
                    {
                        syslog(LOG_DEBUG, "%d=%02x ", i, txbuf[i]);
                    }
// printf("\n");
#endif
                    // send ACK, based on test, needs to delay 1.8 seconds for
                    // MCU to receive ACK
                    // upload data to mysql
                    // generate the mqtt message, to upload to DB
                    usleep(1000 * 1000);
                    lora_tx_done = false;
                    LoRa_send(&modem);

                    syslog(LOG_DEBUG,
                           "<<%llu Sending ACK 0x%x length=%d Tsym=%f Tpkt=%f "
                           "payloadSymbNb=%u\n",
                           current_timestamp(), ack_packet.AP_Address,
                           modem.tx.data.size, modem.tx.data.Tsym,
                           modem.tx.data.Tpkt, modem.tx.data.payloadSymbNb);

                    syslog(LOG_DEBUG,
                           "<<sleep %lu ms to transmitt complete %llu\n",
                           (unsigned long)modem.tx.data.Tpkt,
                           current_timestamp());
                    usleep((unsigned long)(modem.tx.data.Tpkt * 1000) + 500);

                    while (!lora_tx_done)
                    {
                        syslog(LOG_DEBUG, "<<wait..\n");
                        usleep(1000 * 40);
                    }

                    // delay after send out ACK
                    usleep(1000 * 500);
                    //}

                    send_seq++;

                    // usleep(1000*1000);
                }
                else
                {
                    syslog(LOG_DEBUG, " invalide packet %x\n",
                           report_packet.package_StartMark);
                    usleep((random() % 300) * 1000);
                }

// break;

#if 0
			if (strstr(rxbuf, "MCU")) {
				// get the correct packet send out ACK
				time (&rawtime);
				timeinfo = localtime(&rawtime);		
				snprintf(txbuf, sizeof(txbuf), "ACK from %s {%s} %04d-%02d-%02d %02d:%02d:%02d SEQ=%u",
					hostname, rxbuf, timeinfo->tm_year + 1900, timeinfo->tm_mon + 1, timeinfo->tm_mday, 
		  			timeinfo->tm_hour, timeinfo->tm_min, timeinfo->tm_sec, send_seq);
				
				modem.tx.data.size = strlen(modem.tx.data.buf) + 1;//Payload len
				
				lora_tx_done = false;
				LoRa_send(&modem);

				printf("<<Sending [%s] length=%d Tsym=%f Tpkt=%f payloadSymbNb=%u\n",
					modem.tx.data.buf, strlen(modem.tx.data.buf), modem.tx.data.Tsym, 
					modem.tx.data.Tpkt, modem.tx.data.payloadSymbNb);

				printf("<<sleep %u ms to transmitt complete %llu\n", 
					(unsigned long)modem.tx.data.Tpkt, current_timestamp());
				usleep((unsigned long)(modem.tx.data.Tpkt * 1000));

				while (!lora_tx_done) {
					printf("<<wait..\n");
					usleep(1000*40);
				}
				send_seq++;
				usleep(1000*500);
			}else {
				// ignore the incorrect packet
				printf(">>noise packet\n");
				usleep((random() % 300)*1000);
			}
#endif
            }
        }
        else
        {
            syslog(LOG_DEBUG, "NORPT\n");
            usleep((random() % 300) * 1000);
        }
#endif
    }

    syslog(LOG_DEBUG, "END\n");

    LoRa_end(&modem);
}
