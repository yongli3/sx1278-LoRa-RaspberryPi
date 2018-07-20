#include "LoRa.h"
#include<pthread.h>
#include <sys/time.h>
#include <sqlite3.h>
#include <mosquitto.h>

#define MQTT_HOST "localhost"
#define MQTT_PORT 1883

#define DB_NAME  "rawdata.db"

sqlite3 *db = NULL;

static long long current_timestamp() {
    struct timeval te; 
    gettimeofday(&te, NULL); // get current time
    long long milliseconds = te.tv_sec*1000*1000LL + te.tv_usec; // calculate microseconds
    //printf("%lld\n", te.tv_usec);
    return milliseconds;
}

void tx_f(txData *tx){
    printf("tx done %u\n", (unsigned)time(NULL));
}

static void connect_callback(struct mosquitto *mosq, void *obj, int result)
{
    printf("connect callback, rc=%d\n", result);
}

static void message_callback(struct mosquitto *mosq, void *obj, const struct mosquitto_message *message)
{
    bool match = 0;
    printf("got message '%.*s' for topic '%s'\n", message->payloadlen, (char*) message->payload, message->topic);

    mosquitto_topic_matches_sub("/devices/wb-adc/controls/+", message->topic, &match);
    if (match) {
        printf("got message for ADC topic\n");
    }
}

static void mosq_log_callback(struct mosquitto *mosq, void *userdata, int level, const char *str)
{
    /* Pring all log messages regardless of level. */
  
  switch(level){
    case MOSQ_LOG_DEBUG:
    case MOSQ_LOG_INFO:
    case MOSQ_LOG_NOTICE:
    case MOSQ_LOG_WARNING:
    case MOSQ_LOG_ERR: {
      printf("level=%i:%s\n", level, str);
    }
  }
}

static int mqtt_test()
{
    char msg[256];
    char clientid[24];
    struct mosquitto *mosq = NULL;
    int ret = 0;

    mosquitto_lib_init();

    memset(clientid, 0, sizeof(clientid));
    snprintf(clientid, sizeof(clientid) - 1, "mysql_log_%d", getpid());

    mosq = mosquitto_new(clientid, true, 0);

    if (mosq) {
        mosquitto_log_callback_set(mosq, mosq_log_callback);
        mosquitto_connect_callback_set(mosq, connect_callback);
        mosquitto_message_callback_set(mosq, message_callback);
    
        ret = mosquitto_connect(mosq, MQTT_HOST, MQTT_PORT, 60);

        if (ret) {
            printf("connect error!\n");
        }


        ret = mosquitto_loop_start(mosq);
        if (ret != MOSQ_ERR_SUCCESS) {
            printf("LOOP fail!\n");
        }

        while (1) {        
            sprintf(msg, "%d\n", current_timestamp());
            //mosquitto_subscribe(mosq, NULL, "/devices/wb-adc/controls/+", 0);
            mosquitto_publish(mosq, NULL, "testtopic", strlen(msg), msg, 0, 0);
            break;
            usleep(1000*1000);
        }
#if 0
        while (true) {
            ret = mosquitto_loop(mosq, -1, 1);
            if(ret){
                printf("connection error!\n");
                sleep(10);
                mosquitto_reconnect(mosq);
            }
        }
#endif    
    mosquitto_destroy(mosq);
    } else {
        printf("mosq new fail!\n");
    }

    mosquitto_lib_cleanup();

    return 0;
}

static int callbackInsert(void *NotUsed, int argc, char **argv, char **colName) {
    printf("+%s\n", __func__);
    return 0;
}

static int callbackSelect(void *NotUsed, int argc, char **argv, char **colName) {
    char sqlString[256];
     char *errMsg = NULL;
     int ret = -1;
    int i;
    //argc= column number argv=column value
    printf("+%s argc=%d ID=%s\n", __func__, argc, argv[0]);

    // send this record to MQTT server, and set local = 0


    sqlite3_mutex_enter(sqlite3_db_mutex(db));    
    sprintf(sqlString, "UPDATE raw set %s = 0;", colName[3]);
        ret = sqlite3_exec(db, sqlString, callbackSelect, NULL, &errMsg);
        if( ret != SQLITE_OK)
                {
                    printf("Can't exec %s: %s\n", sqlString, sqlite3_errmsg(db));
                    return -1;
                } 
                else
                {
                    printf("[%s] okay!\n", sqlString);
                }

    
    sqlite3_mutex_leave(sqlite3_db_mutex(db));

#if 0
    for (i = 0; i < argc; i++) {
        printf("%d:%s-%s\n", i, argv[i], colName[i]);
    }
#endif    
    //argv[0]; colName[0]
    //UPDATE tblStuff SET name = 'Temperature10' WHERE name = 'Temperature1'

    return 0;
}

void *threadFun1(void *ptr)
{
    char sqlString[256];
    char *errMsg = NULL;
    int ret = -1;

    int type = (int) ptr;
    fprintf(stderr,"Thread1 - %d\n",type);

while (1) {
    sqlite3_mutex_enter(sqlite3_db_mutex(db));
    // Perform some queries on the database
    sprintf(sqlString, "INSERT INTO raw(time, message, local) VALUES ('%llu','%s', 1);",
                                                            current_timestamp(), "lora");
        ret = sqlite3_exec(db, sqlString, callbackInsert, NULL, &errMsg);
        if( ret != SQLITE_OK)
                {
                    printf("Can't exec %s: %s\n", sqlString, sqlite3_errmsg(db));
                } 
                else
                {
                    printf("[%s] okay!\n", sqlString);
                }

    
    sqlite3_mutex_leave(sqlite3_db_mutex(db));
    usleep(1000000);
}   
    return  ptr;
}

void *threadFun2(void *ptr)
{
    char sqlString[256];
     int ret = -1;
     char *errMsg = NULL;
    int type = (int) ptr;
    fprintf(stderr,"Thread2 - %d\n",type);

while (1) {
    sqlite3_mutex_enter(sqlite3_db_mutex(db));
    // Perform some queries on the database
    // query all local=1; send out and set local=0
    sprintf(sqlString, "SELECT * from raw where local=1;");
        ret = sqlite3_exec(db, sqlString, callbackSelect, NULL, &errMsg);
        if( ret != SQLITE_OK)
                {
                    printf("Can't exec %s: %s\n", sqlString, sqlite3_errmsg(db));
                } 
                else
                {
                    printf("[%s] okay!\n", sqlString);
                }

    
    sqlite3_mutex_leave(sqlite3_db_mutex(db));
    usleep(1000);
}    
    return  ptr;
}

#if 1
int main() {
//create a thread for sqlite reading, and publish to mqtt
// create a thread for lora data RX, and write to local sqlite

    int ret = 0;
    pthread_t thread1, thread2;
    int thr = 1;
    int thr2 = 2;
    char *errMsg = NULL;
    sqlite3_mutex* mutex;
    char sqlString[256];

    mqtt_test();
    return 0;

    ret = sqlite3_open(DB_NAME, &db);

        if( ret != SQLITE_OK)
        {
            printf("Can't open database: %s\n", sqlite3_errmsg(db));
            return -1;
        } 
        else
        {
            printf("Open database successfully\n");
        }
        // insert into raw (time, message) values(2, "aa");

    // start the threads
    pthread_create(&thread1, NULL, *threadFun1, (void *) thr);
    pthread_create(&thread2, NULL, *threadFun2, (void *) thr2);
    // wait for threads to finish
    pthread_join(thread1,NULL);
    pthread_join(thread2,NULL);

    sqlite3_close(db);
    return 0;
}
#else
int main(){

char txbuf[128];
LoRa_ctl modem;

//See for typedefs, enumerations and there values in LoRa.h header file
modem.spiCS = 0;//Raspberry SPI CE pin number
modem.tx.callback = tx_f;
modem.tx.data.buf = txbuf;

memset(txbuf, 'a', sizeof(txbuf));

//sprintf(txbuf, "LoraLongTest%u", (unsigned)time(NULL));

printf("%s %d\n", modem.tx.data.buf, strlen(modem.tx.data.buf));
//memcpy(modem.tx.data.buf, "LoRa", 5);//copy data we'll sent to buffer

modem.tx.data.size = strlen(modem.tx.data.buf) + 1;//Payload len
modem.eth.preambleLen=6;
modem.eth.bw = BW500;//Bandwidth 62.5KHz
modem.eth.sf = SF8;//Spreading Factor 12
modem.eth.ecr = CR8;//Error coding rate CR4/8
modem.eth.CRC = 1;//Turn on CRC checking
modem.eth.freq = 434800000;// 434.8MHz
modem.eth.resetGpioN = 23;//GPIO4 on lora RESET pin
modem.eth.dio0GpioN = 24;//GPIO17 on lora DIO0 pin to control Rxdone and Txdone interrupts
modem.eth.outPower = OP20;//Output power
modem.eth.powerOutPin = PA_BOOST;//Power Amplifire pin
modem.eth.AGC = 1;//Auto Gain Control
modem.eth.OCP = 240;// 45 to 240 mA. 0 to turn off protection
modem.eth.implicitHeader = 0;//Explicit header mode
modem.eth.syncWord = 0x12;
//For detail information about SF, Error Coding Rate, Explicit header, Bandwidth, AGC, Over current protection and other features refer to sx127x datasheet https://www.semtech.com/uploads/documents/DS_SX1276-7-8-9_W_APP_V5.pdf

LoRa_begin(&modem);
while (1)
{
	memset(txbuf, 'a', sizeof(txbuf));
	
	//sprintf(txbuf, "LoraLongTest%u", (unsigned)time(NULL));
	
	//printf("%s %d\n", modem.tx.data.buf, strlen(modem.tx.data.buf));

	LoRa_send(&modem);

	printf("Sending [%s] length=%d\n", modem.tx.data.buf, strlen(modem.tx.data.buf));

	printf("Tsym: %f\n", modem.tx.data.Tsym);
	printf("Tpkt: %f\n", modem.tx.data.Tpkt);
	printf("payloadSymbNb: %u\n", modem.tx.data.payloadSymbNb);

	printf("sleep %u ms to transmitt complete %u\n", (unsigned long)modem.tx.data.Tpkt, (unsigned)time(NULL));
	usleep((unsigned long)(modem.tx.data.Tpkt * 1000));
	//sleep(((int)modem.tx.data.Tpkt/1000)+1);
	//sleep(3);
}

printf("end\n");

LoRa_end(&modem);
}
#endif
