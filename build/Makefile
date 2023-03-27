CXX = g++
CFLAGS = -std=c++11 -O2 -g 
TARGET = webserver
OBJS = ../code/buffer/*.cpp ../code/http/*.cpp ../code/locker/*.cpp\
       ../code/log/*.cpp ../code/socket_control/*.cpp ../code/sqlconnpool/*.cpp\
       ../code/timer/*.cpp ../code/main.cpp

all: $(OBJS)
	$(CXX) $(CFLAGS) $(OBJS) -o ../bin/$(TARGET)  -lpthread -lmysqlclient

clean:
	rm -rf ../bin/$(OBJS) $(TARGET)



