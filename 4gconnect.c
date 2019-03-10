#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
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

#include <termios.h>
#include <unistd.h>
#include <usb.h>

#include "common.h"

// version string
static char versionString[128];

static bool connected = true;
static sqlite3* db = NULL;

static long long current_timestamp()
{
    struct timeval te;
    gettimeofday(&te, NULL); // get current time
    // long long milliseconds = te.tv_sec*1000*1000LL + te.tv_usec; // calculate
    // microseconds
    long long microseconds = te.tv_sec * 1000LL + te.tv_usec / 1000;
    // printf("%lld\n", te.tv_usec);
    return microseconds;
}

static int mqtt_test(char* topic, char* message)
{
    char msg[256];
    char clientid[24];
    struct mosquitto* mosq = NULL;
    int ret = 0;
    char command[255];

    syslog(LOG_NOTICE, "+%s %s-%s\n", __func__, topic, message);

    // for double safe use system command to send

    sprintf(command, "mosquitto_pub -t \"%s\" -m \"%s;\" -h %s", topic, message,
            MQTT_HOST);

    ret = system(command);
    syslog(LOG_NOTICE, "exec [%s] return %d\n", command, ret);

    mosquitto_lib_init();

    memset(clientid, 0, sizeof(clientid));
    snprintf(clientid, sizeof(clientid) - 1, "clientid_%d", getpid());

    mosq = mosquitto_new(clientid, true, 0);

    if (mosq)
    {
        mosquitto_log_callback_set(mosq, mqttqos_log_callback);
        mosquitto_connect_callback_set(mosq, mqttqos_connect_callback);
        mosquitto_message_callback_set(mosq, mqttqos_message_callback);

        ret = mosquitto_connect(mosq, MQTT_HOST, MQTT_PORT, 60);

        if (ret)
        {
            syslog(LOG_ERR, "MQTT %s connect error!\n", MQTT_HOST);
            return -1;
        }

        ret = mosquitto_loop_start(mosq);
        if (ret != MOSQ_ERR_SUCCESS)
        {
            syslog(LOG_ERR, "LOOP fail!\n");
        }

        // while (1)
        {
            // sprintf(message, "%s-%d\n", message, current_timestamp());
            syslog(LOG_NOTICE, "message=[%s] len=%d size=%d\n", message,
                   strlen(message), sizeof(message));
            // mosquitto_subscribe(mosq, NULL, "/devices/wb-adc/controls/+", 0);
            mosquitto_publish(mosq, NULL, topic, strlen(message), message, 0,
                              0);
            // break;
            // usleep(1000*1000);
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
    }
    else
    {
        syslog(LOG_ERR, "mosq new fail!\n");
    }

    mosquitto_lib_cleanup();

    syslog(LOG_NOTICE, "publish message [%s] okay!\n", message);

    return 0;
}

static int callbackInsert(void* NotUsed, int argc, char** argv, char** colName)
{
    syslog(LOG_NOTICE, "+%s\n", __func__);
    return 0;
}

static int callbackSelect(void* NotUsed, int argc, char** argv, char** colName)
{
    char sqlString[256];
    char* errMsg = NULL;
    int ret = -1;
    int i;
    // argc= column number argv=column value
    syslog(LOG_NOTICE, "+%s argc=%d ID=%s\n", __func__, argc, argv[0]);

    // send this record to MQTT server, and set local = 0

    sqlite3_mutex_enter(sqlite3_db_mutex(db));
    sprintf(sqlString, "UPDATE raw set %s = 0;", colName[3]);
    ret = sqlite3_exec(db, sqlString, callbackSelect, NULL, &errMsg);
    if (ret != SQLITE_OK)
    {
        syslog(LOG_NOTICE, "Can't exec %s: %s\n", sqlString,
               sqlite3_errmsg(db));
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
    // argv[0]; colName[0]
    // UPDATE tblStuff SET name = 'Temperature10' WHERE name = 'Temperature1'

    return 0;
}

void* threadFun1(void* ptr)
{
    char sqlString[256];
    char* errMsg = NULL;
    int ret = -1;

    int type = (int)ptr;
    fprintf(stderr, "Thread1 - %d\n", type);

    while (1)
    {
        sqlite3_mutex_enter(sqlite3_db_mutex(db));
        // Perform some queries on the database
        sprintf(
            sqlString,
            "INSERT INTO raw(time, message, local) VALUES ('%llu','%s', 1);",
            current_timestamp(), "lora");
        ret = sqlite3_exec(db, sqlString, callbackInsert, NULL, &errMsg);
        if (ret != SQLITE_OK)
        {
            syslog(LOG_NOTICE, "Can't exec %s: %s\n", sqlString,
                   sqlite3_errmsg(db));
        }
        else
        {
            syslog(LOG_NOTICE, "[%s] okay!\n", sqlString);
        }

        sqlite3_mutex_leave(sqlite3_db_mutex(db));
        usleep(1000000);
    }
    return ptr;
}

void* threadFun2(void* ptr)
{
    char sqlString[256];
    int ret = -1;
    char* errMsg = NULL;
    int type = (int)ptr;
    fprintf(stderr, "Thread2 - %d\n", type);

    while (1)
    {
        sqlite3_mutex_enter(sqlite3_db_mutex(db));
        // Perform some queries on the database
        // query all local=1; send out and set local=0
        sprintf(sqlString, "SELECT * from raw where local=1;");
        ret = sqlite3_exec(db, sqlString, callbackSelect, NULL, &errMsg);
        if (ret != SQLITE_OK)
        {
            syslog(LOG_NOTICE, "Can't exec %s: %s\n", sqlString,
                   sqlite3_errmsg(db));
        }
        else
        {
            syslog(LOG_NOTICE, "[%s] okay!\n", sqlString);
        }

        sqlite3_mutex_leave(sqlite3_db_mutex(db));
        usleep(1000);
    }
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

    int ret = -1;
    int fd = 1;
    struct ifreq ifr;
    char iface[] = "eth1";

    syslog(LOG_DEBUG, "+%s\n", __func__);

    fd = socket(AF_INET, SOCK_DGRAM, 0);

    if (fd < 0)
    {
        syslog(LOG_ERR, "socket error! %d(%s)", errno, strerror(errno));
        return -1;
    }

    // Type of address to retrieve - IPv4 IP address
    ifr.ifr_addr.sa_family = AF_INET;

    // Copy the interface name in the ifreq structure
    strncpy(ifr.ifr_name, iface, IFNAMSIZ - 1);

    ret = ioctl(fd, SIOCGIFADDR, &ifr);
    close(fd);
    if (ret < 0)
    {
        syslog(LOG_ERR, "ioctl error! %d(%s)", errno, strerror(errno));
        return -1;
    }

    // display result
    strcpy(ipaddress,
           inet_ntoa(((struct sockaddr_in*)&ifr.ifr_addr)->sin_addr));

    syslog(LOG_DEBUG, "%s [%s]\n", iface, ipaddress);

    return 0;
}

int time_test()
{
    time_t rawtime;
    struct tm* timeinfo;

    time(&rawtime);
    timeinfo = localtime(&rawtime);

    printf("%04d-%02d-%02d %02d:%02d:%02d\n", timeinfo->tm_year + 1900,
           timeinfo->tm_mon + 1, timeinfo->tm_mday, timeinfo->tm_hour,
           timeinfo->tm_min, timeinfo->tm_sec);

    printf("Current local time and date: %s", asctime(timeinfo));

    return 0;
}

static int reset_usb_main(int argc, char* argv[])
{
    struct usb_bus* busses;
    usb_init();
    usb_find_busses();
    usb_find_devices();
    busses = usb_get_busses();
    struct usb_bus* bus;
    int c, i, a;
    for (bus = busses; bus; bus = bus->next)
    {
        struct usb_device* dev;
        int val;
        usb_dev_handle* junk;
        for (dev = bus->devices; dev; dev = dev->next)
        {
            char buf[1024];
            junk = usb_open(dev);
            usb_get_string_simple(junk, 2, buf, 1023);
            switch (argc)
            {
                case 1:
                    if (junk == NULL)
                    {
                        printf("Can't open %p (%s)\n", dev, buf);
                    }
                    else if (strcmp(buf, "HD Pro Webcam C920") == 0)
                    {
                        val = usb_reset(junk);
                        printf("reset %p %d (%s)\n", dev, val, buf);
                    }
                    break;

                default:
                    if (junk == NULL)
                    {
                        syslog(LOG_ERR, "Can't open %p (%s)\n", dev, buf);
                    }
                    else
                    {
                        val = usb_reset(junk);
                        syslog(LOG_INFO, "reset %p %d (%s)\n", dev, val, buf);
                    }
            }

            usb_close(junk);
        }
    }
}

/*
ate0
at+cgsn
at+cimi
at+cfun=0
at+cfun=1
at+cgact=1,1
at+zgact=1,1
*/
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
    char* portname = "/dev/ttyUSB0";
    int fd = -1;

    time_t rawtime;
    struct tm* timeinfo;
    struct sysinfo info;
    struct stat fileSt;
    struct statvfs statRootFs;
    struct statvfs statVarFs;	

    repeat_count = 0;
    while (1)
    {
        fd = open(portname, O_RDWR | O_NOCTTY | O_SYNC);
        if (fd < 0)
        {
            syslog(LOG_ERR, "%d opening %s error %d(%s)", repeat_count,
                   portname, errno, strerror(errno));
            repeat_count++;
            if (repeat_count > 5)
            {
                syslog(LOG_ERR, "Opening %s error %d(%s)! return...", portname,
                       errno, strerror(errno));
                return -1;
                // sync();
                // system("reboot -f");
            }
            reset_usb_main(2, NULL);
            sleep(10);
        }
        else
        {
            break;
        }
    }

    set_interface_attribs(fd, B115200,
                          0); // set speed to 115,200 bps, 8n1 (no parity)
    set_blocking(fd, 0);      // set no blocking

    repeat_count = 0;
    while (1)
    {
        memset(write_buffer, 0, sizeof(write_buffer));

        // echo disable
        sprintf(write_buffer, "%s", "ATE0\r\n");
        size = write(fd, write_buffer, strlen(write_buffer));
        tcflush(fd, TCIOFLUSH);
        syslog(LOG_DEBUG, "write [%s] %d\n", write_buffer, size);
        // usleep (size * 1000);
        sleep(1);
        memset(read_buffer, 0, sizeof(read_buffer));
        size = read(fd, read_buffer, sizeof(read_buffer));
        syslog(LOG_DEBUG, "read %d [%s]\n", size, read_buffer);

        // for (i = 0; i < size; i++)
        // printf("%x\n", read_buffer[i]);

        if (strstr(read_buffer, "OK"))
        {
            syslog(LOG_DEBUG, "Modem OK\n");
            break;
        }
        else
        {
            if (repeat_count > 5)
            {
                syslog(LOG_ERR, "MODEM ATE0 fail!\n");
                goto cleanup;
            }
        };
        repeat_count++;
        sleep(1);
    }

    // Get IMEI
    memset(write_buffer, 0, sizeof(write_buffer));
    sprintf(write_buffer, "%s", "AT+CGSN\r\n");
    size = write(fd, write_buffer, strlen(write_buffer));
    tcflush(fd, TCOFLUSH);
    syslog(LOG_DEBUG, "write [%s] %d\n", write_buffer, size);
    sleep(1);
    size = read(fd, read_buffer, sizeof(read_buffer));
    syslog(LOG_DEBUG, "read %d [%s]\n", size, read_buffer);

    // Get CIMI
    memset(write_buffer, 0, sizeof(write_buffer));
    sprintf(write_buffer, "%s", "AT+CIMI\r\n");
    size = write(fd, write_buffer, strlen(write_buffer));
    tcflush(fd, TCOFLUSH);
    syslog(LOG_DEBUG, "write %s %d\n", write_buffer, size);
    sleep(1);
    size = read(fd, read_buffer, sizeof(read_buffer));
    syslog(LOG_DEBUG, "read %d [%s]\n", size, read_buffer);

    memset(write_buffer, 0, sizeof(write_buffer));
    sprintf(write_buffer, "%s", "AT+CFUN=0\r\n");
    size = write(fd, write_buffer, strlen(write_buffer));
    tcflush(fd, TCOFLUSH);
    syslog(LOG_DEBUG, "write %s %d\n", write_buffer, size);
    sleep(4);
    memset(read_buffer, 0, sizeof(read_buffer));
    size = read(fd, read_buffer, sizeof(read_buffer));
    syslog(LOG_DEBUG, "read %d [%s]\n", size, read_buffer);
    if (NULL == strstr(read_buffer, "OK"))
    {
        syslog(LOG_ERR, "command %s error !\n", write_buffer);
        goto cleanup;
    };

    memset(write_buffer, 0, sizeof(write_buffer));
    sprintf(write_buffer, "%s", "AT+CFUN=1\r\n");
    size = write(fd, write_buffer, strlen(write_buffer));
    tcflush(fd, TCOFLUSH);
    syslog(LOG_DEBUG, "write %s %d\n", write_buffer, size);
    sleep(5);
    memset(read_buffer, 0, sizeof(read_buffer));
    size = read(fd, read_buffer, sizeof(read_buffer));
    syslog(LOG_DEBUG, "read %d [%s]\n", size, read_buffer);
    if (NULL == strstr(read_buffer, "OK"))
    {
        syslog(LOG_ERR, "command %s error !\n", write_buffer);
        goto cleanup;
    };

    // re-try for this command
    repeat_count = 0;
    while (1)
    {
        memset(write_buffer, 0, sizeof(write_buffer));
        sprintf(write_buffer, "%s", "AT+CGACT=1,1\r\n");
        tcflush(fd, TCOFLUSH);
        size = write(fd, write_buffer, strlen(write_buffer));
        syslog(LOG_DEBUG, "write %s %d\n", write_buffer, size);
        sleep(5);
        memset(read_buffer, 0, sizeof(read_buffer));
        size = read(fd, read_buffer, sizeof(read_buffer));
        syslog(LOG_DEBUG, "read %d [%s]\n", size, read_buffer);
        if (NULL == strstr(read_buffer, "OK"))
        {
            if (repeat_count > 3)
            {
                syslog(LOG_ERR, "command %s error !\n", write_buffer);
                goto cleanup;
            }
            else
            {
                syslog(LOG_WARNING, "command %s error Retry!\n", write_buffer);
            }
        }
        else
        {
            // OKay
            break;
        };
        repeat_count++;
        sleep(1);
    }

    // +ZGIPDNS:
    // 1,1,"IP","10.101.206.237","0.0.0.0","116.116.116.116","221.5.88.88"
    // strcpy(mqtt_message, read_buffer);

    memset(write_buffer, 0, sizeof(write_buffer));
    sprintf(write_buffer, "%s", "AT+ZGACT=1,1\r\n");
    size = write(fd, write_buffer, strlen(write_buffer));
    tcflush(fd, TCOFLUSH);
    syslog(LOG_DEBUG, "write %s %d\n", write_buffer, size);
    sleep(5);
    memset(read_buffer, 0, sizeof(read_buffer));
    size = read(fd, read_buffer, sizeof(read_buffer));
    syslog(LOG_DEBUG, "read %d [%s]\n", size, read_buffer);

    if (NULL == strstr(read_buffer, "OK"))
    {
        syslog(LOG_ERR, "command %s error !\n", write_buffer);
        goto cleanup;
    }

    // ret = system("ifconfig eth0 down");
    // printf("system return %d\n", ret);

    // ret = system("systemctl restart connman"); // system("udhcpc -i eth1");
    // syslog(LOG_DEBUG, "connman return %d\n", ret);

    sleep(3);
    gethostname(hostname, sizeof(hostname));

    syslog(LOG_DEBUG, "hostname=%s\n", hostname);

    // sprintf(hostname, "%s-network", hostname);

    ret = system("systemctl restart systemd-timesyncd.service");
    syslog(LOG_DEBUG, "timesync return %d\n", ret);

    while (1)
    {
#if 0
        // send to systemd
        syslog(LOG_DEBUG, "Checking network ...\n");
        // check if internet is okay

        ret = system("ping 114.114.114.114 -c 1");
        if (ret)
        {
            syslog(LOG_ERR, "network error!\n");
            break;
        }
#endif
        ret = get_ipaddress(ipaddress, sizeof(ipaddress));
        if (ret)
        {
            syslog(LOG_ERR, "get_IP error!\n");
            goto cleanup;
        }

        // Check Ip address 0.0.0.0; 169.
        if (0 == strncmp("0.0.0.0", ipaddress, 7))
        {
            syslog(LOG_ERR, "[%s] incorrect!\n", ipaddress);
            goto cleanup;
        }

        if (0 == strncmp("169.", ipaddress, 4))
        {
            syslog(LOG_ERR, "[%s] incorrect!\n", ipaddress);
            goto cleanup;
        }

       // Get current system status
        statRootFs.f_bsize = 0;
		statRootFs.f_bavail = 0;
		statVarFs.f_bsize = 0;
		statVarFs.f_bavail = 0;
		statvfs("/", &statRootFs);
		statvfs("/var/log/syslog", &statVarFs);
  
       fileSt.st_size = 0;
        stat("/var/log/syslog", &fileSt);
        sysinfo(&info);
        snprintf(mqtt_message, sizeof(mqtt_message),
                 "IP=%s;UP=%lu;LOADS=%lu-%lu-%lu;TOTAL=%lu;FREE=%lu;PROCS=%d;LOG=%lu;FS=%llu-%llu;VER=%s", ipaddress, info.uptime,
                 info.loads[0], info.loads[1], info.loads[2], info.totalram, info.freeram, info.procs, fileSt.st_size, 
                 (uint64_t)statRootFs.f_bsize * (uint64_t)statRootFs.f_bavail, 
                 (uint64_t)statVarFs.f_bsize * (uint64_t)statVarFs.f_bavail, versionString);
        // syslog(LOG_NOTICE, "mqtt_message=%s hostname=%s\n", mqtt_message,
        // hostname);

        time(&rawtime);
        timeinfo = localtime(&rawtime);

        snprintf(mqtt_topic, sizeof(mqtt_topic),
                 "%s-%04d-%02d-%02d %02d:%02d:%02d", hostname,
                 timeinfo->tm_year + 1900, timeinfo->tm_mon + 1,
                 timeinfo->tm_mday, timeinfo->tm_hour, timeinfo->tm_min,
                 timeinfo->tm_sec);

        // the message published to server
        // 9999-2019-03-10 10:58:28 IP=10.82.186.238;UP=5842;VER=Mar  9
        // 2019-14:24:28

        ret = mqtt_publish_message(mqtt_topic, mqtt_message);
        if (ret)
        {
            syslog(LOG_ERR, "MQTT error!\n");
            goto cleanup;
        }
        sleep(IP_REPORT_INTERNAL_SECONDS);
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
     not used
    ret = system("udhcpc -i eth1");
    printf("system return %d\n", ret);

    

cleanup:
    close(fd);
    return ret;
}

#endif

#if 1
int main()
{
    // create a thread for sqlite reading, and publish to mqtt
    // create a thread for lora data RX, and write to local sqlite
    int i = 0;
    int ret = 0;
    pthread_t thread1, thread2;
    int thr = 1;
    int thr2 = 2;
    char* errMsg = NULL;
    sqlite3_mutex* mutex;
    char sqlString[256];

    openlog("4GConnect", LOG_CONS | LOG_PID | LOG_NDELAY, LOG_LOCAL1);

    // printf("%04d-%02d-%02dT%02d:%02d:%02d\n", BUILD_YEAR, BUILD_MONTH,
    // BUILD_DAY, BUILD_HOUR, BUILD_MIN, BUILD_SEC);

    sprintf(versionString, "%04d%02d%02d", BUILD_YEAR, BUILD_MONTH, BUILD_DAY);
    syslog(LOG_INFO, "INFO Built on %s", versionString);
    syslog(LOG_DEBUG, "DEBUG Built on %s %s", __DATE__, __TIME__);
    syslog(LOG_ERR, "ERR Built on %s %s", __DATE__, __TIME__);

    i = 0;
#if 0
	while (1) {
		sprintf(sqlString, "topic-%03d", i++);
		mqtt_qos_test(sqlString, "testmessage");
		usleep(1000 * 1000);
	}
#endif
    www_connect();

    syslog(LOG_DEBUG, "APP Exit!\n");
    closelog();
    return 0;

    mqtt_test("topic", "message");
    return 0;

    ret = sqlite3_open(DB_NAME, &db);

    if (ret != SQLITE_OK)
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
    pthread_create(&thread1, NULL, *threadFun1, (void*)thr);
    pthread_create(&thread2, NULL, *threadFun2, (void*)thr2);
    // wait for threads to finish
    pthread_join(thread1, NULL);
    pthread_join(thread2, NULL);

    sqlite3_close(db);
    return 0;
}
#else
#endif
