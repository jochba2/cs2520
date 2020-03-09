CXX ?= g++
STD=--std=c++98
LIBS=-lpthread
HEADERS=thread.hpp sockets.hpp structures.hpp common.hpp 

all: routed interface

routed: $(HEADERS) routed.cpp
	$(CXX) $(STD) $(LIBS) routed.cpp -o routed

interface: $(HEADERS) interface.cpp
	$(CXX) $(STD) $(LIBS) interface.cpp -o interface

