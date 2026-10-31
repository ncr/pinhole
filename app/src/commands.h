#pragma once

// pinhole's commands (pinhole status, pinhole add …): what the window asks
// of pinholed (docs/service.md), from a terminal, through the window's own
// ServiceClient. Nothing else writes the library: pinholed does it all,
// started if it is not running. -1 when the arguments name no command, so
// the window opens.
int runCommand(int argc, char *argv[]);
