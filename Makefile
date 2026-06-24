# ©2017-2025 YUICHIRO NAKADA

PROGRAM = $(patsubst %.c,%,$(wildcard *.c))
HEAD    = $(wildcard *.h)

ifneq (, $(shell which clang))
CC = clang
endif
ifneq (, $(shell which icc))
CC = icc
endif
CFLAGS = -O3 -ffunction-sections -fdata-sections -funroll-loops -finline-functions -ftree-vectorize \
         -Wall -Wextra -Wno-unused-parameter \
         -Wno-unused-result \
         -Wno-unused-function \
         -Wno-unused-variable \
         -Wno-sign-compare \
         -Wno-missing-field-initializers \
         -Wno-aggressive-loop-optimizations \
         -Wno-stringop-overflow
LDFLAGS = -lasound -lm -Wl,-s -Wl,--gc-sections

.PHONY: all
all: $(PROGRAM)

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
