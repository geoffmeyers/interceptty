AM_CFLAGS                  = @CFLAGS@ -Wall
bin_PROGRAMS = interceptty 
bin_SCRIPTS = interceptty-nicedump
man_MANS = interceptty.1
interceptty_SOURCES = common.c common.h interceptty.c bsd-openpty.h bsd-openpty.c
EXTRA_DIST = test testport interceptty.pod $(man_MANS) $(bin_SCRIPTS)
check-local: interceptty test
	$(SHELL) ./test
