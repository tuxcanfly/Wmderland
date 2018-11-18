CXX=g++
CXXFLAGS=-g -std=c++14 -Wall -lpthread -lX11
BIN=Wmderland
SRC=$(wildcard *.cpp)

all:
	$(CXX) -o $(BIN) $(SRC) $(CXXFLAGS)

clean:
	rm $(BIN)
