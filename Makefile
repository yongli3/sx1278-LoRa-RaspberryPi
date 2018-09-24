all: 4gconnect receive_implicit receive_explicit transmit_implicit lora

LoRa.o: LoRa.c
	gcc -g -c LoRa.c -o LoRa.o -lpigpio -lrt -pthread -lm

tx_implicit_example.o: tx_implicit_example.c
	gcc -c tx_implicit_example.c -o tx_implicit_example.o -lpigpio -lrt -pthread -lm

rx_implicit_example.o: rx_implicit_example.c
	gcc -c rx_implicit_example.c -o rx_implicit_example.o -lpigpio -lrt -pthread -lm

tx_explicit_example.o: tx_explicit_example.c
	gcc -g -c tx_explicit_example.c -o tx_explicit_example.o -lpigpio -lrt -pthread -lm

rx_explicit_example.o: rx_explicit_example.c
	gcc -g -c rx_explicit_example.c -o rx_explicit_example.o -lpigpio -lrt -pthread -lm

lora: LoRa.o tx_explicit_example.o
	gcc -g -o lora tx_explicit_example.o LoRa.o -lpigpio -lrt -pthread -lm -lsqlite3 -lmosquitto

transmit_implicit: LoRa.o tx_implicit_example.o
	gcc -o transmit_implicit tx_implicit_example.o LoRa.o -lpigpio -lrt -pthread -lm

receive_explicit: LoRa.o rx_explicit_example.o
	gcc -g -o receive_explicit rx_explicit_example.o LoRa.o -lpigpio -lrt -pthread -lm

receive_implicit: LoRa.o rx_implicit_example.o
	gcc -o receive_implicit rx_implicit_example.o LoRa.o -lpigpio -lrt -pthread -lm

4gconnect: 4gconnect.o
	gcc -O2 -o 4gconnect 4gconnect.o -lrt -pthread -lm -lsqlite3 -lmosquitto -lusb
