PROG=		psg_play
SRCS=		psg_play.c
SRCS+=		p6psg.c psg_driver.c player_ui.c
SRCS+=		psg_backend_rpi_gpio.c
OBJS=		${SRCS:.c=.o}

CFLAGS=		-O2 -Wall
LDFLAGS=

${PROG}:	${OBJS}
	${CC} ${LDFLAGS} -o $@ ${OBJS}

clean:
	rm -f ${PROG} *.o *.core

psg_play.o:	psg_driver.h player_ui.h p6psg.h
p6psg.o:	p6psg.h
psg_driver.o:	player_ui.h ym2149f.h
psg_player.o:	player_ui.h
psg_backend_rpi_gpio.o:	psg_backend.h psg_backend_rpi_gpio.h ym2149f.h
