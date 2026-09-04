CXX = g++

.DEFAULT_GOAL := default

.PHONY: default all part1 part2 commands clean

default: npshell np_simple

all: part1 part2

part1: npshell commands

part2: np_simple commands

npshell: npshell.cpp
	$(CXX) npshell.cpp -o npshell

np_simple: np_simple.cpp
	$(CXX) np_simple.cpp -o np_simple

commands: bin/noop bin/number bin/removetag bin/removetag0 bin/ls bin/cat

bin/noop: commands/noop.cpp
	mkdir -p bin
	$(CXX) commands/noop.cpp -o bin/noop

bin/number: commands/number.cpp
	mkdir -p bin
	$(CXX) commands/number.cpp -o bin/number

bin/removetag: commands/removetag.cpp
	mkdir -p bin
	$(CXX) commands/removetag.cpp -o bin/removetag

bin/removetag0: commands/removetag0.cpp
	mkdir -p bin
	$(CXX) commands/removetag0.cpp -o bin/removetag0

bin/ls:
	mkdir -p bin
	cp /bin/ls bin/ls

bin/cat:
	mkdir -p bin
	cp /bin/cat bin/cat

clean:
	rm -f npshell np_simple
	rm -f bin/noop bin/number bin/removetag bin/removetag0 bin/ls bin/cat
