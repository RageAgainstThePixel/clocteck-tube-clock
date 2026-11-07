#!/bin/sh
# Prints the running emulator's serial log, for the API test (VER-020, VER-021).
exec cat "$HOME/.cache/tubeclock/host/emulator.log"
