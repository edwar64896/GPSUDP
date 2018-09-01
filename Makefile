IDIR =../include
CC=gcc
CFLAGS=-I../include -Iportaudio/include -O3 -march=native -Rpass-analysis=loop-vectorize -g -DLOG_USE_COLOR

ODIR=obj
LDIR =../lib

LIBS=-lm -lportaudio -lopus -logg -lpthread   -lsamplerate -lncurses
LIBS3=-lm -lportaudio -lopus -logg -lpthread  

_DEPS =
DEPS = $(patsubst %,$(IDIR)/%,$(_DEPS))

_OBJ3 = clktst.o
OBJ3 = $(patsubst %,$(ODIR)/%,$(_OBJ3))

_OBJ2 = gsp-opus-rx-ogg.o  pa_ringbuffer.o log.o ma.o timef.o
OBJ2 = $(patsubst %,$(ODIR)/%,$(_OBJ2))

$(ODIR)/%.o: %.c $(DEPS)
	$(CC) -c -o $@ $< $(CFLAGS)

gsp-opus-rx-ogg: $(OBJ2)
	$(CC) -o $@ $^ $(CFLAGS) $(LIBS)

clktest: $(OBJ3)
	$(CC) -o $@ $^ $(CFLAGS) $(LIBS3)


all: gsp-opus-rx-ogg

.PHONY: clean

clean:
	rm -f $(ODIR)/*.o *~ core $(INCDIR)/*~  gsp-opus-rx-ogg
