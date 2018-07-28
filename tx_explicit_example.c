#include "LoRa.h"
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <net/if.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <termios.h>
#include <errno.h>
#include<pthread.h>
#include <sys/time.h>
#include <sqlite3.h>
#include <mosquitto.h>
#include <syslog.h>
#include <sys/sysinfo.h>

#define LORA_TX_LEN	64
#define LORA_RX_LEN	255
// wait for COUNT*DELAY
#define LORA_WAIT_FOR_RECEIVE_COUNT  (6)
#define LORA_WAIT_FOR_RECEIVE_MS  (140) 
#define WWW_INTERNAL_SECONDS	120
#define MQTT_HOST "202.120.26.119"
#define MQTT_PORT 1883

#define DB_NAME  "rawdata.db"

static char txbuf[LORA_TX_LEN];
static char rxbuf[LORA_RX_LEN];

static bool lora_tx_done = false;
static bool lora_rx_done = false;
static bool connected = true;
static sqlite3 *db = NULL;

static long long current_timestamp() {
    struct timeval te; 
    gettimeofday(&te, NULL); // get current time
    long long milliseconds = te.tv_sec*1000*1000LL + te.tv_usec; // calculate microseconds
    //printf("%lld\n", te.tv_usec);
    return milliseconds;
}

static void rx_f(rxData *rx){

	int i = 0;

	if (rx->CRC) {
		// crc error, discard
		rx->size = 0;
		printf(">>RXCRCERR\n");
	} else {
		printf(">>RXdone %llu [%s] CRC=%d size=%d RSSI=%d SNR=%f\n", 
			current_timestamp(), rx->buf, rx->CRC, rx->size, rx->RSSI, rx->SNR);
	}
#if 0
	for (i = 0; i < rx->size; i++) {
	    printf("%x\n", rx->buf[i]);
	}
	printf("%s\n", rx->buf);
#endif
	lora_rx_done = true;
}

static void tx_f(txData *tx){
    printf("<<TXdone %llu\n", current_timestamp());
	lora_tx_done = true;
}

static void connect_callback(struct mosquitto *mosq, void *obj, int result)
{
    syslog(LOG_NOTICE, "%s rc=%d\n", __func__, result);
}

static void mqttqos_connect_callback(struct mosquitto *mosq, void *obj, int result)
{
    syslog(LOG_NOTICE, "%s rc=%d\n", __func__, result);
	
} 

static void mqttqos_disconnect_callback(struct mosquitto *mosq, void *obj, int result)
{
    syslog(LOG_NOTICE, "%s rc=%d\n", __func__, result);
	//connected = false;
}

static void mqttqos_message_callback(struct mosquitto *mosq, void *obj, const struct mosquitto_message *message)
{
    bool match = 0;
    syslog(LOG_NOTICE, "got message '%.*s' for topic '%s'\n", message->payloadlen, (char*) message->payload, message->topic);

    mosquitto_topic_matches_sub("/devices/wb-adc/controls/+", message->topic, &match);
    if (match) {
        syslog(LOG_NOTICE, "got message for ADC topic\n");
    }
}

static void mqttqos_publish_callback(struct mosquitto *mosq, void *obj, int result)
{
    syslog(LOG_NOTICE, "%s rc=%d\n", __func__, result);
	mosquitto_disconnect((struct mosquitto *)obj);
}

static void message_callback(struct mosquitto *mosq, void *obj, const struct mosquitto_message *message)
{
    bool match = 0;
    syslog(LOG_NOTICE, "got message '%.*s' for topic '%s'\n", message->payloadlen, (char*) message->payload, message->topic);

    mosquitto_topic_matches_sub("/devices/wb-adc/controls/+", message->topic, &match);
    if (match) {
        syslog(LOG_NOTICE, "got message for ADC topic\n");
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
      syslog(LOG_NOTICE, "%s level=%i:[%s]\n", __func__, level, str);
    }
  }
}

static void mqttqos_log_callback(struct mosquitto *mosq, void *userdata, int level, const char *str)
{
    /* Pring all log messages regardless of level. */
  
  switch(level){
    case MOSQ_LOG_DEBUG:
    case MOSQ_LOG_INFO:
    case MOSQ_LOG_NOTICE:
    case MOSQ_LOG_WARNING:
    case MOSQ_LOG_ERR: {
      //syslog(LOG_NOTICE, "%s level=%i:[%s]\n", __func__, level, str);
    }
  }
}

static int mqtt_publish_message(char *topic, char *message)
{
    char msg[256];
    char clientid[255];
    struct mosquitto *mosq = NULL;
    int ret = 0;
	char command[255];
	char hostname[64];
	bool clean_session = true;

    syslog(LOG_NOTICE, "+%s topic=[%s] message=[%s]\n", __func__, topic, message);

	gethostname(hostname, sizeof(hostname));

    mosquitto_lib_init();

    memset(clientid, 0, sizeof(clientid));
    snprintf(clientid, sizeof(clientid) - 1, "%s_%d", hostname, getpid());

    mosq = mosquitto_new(clientid, clean_session, 0);

    if (mosq) {
        mosquitto_log_callback_set(mosq, mqttqos_log_callback);
        mosquitto_connect_callback_set(mosq, mqttqos_connect_callback);
	    mosquitto_disconnect_callback_set(mosq, mqttqos_disconnect_callback);
        mosquitto_message_callback_set(mosq, mqttqos_message_callback);
    	mosquitto_publish_callback_set(mosq, mqttqos_publish_callback);
	
        ret = mosquitto_connect(mosq, MQTT_HOST, MQTT_PORT, 60);

        if (ret) {
            syslog(LOG_ERR, "MQTT %s connect error!\n", MQTT_HOST);
			return -1;
        }

#if 0
		// trigger connect callback
		do {
			ret = mosquitto_loop(mosq, -1, 1);

		} while (ret == MOSQ_ERR_SUCCESS && connected);
#endif

		ret = mosquitto_loop_start(mosq);
		if (ret != MOSQ_ERR_SUCCESS) {
			syslog(LOG_ERR, "LOOP fail!\n");
			return -1;		
		}

		syslog(LOG_NOTICE, "publish message=[%s] len=%d size=%d\n", message, strlen(message), sizeof(message));
        mosquitto_publish(mosq, NULL, topic, strlen(message), message, 0, 0);

		mosquitto_loop_stop(mosq, false);
        
    	mosquitto_destroy(mosq);
    } else {
        syslog(LOG_ERR, "mosq new fail!\n");
    }

    mosquitto_lib_cleanup();

	syslog(LOG_NOTICE, "-%s\n", __func__);

    return 0;
}

static int mqtt_test(char *topic, char *message)
{
    char msg[256];
    char clientid[24];
    struct mosquitto *mosq = NULL;
    int ret = 0;
	char command[255];

    syslog(LOG_NOTICE, "+%s %s-%s\n", __func__, topic, message);

	// for double safe use system command to send

	sprintf(command, "mosquitto_pub -t \"%s\" -m \"%s;\" -h %s", topic, message, MQTT_HOST);


	ret = system(command);
	syslog(LOG_NOTICE, "exec [%s] return %d\n", command, ret);
    
    mosquitto_lib_init();

    memset(clientid, 0, sizeof(clientid));
    snprintf(clientid, sizeof(clientid) - 1, "clientid_%d", getpid());

    mosq = mosquitto_new(clientid, true, 0);

    if (mosq) {
        mosquitto_log_callback_set(mosq, mosq_log_callback);
        mosquitto_connect_callback_set(mosq, connect_callback);
        mosquitto_message_callback_set(mosq, message_callback);
    
        ret = mosquitto_connect(mosq, MQTT_HOST, MQTT_PORT, 60);

        if (ret) {
            syslog(LOG_ERR, "MQTT %s connect error!\n", MQTT_HOST);
			return -1;
        }

        ret = mosquitto_loop_start(mosq);
        if (ret != MOSQ_ERR_SUCCESS) {
            syslog(LOG_ERR, "LOOP fail!\n");
        }

        //while (1) 
        {        
            //sprintf(message, "%s-%d\n", message, current_timestamp());
            syslog(LOG_NOTICE, "message=[%s] len=%d size=%d\n", message, strlen(message), sizeof(message));
            //mosquitto_subscribe(mosq, NULL, "/devices/wb-adc/controls/+", 0);
            mosquitto_publish(mosq, NULL, topic, strlen(message), message, 0, 0);
            //break;
            //usleep(1000*1000);
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
        syslog(LOG_ERR, "mosq new fail!\n");
    }

    mosquitto_lib_cleanup();

	syslog(LOG_NOTICE, "publish message [%s] okay!\n", message);

    return 0;
}

static int callbackInsert(void *NotUsed, int argc, char **argv, char **colName) {
    syslog(LOG_NOTICE, "+%s\n", __func__);
    return 0;
}

static int callbackSelect(void *NotUsed, int argc, char **argv, char **colName) {
    char sqlString[256];
     char *errMsg = NULL;
     int ret = -1;
    int i;
    //argc= column number argv=column value
    syslog(LOG_NOTICE, "+%s argc=%d ID=%s\n", __func__, argc, argv[0]);

    // send this record to MQTT server, and set local = 0


    sqlite3_mutex_enter(sqlite3_db_mutex(db));    
    sprintf(sqlString, "UPDATE raw set %s = 0;", colName[3]);
        ret = sqlite3_exec(db, sqlString, callbackSelect, NULL, &errMsg);
        if( ret != SQLITE_OK)
                {
                    syslog(LOG_NOTICE, "Can't exec %s: %s\n", sqlString, sqlite3_errmsg(db));
                    return -1;
                } 
                else
                {
                    syslog(LOG_NOTICE, "[%s] okay!\n", sqlString);
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
                    syslog(LOG_NOTICE, "Can't exec %s: %s\n", sqlString, sqlite3_errmsg(db));
                } 
                else
                {
                    syslog(LOG_NOTICE, "[%s] okay!\n", sqlString);
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
                    syslog(LOG_NOTICE, "Can't exec %s: %s\n", sqlString, sqlite3_errmsg(db));
                } 
                else
                {
                    syslog(LOG_NOTICE, "[%s] okay!\n", sqlString);
                }

    
    sqlite3_mutex_leave(sqlite3_db_mutex(db));
    usleep(1000);
}    
    return  ptr;
}

static int set_interface_attribs (int fd, int speed, int parity)
{
        struct termios tty;
        memset (&tty, 0, sizeof tty);
        if (tcgetattr (fd, &tty) != 0)
        {
                printf("error %d from tcgetattr", errno);
                return -1;
        }

        cfsetospeed (&tty, speed);
        cfsetispeed (&tty, speed);

        tty.c_cflag = (tty.c_cflag & ~CSIZE) | CS8;     // 8-bit chars
        // disable IGNBRK for mismatched speed tests; otherwise receive break
        // as \000 chars
        tty.c_iflag &= ~IGNBRK;         // disable break processing
        tty.c_lflag = 0;                // no signaling chars, no echo,
                                        // no canonical processing
        tty.c_oflag = 0;                // no remapping, no delays
        tty.c_cc[VMIN]  = 0;            // read doesn't block
        tty.c_cc[VTIME] = 5;            // 0.5 seconds read timeout

        tty.c_iflag &= ~(IXON | IXOFF | IXANY); // shut off xon/xoff ctrl

        tty.c_cflag |= (CLOCAL | CREAD);// ignore modem controls,
                                        // enable reading
        tty.c_cflag &= ~(PARENB | PARODD);      // shut off parity
        tty.c_cflag |= parity;
        tty.c_cflag &= ~CSTOPB;
        tty.c_cflag &= ~CRTSCTS;

        if (tcsetattr (fd, TCSANOW, &tty) != 0)
        {
                printf ("error %d from tcsetattr", errno);
                return -1;
        }
        return 0;
}

static void set_blocking (int fd, int should_block)
{
        struct termios tty;
        memset (&tty, 0, sizeof tty);
        if (tcgetattr (fd, &tty) != 0)
        {
                printf ("error %d from tggetattr", errno);
                return;
        }

        tty.c_cc[VMIN]  = should_block ? 1 : 0;
        tty.c_cc[VTIME] = 10;            // 0.5 seconds read timeout

        if (tcsetattr (fd, TCSANOW, &tty) != 0)
                printf ("error %d setting term attributes", errno);
}

static int get_ipaddress(char *ipaddress, int size)
{

	int fd = 1;
	 struct ifreq ifr;
	  
	 char iface[] = "eth1";
	  
	 fd = socket(AF_INET, SOCK_DGRAM, 0);
	
	 //Type of address to retrieve - IPv4 IP address
	 ifr.ifr_addr.sa_family = AF_INET;
	
	 //Copy the interface name in the ifreq structure
	 strncpy(ifr.ifr_name , iface , IFNAMSIZ-1);
	
	 ioctl(fd, SIOCGIFADDR, &ifr);
	
	 close(fd);
	
	 //display result
	 strcpy(ipaddress, inet_ntoa(( (struct sockaddr_in *)&ifr.ifr_addr )->sin_addr));
	 printf("%s - %s\n" , iface , ipaddress);

	return 0;

}

int time_test()
{
  time_t rawtime;
  struct tm * timeinfo;

  time (&rawtime);
  timeinfo = localtime(&rawtime);

  printf("%04d-%02d-%02d %02d:%02d:%02d\n", 
  	timeinfo->tm_year + 1900, timeinfo->tm_mon + 1, timeinfo->tm_mday, 
  	timeinfo->tm_hour, timeinfo->tm_min, timeinfo->tm_sec);
  
  printf ("Current local time and date: %s", asctime(timeinfo));

  return 0;
}

static int www_connect()
{
    int ret = 0;
    int i = 0;
    int size = 0;
    int repeat_count = 0;
    char write_buffer[256];
    char read_buffer[256];
    char mqtt_message[256];
	char mqtt_topic[256];
	char hostname[255];
	char ipaddress[255];
    char *portname = "/dev/ttyUSB0";

	time_t rawtime;
  	struct tm * timeinfo;
	struct sysinfo info;


	
	
    int fd = open(portname, O_RDWR | O_NOCTTY | O_SYNC);
    if (fd < 0)
    {
   		syslog(LOG_ERR, "error %d opening %s: %s", errno, portname, strerror (errno));
		system("reboot");
        return -1;
    }
    
    set_interface_attribs(fd, B115200, 0);  // set speed to 115,200 bps, 8n1 (no parity)
    set_blocking(fd, 0);                // set no blocking
    
    while (0)
    {
    write (fd, "ATE0\r\n", 7);           // send 7 character greeting
    
    //sleep(1);
    usleep ((7 + 25) * 1000);             // sleep enough to transmit the 7 plus
                                         // receive 25:  approx 100 uS per char transmit
    char buf [100];
    
    memset(buf, 0, sizeof(buf));
    
    int n = read(fd, buf, sizeof(buf));  // read up to 100 characters if ready to read
    
    printf("read %d buf=%s\n", n, buf);
    sleep(1);
    }

    while (1) {
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

    //for (i = 0; i < size; i++)
       //printf("%x\n", read_buffer[i]);

    if (strstr(read_buffer, "OK")) {
        printf("Modem OK\n");
        break;
    } else {
        if (repeat_count > 5) {
            syslog(LOG_ERR, "MODEM ATE0 fail!\n");
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
sleep(4);
memset(read_buffer, 0, sizeof(read_buffer));
size = read(fd, read_buffer, sizeof(read_buffer));
printf("read %d [%s]\n", size, read_buffer);    
if (NULL == strstr(read_buffer, "OK")) {
    syslog(LOG_ERR, "command %s error !\n", write_buffer);
    goto cleanup;
};

memset(write_buffer, 0, sizeof(write_buffer));
sprintf(write_buffer, "%s", "AT+CFUN=1\r\n");
size = write(fd,write_buffer,strlen(write_buffer));
tcflush(fd, TCOFLUSH);
printf("write %s %d\n", write_buffer, size);
sleep(7);
memset(read_buffer, 0, sizeof(read_buffer));
size = read(fd, read_buffer, sizeof(read_buffer));
printf("read %d [%s]\n", size, read_buffer);    
if (NULL == strstr(read_buffer, "OK")) {
    syslog(LOG_ERR, "command %s error !\n", write_buffer);
    goto cleanup;
};

    memset(write_buffer, 0, sizeof(write_buffer));
    sprintf(write_buffer, "%s", "AT+CGACT=1,1\r\n");
    tcflush(fd, TCOFLUSH);
    size = write(fd,write_buffer,strlen(write_buffer));
    printf("write %s %d\n", write_buffer, size);
    sleep(7);
    memset(read_buffer, 0, sizeof(read_buffer));
    size = read(fd, read_buffer, sizeof(read_buffer));
    printf("read %d [%s]\n", size, read_buffer);    
    if (NULL == strstr(read_buffer, "OK")) {
        syslog(LOG_ERR, "command %s error !\n", write_buffer);
        goto cleanup;
    };

    // +ZGIPDNS: 1,1,"IP","10.101.206.237","0.0.0.0","116.116.116.116","221.5.88.88"
    //strcpy(mqtt_message, read_buffer);

    memset(write_buffer, 0, sizeof(write_buffer));
    sprintf(write_buffer, "%s", "AT+ZGACT=1,1\r\n");
    size = write(fd,write_buffer,strlen(write_buffer));
    tcflush(fd, TCOFLUSH);
    printf("write %s %d\n", write_buffer, size);
    sleep(7);
    memset(read_buffer, 0, sizeof(read_buffer));
    size = read(fd, read_buffer, sizeof(read_buffer));
    printf("read %d [%s]\n", size, read_buffer);    


    if (NULL == strstr(read_buffer, "OK")) {
        syslog(LOG_ERR, "command %s error !\n", write_buffer);
        goto cleanup;
    }

    //ret = system("ifconfig eth0 down");
    //printf("system return %d\n", ret);
    
    ret = system("udhcpc -i eth1");
    printf("system return %d\n", ret);

	sleep(3);
	gethostname(hostname, sizeof(hostname));

	printf("hostname=%s\n", hostname);

	//sprintf(hostname, "%s-network", hostname);

	system("systemctl restart systemd-timesyncd.service");
	
	while (1) {
		// send to systemd
		printf("Checking network ...\n");
		// check if internet is okay
		
		ret = system("ping 114.114.114.114 -c 1");
		if (ret) {
			syslog(LOG_ERR, "network error!\n");
			break;
		}

		get_ipaddress(ipaddress, sizeof(ipaddress));
		sysinfo(&info);
		snprintf(mqtt_message, sizeof(mqtt_message), "IP=%s;UP=%d", ipaddress, info.uptime);
		//syslog(LOG_NOTICE, "mqtt_message=%s hostname=%s\n", mqtt_message, hostname);

		time (&rawtime);
		timeinfo = localtime(&rawtime);

		snprintf(mqtt_topic, sizeof(mqtt_topic), "%s-%04d-%02d-%02d %02d:%02d:%02d", hostname,
		  timeinfo->tm_year + 1900, timeinfo->tm_mon + 1, timeinfo->tm_mday, 
		  timeinfo->tm_hour, timeinfo->tm_min, timeinfo->tm_sec);

		ret = mqtt_publish_message(mqtt_topic, mqtt_message);
		if (ret) {
			syslog(LOG_ERR, "MQTT error!\n");
			break;
		}
		sleep(WWW_INTERNAL_SECONDS);
	}    
cleanup:    
    close(fd);



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

#if 0
int main() {
//create a thread for sqlite reading, and publish to mqtt
// create a thread for lora data RX, and write to local sqlite
	int i = 0;
    int ret = 0;
    pthread_t thread1, thread2;
    int thr = 1;
    int thr2 = 2;
    char *errMsg = NULL;
    sqlite3_mutex* mutex;
    char sqlString[256];

	openlog("4G", LOG_CONS | LOG_PID | LOG_NDELAY, LOG_LOCAL1);

	syslog(LOG_NOTICE, "version %s", __DATE__);

	i = 0;

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

    // start the threads for sqlite3 test
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
	unsigned int send_seq = 0;
	int i = 0;
	int ret = 0;
	char hostname[128];
	time_t rawtime;
	struct tm * timeinfo;
	LoRa_ctl modem;

	gethostname(hostname, sizeof(hostname));

//See for typedefs, enumerations and there values in LoRa.h header file
modem.spiCS = 0;//Raspberry SPI CE pin number
modem.tx.callback = tx_f;
modem.tx.data.buf = txbuf;

modem.rx.callback = rx_f;
modem.rx.data.buf = rxbuf;

memset(txbuf, 'a', sizeof(txbuf));

//sprintf(txbuf, "LoraLongTest%u", (unsigned)time(NULL));

//printf("%s %d\n", modem.tx.data.buf, strlen(modem.tx.data.buf));
//memcpy(modem.tx.data.buf, "LoRa", 5);//copy data we'll sent to buffer

modem.tx.data.size = strlen(modem.tx.data.buf) + 1;//Payload len
modem.eth.preambleLen=6;
// data speed for 64 bytes
// BW500+SF7=87ms  DIO high=21.7ms 106ms
// BW500+SF12 = 900ms

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
send_seq = 0;
LoRa_begin(&modem);
while (1)
{
	memset(txbuf, 'a', sizeof(txbuf));

	time (&rawtime);
	timeinfo = localtime(&rawtime);		
	snprintf(txbuf, sizeof(txbuf), "BOARDCAST %u %s TX %04d-%02d-%02d %02d:%02d:%02d", send_seq++,
		hostname, timeinfo->tm_year + 1900, timeinfo->tm_mon + 1, timeinfo->tm_mday, 
	  timeinfo->tm_hour, timeinfo->tm_min, timeinfo->tm_sec);
	
	//sprintf(txbuf, "LoraLongTest%u", (unsigned)time(NULL));
	
	//printf("%s %d\n", modem.tx.data.buf, strlen(modem.tx.data.buf));

#if 1	
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

	//usleep(1000*500);
#endif

#if 1
	// tx done, start to receive
	usleep(LORA_WAIT_FOR_RECEIVE_MS*100);
	memset(rxbuf, 0, sizeof(rxbuf));
	lora_rx_done = false;
	LoRa_receive(&modem);

	//sleep(100);
	i = 0;
	while (!lora_rx_done) {
		if (i++ > LORA_WAIT_FOR_RECEIVE_COUNT) {
			printf("RX timeout! %d\n", i);
			break;
		}
		usleep(LORA_WAIT_FOR_RECEIVE_MS*1000);
	}

	if (lora_rx_done) {
		// get data, send out ACK
		if (modem.rx.data.CRC) {
			printf(">>rxcrcerror\n");
		} else {
			printf(">>rxbuf=[%s]-%d %llu\n", rxbuf, strlen(rxbuf), current_timestamp());
			// data crc okay, send out ACK
			time (&rawtime);
			timeinfo = localtime(&rawtime);		
			snprintf(txbuf, sizeof(txbuf), "ACK %s [%s] %04d-%02d-%02d %02d:%02d:%02d",
				hostname, rxbuf, timeinfo->tm_year + 1900, timeinfo->tm_mon + 1, timeinfo->tm_mday, 
	  			timeinfo->tm_hour, timeinfo->tm_min, timeinfo->tm_sec);
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

			usleep(1000*500);
		}
	}
#endif
}

printf("end\n");

LoRa_end(&modem);
}
#endif
