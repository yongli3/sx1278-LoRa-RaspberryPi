all: 4gconnect receive_implicit receive_explicit transmit_implicit lora

common.o: common.c
	gcc -Wall -Werror -std=gnu99 -g -c common.c -o common.o -lpigpio -lrt -pthread -lm -lmosquitto

LoRa.o: LoRa.c
	gcc -Wall -Werror -std=gnu99 -g -c LoRa.c -o LoRa.o -lpigpio -lrt -pthread -lm

tx_implicit_example.o: tx_implicit_example.c
	gcc -Wall -Werror -std=gnu99 -c tx_implicit_example.c -o tx_implicit_example.o -lpigpio -lrt -pthread -lm

rx_implicit_example.o: rx_implicit_example.c
	gcc -Wall -Werror -std=gnu99 -c rx_implicit_example.c -o rx_implicit_example.o -lpigpio -lrt -pthread -lm

tx_explicit_example.o: tx_explicit_example.c
	gcc -Wall -Werror -std=gnu99 -g -c tx_explicit_example.c -o tx_explicit_example.o -lpigpio -lrt -pthread -lm

rx_explicit_example.o: rx_explicit_example.c
	gcc -Wall -Werror -std=gnu99 -g -c rx_explicit_example.c -o rx_explicit_example.o -lpigpio -lrt -pthread -lm

lora: LoRa.o tx_explicit_example.o common.o
	gcc -Wall -Werror -std=gnu99 -g -o lora tx_explicit_example.o LoRa.o common.o -lpigpio -lrt -pthread -lm -lsqlite3 -lmosquitto

transmit_implicit: LoRa.o tx_implicit_example.o
	gcc -Wall -Werror -std=gnu99 -o transmit_implicit tx_implicit_example.o LoRa.o -lpigpio -lrt -pthread -lm

receive_explicit: LoRa.o rx_explicit_example.o
	gcc -Wall -Werror -std=gnu99 -g -o receive_explicit rx_explicit_example.o LoRa.o -lpigpio -lrt -pthread -lm

receive_implicit: LoRa.o rx_implicit_example.o
	gcc -Wall -Werror -std=gnu99 -o receive_implicit rx_implicit_example.o LoRa.o -lpigpio -lrt -pthread -lm

4gconnect: 4gconnect.o common.o
	gcc -Wall -Werror -std=gnu99 -O2 -o 4gconnect 4gconnect.o common.o -lrt -pthread -lm -lsqlite3 -lmosquitto -lusb
