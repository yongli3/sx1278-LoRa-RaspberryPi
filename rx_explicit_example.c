#include "LoRa.h"

static char txbuf[LORA_TX_LEN];
static char rxbuf[LORA_RX_LEN];

static bool lora_tx_done = false;
static bool lora_rx_done = false;

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
int main(){
	int ack_retry_count = 0;
	unsigned int send_seq = 0;
	int i = 0;
	int ret = 0;
	char hostname[128];
	time_t rawtime;
	struct tm * timeinfo;
	LoRa_ctl modem;

	
	srand((unsigned)time(NULL));
	gethostname(hostname, sizeof(hostname));
//See for typedefs, enumerations and there values in LoRa.h header file
modem.spiCS = 0;//Raspberry SPI CE pin number
modem.tx.callback = tx_f;
modem.tx.data.buf = txbuf;
modem.rx.callback = rx_f;
modem.rx.data.buf = rxbuf;
memset(txbuf, 'a', sizeof(txbuf));
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
modem.eth.resetGpioN = 23;//GPIO23 on lora RESET pi
modem.eth.dio0GpioN = 24;//GPIO24 on lora DIO0 pin to control Rxdone and Txdone interrupts
modem.eth.outPower = OP20;//Output power
modem.eth.powerOutPin = PA_BOOST;//Power Amplifire pin
modem.eth.AGC = 1;//Auto Gain Control
modem.eth.OCP = 240;// 45 to 240 mA. 0 to turn off protection
modem.eth.implicitHeader = 0;//Explicit header mode
modem.eth.syncWord = 0x12;
//For detail information about SF, Error Coding Rate, Explicit header, Bandwidth, AGC, Over current protection and other features refer to sx127x datasheet https://www.semtech.com/uploads/documents/DS_SX1276-7-8-9_W_APP_V5.pdf

gpioSetMode(SW_T_PIN, PI_OUTPUT);
gpioWrite(SW_T_PIN, 0);

gpioSetMode(SW_R_PIN, PI_OUTPUT);
gpioWrite(SW_R_PIN, 0);

LoRa_begin(&modem);
send_seq = 0;
while (1) {
	printf("wait for boardcast\n");
	lora_rx_done = false;
	LoRa_receive(&modem);
	
	// wait forever
	while (!lora_rx_done) {
		usleep(LORA_WAIT_FOR_RECEIVE_MS*1000);
	}

	if (modem.rx.data.CRC) {
			printf(">>rxcrcerror [%s]\n", rxbuf);
			continue;
		} else {
			// crc okay, check if it is the correct format
			printf(">>rxbuf=[%s]-%lu %llu\n", rxbuf, strlen(rxbuf), current_timestamp());
			// make sure it is boardcast
			if (!strstr(rxbuf, "BOARDCAST")) {
				 printf("skip\n");
				 usleep(1000*100);
				 continue;
			}
			
			printf("Get Boardcast send out and wait for ACK\n");
			time (&rawtime);
			timeinfo = localtime(&rawtime);		
			snprintf(txbuf, sizeof(txbuf), "MCU %s %d [%s] %04d-%02d-%02d %02d:%02d:%02d",
				hostname, send_seq, rxbuf, timeinfo->tm_year + 1900, timeinfo->tm_mon + 1, timeinfo->tm_mday, 
	  				timeinfo->tm_hour, timeinfo->tm_min, timeinfo->tm_sec);

			modem.tx.data.size = strlen(modem.tx.data.buf) + 1;//Payload len
			
			// send out info packet and wait for ACK
			ack_retry_count = 0;
			while (ack_retry_count++ < 8) {
				printf("waitack %d\n", ack_retry_count);
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
				// receive for ACK
				memset(rxbuf, 0, sizeof(rxbuf));
				lora_rx_done = false;
				LoRa_receive(&modem);
				i = 0;
				while (!lora_rx_done) {
					if (i++ > LORA_WAIT_FOR_RECEIVE_COUNT) {
						printf("RX timeout! %d\n", i);
						break;
					}
					usleep(LORA_WAIT_FOR_RECEIVE_MS*1000);
				}
			
				if (lora_rx_done) {
					// get data, check if it is ACK
					if (modem.rx.data.CRC) {
						printf(">>rxcrcerror\n");
					} else {
						printf(">>rxbuf=[%s]-%d %llu\n", rxbuf, strlen(rxbuf), current_timestamp());
						if (strstr(rxbuf, "ACK")) {
							printf("Get ACK Finish %d!\n", send_seq);
							send_seq++;
							break;
						} else {
							printf("No ACK re-send %d\n", ack_retry_count);
						}
						usleep(1000*500);
					}
				}
			}			
			usleep(1000*(random() % 300));
		}
}


sleep(600);
printf("end\n");
LoRa_end(&modem);
}
