# Every object made again when a header it includes changes, the ones it
# came to include after qmake last ran too. qmake lists those headers only
# as it writes the Makefile, and the build scripts run it only when a project
# file changed: an object that went on to include a header was then left
# built against its old layout (the bar's `pinhole recent --follow` freed a
# ServiceClient of the size it once had, and aborted). The compiler says
# what each object read (-MMD, beside it as .d; -MP, so a header removed is
# no error), and headerdeps.mk has make read that.
QMAKE_CFLAGS += -MMD -MP
QMAKE_CXXFLAGS += -MMD -MP
QMAKE_EXTRA_INCLUDES += $$PWD/headerdeps.mk
