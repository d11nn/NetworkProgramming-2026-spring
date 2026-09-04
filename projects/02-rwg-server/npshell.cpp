#include <signal.h>

#include "shell_core.h"

int main() {
	/* 
		Register a SIGCHLD handler so the parent shell reaps any finished child
		processes immediately. Without this, exited children would remain as
		zombies until the shell explicitly waited for them.
	*/
	signal(SIGCHLD, shellcore::reapChildren); // reap means recycle

	/*
		Keep the entry point thin: ShellSession owns the prompt loop, input parsing,
		command execution, pipe handling, and all reusable shell behavior.
	*/
	shellcore::ShellSession shellSession;
	shellSession.run();
	return 0;
}
