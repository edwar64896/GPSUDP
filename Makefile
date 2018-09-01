IDIR =../include
CC=gcc
CFLAGS=-I../include -Iportaudio/include -O3 -march=native -Rpass-analysis=loop-vectorize -g -DLOG_USE_COLOR

ODIR=obj
LDIR =../lib

LIBS=-lm -lportaudio -lopus -logg -lpthread   -lsamplerate -lncurses
LIBS3=-lm -lportaudio -lopus -logg -lpthread  

_DEPS =
DEPS = $(patsubst %,$(IDIR)/%,$(_DEPS))

_OBJ_clktest = clktst.o
OBJ_clktest = $(patsubst %,$(ODIR)/%,$(_OBJ_clktest))

_OBJ_rx-ogg = gsp-opus-rx-ogg.o  pa_ringbuffer.o log.o ma.o timef.o
OBJ_rx-ogg = $(patsubst %,$(ODIR)/%,$(_OBJ_rx-ogg))

_OBJ_ctl = gsp-ctl.o  log.o
OBJ_ctl = $(patsubst %,$(ODIR)/%,$(_OBJ_ctl))

_OBJ_tx-net = gsp-opus-tx-net.o  log.o
OBJ_tx-net = $(patsubst %,$(ODIR)/%,$(_OBJ_tx-net))

_OBJ_tx-ogg = gsp-opus-tx-ogg.o  log.o
OBJ_tx-ogg = $(patsubst %,$(ODIR)/%,$(_OBJ_tx-ogg))

_OBJ_tx = gsp-opus-tx.o  log.o
OBJ_tx = $(patsubst %,$(ODIR)/%,$(_OBJ_tx))


$(ODIR)/%.o: %.c $(DEPS)
	$(CC) -c -o $@ $< $(CFLAGS)

gsp-opus-rx-ogg: $(OBJ_rx-ogg)
	$(CC) -o $@ $^ $(CFLAGS) $(LIBS)

clktest: $(OBJ_clktest)
	$(CC) -o $@ $^ $(CFLAGS) $(LIBS3)

gsp-ctl: $(OBJ_ctl)
	$(CC) -o $@ $^ $(CFLAGS) $(LIBS)

gsp-opus-tx-net: $(OBJ_tx-net)
	$(CC) -o $@ $^ $(CFLAGS) $(LIBS)

gsp-opus-tx-ogg: $(OBJ_tx-ogg)
	$(CC) -o $@ $^ $(CFLAGS) $(LIBS)

gsp-opus-tx: $(OBJ_tx)
	$(CC) -o $@ $^ $(CFLAGS) $(LIBS)

rx: gsp-opus-rx-ogg

tx: gsp-opus-tx-net

ctl: gsp-ctl

.PHONY: clean

clean:
	rm -f $(ODIR)/*.o *~ core $(INCDIR)/*~  gsp-opus-rx-ogg
