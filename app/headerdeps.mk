# Read by every app Makefile (headerdeps.pri): what the compiler said each
# object includes. qmake puts this before its first rule, so the default
# goal is kept as it was (none yet: its own `first`), not a header's.
goal := $(.DEFAULT_GOAL)
-include $(wildcard $(OBJECTS_DIR)*.d)
.DEFAULT_GOAL := $(goal)
