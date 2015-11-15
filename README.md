# L9X

L9X is a very tight implementation of the Level 9 game engine. It is designed
at this point to run version 2 game databases which must be just the database
(look for L9Cut). Set the define according to the game. Erik The Viking has
V1 style messages but a V2 database, the other V2 games are generally using
the later message format with lengths not end markers.

In theory a V1 game will work providing you put a proper V2 style header on the
front and compile it with the V1 messages.

# Things To Do

Double check the parsing logic is correct with regards to unknown words and
word counting

Once the Fuzix console scrolling support is tweaked add graphics by forking and
running a second graphics process.

Put a V2 header on Colossal Cave and test it

Autodetect the text table type somehow.

# V3 and V4 games

These are generally bigger and the interpreter also has to provide some rather
uglier and more complicated decompressors and output handlers. Possibly doable
but further down the list.

