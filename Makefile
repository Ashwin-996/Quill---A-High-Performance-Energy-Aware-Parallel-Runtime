CXX = g++
CC = gcc

CXXFLAGS = -O3 -Wall -std=c++11
CFLAGS = -O3 -Wall
LDFLAGS = -lpthread -lpfm

TARGET = streams_bench

all: clean $(TARGET)

$(TARGET): streams.o quill-runtime.o profiler.o
	$(CXX) $(CXXFLAGS) -o $(TARGET) streams.o quill-runtime.o profiler.o $(LDFLAGS)

streams.o: streams.cpp
	$(CXX) $(CXXFLAGS) -c streams.cpp -o streams.o

quill-runtime.o: quill-runtime.cpp
	$(CXX) $(CXXFLAGS) -c quill-runtime.cpp -o quill-runtime.o

profiler.o: profiler/profiler.c
	$(CC) $(CFLAGS) -c profiler/profiler.c -o profiler.o

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET) *.o
