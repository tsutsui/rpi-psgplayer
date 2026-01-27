PROG=		psg_play
SRCS=		psg_play.c psg_driver.c player_ui.c
OBJS=		${SRCS:.c=.o}

CFLAGS=		-O2 -Wall
LDFLAGS=

${PROG}:	${OBJS}
	${CC} ${LDFLAGS} -o $@ ${OBJS}

clean:
	rm -f ${PROG} *.o *.core
