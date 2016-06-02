#
#
#
all: l9x-1 l9x

# FIXME: add 6809 rules etc
fuzix: l9x-z80 l9x-z80-1

l9x-1: l9x.c
	$(CC) -O2 -Wall -pedantic -DTEXT_VERSION1 l9x.c -o ./l9x-1

l9x: l9x.c
	$(CC) -O2 -Wall -pedantic l9x.c -o ./l9x

l9x-z80-1: l9x.c
	fcc --nostdio -O2 -DVIRTUAL_GAME -DTEXT_VERSION1 l9x.c -c
	fcc -o l9x-z80-1 l9x.rel
	size.fuzix l9x-z80-1

l9x-z80: l9x.c
	fcc --nostdio -O2 -DVIRTUAL_GAME l9x.c -c
	fcc -o l9x-z80 l9x.rel
	size.fuzix l9x-z80

