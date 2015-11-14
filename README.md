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

Load only the stuff we need and try and push some of it into virtual memory
perhaps ?

# V3 and V4 games

These are generally bigger and the interpreter also has to provide some rather
uglier and more complicated decompressors. V4 games would probably also need
virtual memory implementing to page the game database.
