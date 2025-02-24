CC ?= gcc
CFLAGS += -std=c99 -Wall -Wextra -pedantic -MMD -ggdb
LDFLAGS += -lm

# Platform detection
ifeq ($(OS),Windows_NT)
	PLATFORM = Windows
else
	PLATFORM = $(shell uname -s)
endif

# Platform-specific settings
ifeq ($(PLATFORM),Windows)
	EXE_EXT = .exe
	ARGP_PATH = /usr/local
	CFLAGS += -I$(ARGP_PATH)/include -DWINDOWS_COMPAT
	LDFLAGS += -L$(ARGP_PATH)/bin -largp -lparted-2 -ludev
	INSTALL = install
	LN = cp
else ifeq ($(PLATFORM),Darwin)
	ARGP_PATH = $(shell brew --prefix 2>/dev/null || echo /usr/local)
	CFLAGS += -I$(ARGP_PATH)/include
	LDFLAGS += -L$(ARGP_PATH)/lib -largp
else ifeq ($(PLATFORM),Linux)
	LDFLAGS += -ludev -lparted
endif

# All targets with platform extensions
ALL_TARGETS = f3write f3read f3probe f3brew f3fix
ALL_TARGETS := $(addsuffix $(EXE_EXT),$(ALL_TARGETS))

all: $(ALL_TARGETS)

docker:
	docker build -f Dockerfile -t f3:latest .

install:
	$(INSTALL) -d $(DESTDIR)$(PREFIX)/bin
	$(INSTALL) -m755 $(ALL_TARGETS) $(DESTDIR)$(PREFIX)/bin
	$(INSTALL) -d $(DESTDIR)$(PREFIX)/share/man/man1
	$(INSTALL) -m644 f3read.1 $(DESTDIR)$(PREFIX)/share/man/man1
	$(LN) -f f3read.1 $(DESTDIR)$(PREFIX)/share/man/man1/f3write.1

# Build rules
f3write$(EXE_EXT): utils.o libflow.o f3write.o
	$(CC) -o $@ $^ $(LDFLAGS)

f3read$(EXE_EXT): utils.o libflow.o f3read.o
	$(CC) -o $@ $^ $(LDFLAGS)

f3probe$(EXE_EXT): libutils.o libdevs.o libprobe.o f3probe.o
	$(CC) -o $@ $^ $(LDFLAGS)

f3brew$(EXE_EXT): libutils.o libdevs.o f3brew.o
	$(CC) -o $@ $^ $(LDFLAGS)

f3fix$(EXE_EXT): libutils.o f3fix.o
	$(CC) -o $@ $^ $(LDFLAGS)

-include *.d

.PHONY: cscope clean

cscope:
	cscope -b *.c *.h

clean:
	rm -f *.o *.d cscope.out $(ALL_TARGETS)
