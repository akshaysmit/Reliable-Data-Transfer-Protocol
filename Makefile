CC = g++
CFLAGS = -lm -lpthread -std=c++11

all:
	$(CC) server.cpp $(CFLAGS) -o server
	$(CC) client.cpp $(CFLAGS) -o client
