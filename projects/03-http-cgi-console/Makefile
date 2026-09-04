CXX := g++
CXXFLAGS := -std=c++14 -Wall -pedantic
LDLIBS := -lboost_system
ifeq ($(OS),Windows_NT)
ifneq ($(BOOST_ROOT),)
INCLUDES := -I$(BOOST_ROOT)/include
LIBDIRS := -L$(BOOST_ROOT)/lib
else
INCLUDES :=
LIBDIRS :=
endif
SOCKET_LIBS := -lws2_32 -lwsock32
else
INCLUDES := -I /usr/local/include
LIBDIRS := -L /usr/local/lib
PTHREAD := -pthread
endif

CONSOLE_COMPONENTS := console_components.cpp console_components.hpp

.PHONY: all part1 part2 tests clean

all: part1 part2

part1: http_server console.cgi

part2: cgi_server.exe

tests: test_components

http_server: http_server.cpp $(CONSOLE_COMPONENTS)
	$(CXX) $(CXXFLAGS) $(PTHREAD) $(INCLUDES) $(LIBDIRS) http_server.cpp console_components.cpp -o $@ $(LDLIBS)

console.cgi: console.cpp $(CONSOLE_COMPONENTS)
	$(CXX) $(CXXFLAGS) $(PTHREAD) $(INCLUDES) $(LIBDIRS) console.cpp console_components.cpp -o $@ $(LDLIBS)

cgi_server.exe: cgi_server.cpp $(CONSOLE_COMPONENTS)
	$(CXX) $(CXXFLAGS) $(PTHREAD) $(INCLUDES) $(LIBDIRS) cgi_server.cpp console_components.cpp -o $@ $(LDLIBS) $(SOCKET_LIBS)

test_components: tests/test_components.cpp $(CONSOLE_COMPONENTS)
	$(CXX) $(CXXFLAGS) $(PTHREAD) $(INCLUDES) $(LIBDIRS) tests/test_components.cpp console_components.cpp -o $@ $(LDLIBS)

clean:
ifeq ($(OS),Windows_NT)
	-del /Q http_server console.cgi cgi_server.exe test_components 2>NUL
else
	rm -f http_server console.cgi cgi_server.exe test_components
endif
