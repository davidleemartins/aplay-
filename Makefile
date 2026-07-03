# ©2017-2025 YUICHIRO NAKADA
# Modifications ©2026 David Lee Martins

PROGRAM = $(patsubst %.c,%,$(wildcard *.c))
HEAD    = $(wildcard *.h)

ifneq (, $(shell which clang))
CC = clang
endif
ifneq (, $(shell which icc))
CC = icc
endif
CFLAGS = -O3 -g -ffunction-sections -fdata-sections -funroll-loops -finline-functions -ftree-vectorize \
         -Wall -Wextra -Wno-unused-parameter \
         -Wno-unused-result \
         -Wno-unused-function \
         -Wno-unused-variable \
         -Wno-sign-compare \
         -Wno-missing-field-initializers \
         -Wno-aggressive-loop-optimizations
# Keep symbols (-g above, no strip) and export them (-rdynamic) so the built-in
# crash handler prints a readable backtrace. `make release` builds a stripped
# copy for distribution and keeps this one as the debug artifact.
LDFLAGS = -lasound -lm -rdynamic -Wl,--gc-sections

.PHONY: all
all: $(PROGRAM)

# Release build: keep the unstripped binary as a debug artifact (for addr2line
# against crash-handler backtraces) and ship the stripped copy (~77% smaller).
.PHONY: release
release: aplay+
	cp aplay+ aplay+.debug
	strip aplay+
	@ls -l aplay+ aplay+.debug

.PHONY: test
test:
	bash tests/run_tests.sh

$(PROGRAM): % : %.o
	$(CC) $< -o $@ $(LDFLAGS)

%.o : %.c $(HEAD)
	$(CC) $(CFLAGS) -c $(@F:.o=.c) -o $@

.PHONY: clean
clean:
	$(RM) $(PROGRAM) $(OBJS) _depend.inc *.o

.PHONY: depend
depend: $(OBJS:.o=.c)
	-@ $(RM) _depend.inc
	-@ for i in $^; do cpp -MM $$i | sed "s/\ [_a-zA-Z0-9][_a-zA-Z0-9]*\.c//g" >> _depend.inc; done

-include _depend.inc
