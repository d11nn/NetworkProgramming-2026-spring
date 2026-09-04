CXX = g++

.DEFAULT_GOAL := default

.PHONY: default all part1 part2 commands clean

TARGETS :=
COMMAND_TARGETS := bin/ls bin/cat

ifneq ($(wildcard npshell.cpp),)
TARGETS += npshell
PART1_TARGETS += npshell
endif

ifneq ($(wildcard np_simple.cpp),)
TARGETS += np_simple
PART2_TARGETS += np_simple
endif

ifneq ($(wildcard np_single_proc.cpp),)
TARGETS += np_single_proc
endif

ifneq ($(wildcard np_multi_proc.cpp),)
TARGETS += np_multi_proc
endif

ifneq ($(wildcard commands/noop.cpp),)
COMMAND_TARGETS += bin/noop
endif

ifneq ($(wildcard commands/number.cpp),)
COMMAND_TARGETS += bin/number
endif

ifneq ($(wildcard commands/removetag.cpp),)
COMMAND_TARGETS += bin/removetag
endif

ifneq ($(wildcard commands/removetag0.cpp),)
COMMAND_TARGETS += bin/removetag0
endif

ifneq ($(COMMAND_TARGETS),)
PART1_TARGETS += commands
PART2_TARGETS += commands
endif

default: $(TARGETS)

all: part1 part2

part1: $(PART1_TARGETS)

part2: $(PART2_TARGETS)

np_single_proc: np_single_proc.cpp
	$(CXX) np_single_proc.cpp -o np_single_proc

np_multi_proc: np_multi_proc.cpp
	$(CXX) np_multi_proc.cpp -o np_multi_proc

npshell: npshell.cpp
	$(CXX) npshell.cpp -o npshell

np_simple: np_simple.cpp
	$(CXX) np_simple.cpp -o np_simple

commands: $(COMMAND_TARGETS)

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
	rm -f npshell np_simple np_single_proc np_multi_proc
	rm -f bin/noop bin/number bin/removetag bin/removetag0 bin/ls bin/cat
