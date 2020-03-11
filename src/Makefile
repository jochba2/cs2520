CXX ?= g++
CXXFLAGS=
STD=--std=c++11
LIBS=-pthread
HEADERS=thread.hpp sockets.hpp structures.hpp common.hpp 

all: routed

debug: CXXFLAGS += -g
debug: routed

routed: $(HEADERS) routed.cpp
	$(CXX) $(CXXFLAGS) $(STD) $(LIBS) routed.cpp -o routed

clean:
	rm -f routed
