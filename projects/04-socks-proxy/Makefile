CXX := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -pedantic -O2
LDFLAGS := -lboost_system -pthread

.PHONY: all clean

all: socks_server pj4.cgi

socks_server: socks_server.cpp
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDFLAGS)

pj4.cgi: pj4.cpp console_components.hpp
	$(CXX) $(CXXFLAGS) pj4.cpp -o $@ $(LDFLAGS)

clean:
	rm -f socks_server pj4.cgi
