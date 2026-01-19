APP=osc_gen_ui
SRC=main.c
CFLAGS=`pkg-config --cflags gtk4` -Wall -Wextra -g
LDLIBS=`pkg-config --libs gtk4` -lm

all: $(APP)

$(APP): $(SRC)
	$(CC) $(SRC) $(CFLAGS) $(LDLIBS) -o $(APP)

clean:
	rm -f $(APP) *.o
