# autogenerated (and then hand modified)
# overrideable vars used in implicit make rules
CFLAGS ?= -Os -flto
CPPFLAGS += -Wall -Wextra -Wshadow
LDFLAGS += ${CFLAGS}

# list of targets to build, generated from .c files containing a main() function:

TARGETS=comorse context_switch_timing cotests 
TARGETS_EXTRA=cotone

all : ${TARGETS}

# for each target, the list of objects to link, generated by recursively crawling include statements with a corresponding .c file:

comorse : comorse.o coroutine.o 
context_switch_timing : context_switch_timing.o coroutine.o timing.o 
cotests : cotests.o coroutine.o 

cotone: LDLIBS += -framework SDL2
cotone: CFLAGS += -I/Library/Frameworks/SDL2.framework/Headers/
cotone: LDFLAGS += -F/Library/Frameworks/
cotone: cotone.o coroutine.o

# for each object, the list of headers it depends on, generated by recursively crawling include statements:

comorse.o : coroutine.h 
context_switch_timing.o : coroutine.h timing.h 
coroutine.o : coroutine.h 
cotests.o : coroutine.h 
cotone.o : coroutine.h 
timing.o : timing.h 

*.o : Makefile

clean :
	$(RM) -rf *.o *.dSYM ${TARGETS} ${TARGETS_EXTRA}
.PHONY: clean all 

check : cotests
	@(./cotests 2>/dev/null | openssl md5 | grep bfdad74e6bc7bc9ab906212371eb9f80 1>/dev/null && echo pass) || echo fail

logstamp :
	echo `date -u +%Y%m%dT%H%MZ:` > /tmp/a.txt && echo "" >> /tmp/a.txt && touch changelog && cat changelog >> /tmp/a.txt && cat /tmp/a.txt > changelog && rm /tmp/a.txt

PROJDIR=$(notdir $(shell pwd))

dist : clean logstamp
	cd .. && COPYFILE_DISABLE=1 tar cjfh ${PROJDIR}_`date -u +%Y%m%dT%H%MZ`'.tar.bz2' --exclude='*xcuser*' --exclude='.DS_Store' ${PROJDIR}/