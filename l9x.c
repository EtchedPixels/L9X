/*
 * (C) Copright 2015 Alan Cox
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * Parts of this code are based upon poking through level9, the Level 9
 * interpreter by Glen Summers et al.
 */

#define TEXT_VERSION2

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <ctype.h>
#include <time.h>

static uint8_t game[27000];
static uint16_t gamesize;

static uint8_t *messages;
static uint8_t *worddict;
static uint8_t *dictionary;
static uint8_t *exitmap;
static uint8_t *pc;
static uint8_t *pcbase;

static uint8_t game_over;

static uint8_t opcode;

#define STACKSIZE 256		/* Unclear what we need for V1/V2 */
static uint16_t stackbase[STACKSIZE];
static uint16_t *stack = stackbase;

#define LISTSIZE 1024
static uint8_t lists[LISTSIZE];	/* Probably much bigger for later games */
static uint16_t variables[256];
static uint8_t *tables[16];

static char buffer[80];
static uint8_t wordbuf[3];
static uint8_t wordcount;

static uint16_t seed;	/* Random numbers */

/*
 *	I/O routines. Need to remove stdio and add wrapping logic
 */
void print_num(uint16_t num)
{
  printf("%d", num);
}

void print_char(uint8_t c)
{
  if (c == 0x25)
    c = '\n';
  else if (c == 0x5F)
    c = ' ';
  putchar(c);
}

void read_line(void)
{
  char *p;
  if (fgets(buffer, sizeof(buffer), stdin) == NULL)
    exit(1);
  p = strchr(buffer, '\n');
  if (p)
    *p = 0;
}

void error(const char *p)
{
  write(2, p, strlen(p));
  write(2, "\n", 1);
  exit(1);
}

#ifdef TEXT_VERSION1
/* Call initially with the messages as the pointer. Each message contains
   a mix of character codes and dictionary numbers giving pieces of text
   to substitute. The dictionary is permitted to self reference giving an
   extremely elegant compressor that beats the Infocom and AdventureSoft /
   Adveture International compressors while being smaller ! */

/* FIXME: for version 2 games they swapped the 1 markers for length bytes
   with an odd hack where a 0 length means 255 + nextbyte (unless 0 if so
   repeat */
void decompress(uint8_t *p, uint16_t m)
{
  uint8_t d;
  /* Walk the table looking for 1 bytes and counting off our
     input */
  while(m--)
    while(*p++ != 1);
  while((d = *p++) > 2) {
    if (d < 0x5E)
      print_char(d + 0x1d);
    else
      decompress(worddict, d - 0x5E);
  }
}
#else

uint8_t *msglen(uint8_t *p, uint16_t *l)
{
  *l = 0;
  while(!*p) {
    *l += 255;
    p++;
  }
  *l += *p++;
  return p;
}
  
void decompress(uint8_t *p, uint16_t m)
{
  uint8_t d;
  uint16_t l;
  /* Walk the table skipping messages */
  if (m == 0)
    return;
  while(--m) {
    p = msglen(p, &l);
    p += l - 1;
  }
  p = msglen(p, &l);
  /* A 1 byte message means its 0 text chars long */
  while(--l) {
    d = *p++;
    if (d < 3)
      return;
    if (d < 0x5E)
      print_char(d + 0x1d);
    else
      decompress(worddict - 1, d - 0x5d);
  }
}
#endif

void print_message(uint16_t m)
{
  decompress(messages, m);
}

/*
 *	More complex bits the engine has methods for
 */

static uint8_t reverse[] = {
  0x10, 0x14, 0x16, 0x17,
  0x11, 0x18, 0x12, 0x13,
  0x15, 0x1a, 0x19, 0x1c,
  0x1b
};

void lookup_exit(void)
{
  uint8_t l = variables[*pc++];
  uint8_t d = variables[*pc++];
  uint8_t *p = exitmap;
  uint8_t v;
  uint8_t ls = l;

/*  printf("Finding exit %d for location %d => ", d, l); */
  /* Scan through the table finding 0x80 end markers */
  l--;		/* No entry 0 */
  while (l--) {
    do {
      v = *p;
      p += 2;
    } while (!(v & 0x80));
  }
/*  printf("(%04x) ", p - exitmap); */
  /* Now find our exit */
  /* Basically each entry is a word in the form
     [Last.1][BiDir.1][Flags.2][Exit.4][Target.8] */
  do {
    v = *p;
/*    printf("%02x:", v); */
    if ((v & 0x0F) == d) {
      variables[*pc++] = ((*p++) >> 4) & 7;	/* Flag bits */
      variables[*pc++] = *p++;
/*      printf("Found %d\n", variables[pc[-1]]); */
      return;
    }
    p+=2;
  } while(!(v & 0x80));
  /* Exits can be bidirectional - we have to now sweep the whole table looking
     for a backlinked exit */
  if (d <= 12) {
    d = reverse[d];
    p = exitmap;
    l = 1;
    do {
/*      if (ls == p[1])
        printf("%02x:%d / %02x\n", *p, p[1], d); */
      v = *p++;
      if (*p++ == ls && ((v & 0x1f) == d)) {
        variables[*pc++] = (v >> 4) & 7;
        variables[*pc++] = l;
        return;
      }
      if (v & 0x80)
        l++;
    } while(*p);
  }
/*  printf("None\n"); */
  /* FIXME: check v1 games use backlinks ? */
  variables[*pc++] = 0;
  variables[*pc++] = 0;
}

uint8_t wordcmp(char *s, uint8_t *p, uint8_t *v)
{
  do {
    if (*s != 0 && toupper(*s++) != (*p & 0x7F))
      return 0;
  } while(!(*p++ & 0x80));
  *v = *p;
  return 1;
}

uint8_t matchword(char *s)
{
  uint8_t *p = dictionary;
  uint8_t v;
  
  do {
/*    outword(p); */
    if (wordcmp(s, p, &v) == 1)
      return v;
    /* Find the next word */
    while(*p && !(*p & 0x80))
      p++;
    p++;
    p++;
  } while ((*p & 0x80) == 0);
  /* FIXME: correct code for non match check */
  return 0xFF;
}

void do_input(void)
{
  uint8_t *w = wordbuf;
  char *p = buffer;
  char *s;

  wordcount = 0;
  read_line();

  while(*p) {
    while (isspace(*p))
      p++;
    /* Now at word start */
    wordcount++;
    /* Check - do we count unknown words */
    s = p;
    while(*p && !isspace(*p))
      p++;
    /* The text between s and p-1 is now the word */
    *p++ = 0;
    if (w < wordbuf + sizeof(wordbuf))
      *w++ = matchword(s);
  }

  /* Finally put the first 3 words and the count into variables */
  w = wordbuf;
  variables[*pc++] = *w++;
  variables[*pc++] = *w++;
  variables[*pc++] = *w++;
  variables[*pc++] = wordcount;
}

/*
 *	Implement the core Level 9 machine (for version 1 and 2 anyway)
 *
 *	This is an extremely elegant and very compact bytecode with
 *	various helpers for "game" things.
 */

uint16_t constant(void)
{
  uint16_t r = *pc++;
  if (!(opcode & 0x40))
    r |= (*pc++) << 8;
  return r;
}

uint8_t *address(void)
{
  if (opcode & 0x20) {
    int8_t s = (int8_t)*pc++;
    return pc + s - 1;
  }
  pc += 2;
  return pcbase + pc[-2] + (pc[-1] << 8);
} 

void skipaddress(void)
{
  if (!(opcode & 0x20))
    pc++;
  pc++;
}

/* List ops access a small fixed number of tables */
void listop(void)
{
  uint8_t *base = tables[(opcode & 0x1F) + 1];
  if (base == NULL)
    error("bad list");
/*  fprintf(stderr, "List %d base %p\n", (opcode & 0x1F) + 1, base); */
  if (opcode & 0x20)
    base += variables[*pc++];
  else
    base += *pc++;
  if ((base >= game && base < game + gamesize) ||
      (base >= lists && base < lists + sizeof(lists))) {
    if (!(opcode & 0x40)) {
/*      printf("L%d O%ld read %d\n",
        (opcode & 0x1F) + 1, base - tables[(opcode & 0x1F) + 1], *base); */
      variables[*pc++] = *base;
    } else { 
/*      printf("L%d O%ld assign %d\n",
        (opcode & 0x1F) + 1, base - tables[(opcode & 0x1F) + 1], variables[*pc]); */
      *base = variables[*pc++];
    }
  } else
    fprintf(stderr, "LISTFAULT %p %p %p\n", base, game, lists);
}  

void execute(void)
{
  uint8_t *base;
  uint8_t tmp;
  uint16_t tmp16;
  

  while(!game_over) {
    opcode = *pc++;
/*    fprintf(stderr, "%02x:", opcode);  */
    if (opcode & 0x80)
      listop();
    else switch(opcode & 0x1f) {
      case 0:
        pc = address();
        break;
      case 1: {
          uint8_t *newpc = address();
          if (stack == stackbase + sizeof(stackbase))
            error("stack overflow");
/*          printf("PUSH %04x\n", pc - pcbase); */
          *stack++ = pc - pcbase;
          pc = newpc;
        }
        break;
      case 2:
        if (stack == stackbase)
          error("stack underflow");
        pc = pcbase + *--stack;
/*        printf("POP %04x\n", pc - pcbase); */
        break;
      case 3:
        print_num(variables[*pc++]);
        break;
      case 4:
        print_message(variables[*pc++]);
        break;
      case 5:
        print_message(constant());
        break;
      case 6:
        switch(*pc++) {
          case 1:
            game_over = 1;
            /* FIXME: call driver in later game engines */
            break;
          case 2:
            /* Emulate the random number algorithm in the original */
            seed = (((seed << 8) + 0x0A - seed) << 2) + seed + 1;
            variables[*pc++] = seed & 0xff;
            break;
          case 3:
/*            save_game(); */
            break;
          case 4:
/*            load_game(); */
            break;
          case 5:
            memset(variables, 0, sizeof(variables));
            break;
          case 6:
            stack = stackbase;
            break;
          default:
            fprintf(stderr, "Unknown driver function %d\n", pc[-1]);
            exit(1);
        }
        break;
      case 7:
        do_input();
        break;
      case 8:
        tmp16 = constant();
        variables[*pc++] = tmp16;
        break;
      case 9:
        variables[pc[1]] = variables[*pc];
        pc += 2;
        break;
      case 10:
/*        fprintf(stderr, "V%d (%d) += V%d (%d)\n", pc[1], variables[pc[1]],
          *pc, variables[*pc]); */
        variables[pc[1]] += variables[*pc];
        pc += 2;
        break;
      case 11:
        variables[pc[1]] -= variables[*pc];
        pc += 2;
        break;
      case 14: /* This looks weird, but its basically a jump table */
        base = pcbase + (*pc + (pc[1] << 8));
        base += 2 * variables[pc[2]];	/* 16bit entries * */
        pc = pcbase + *base + (base[1] << 8);
        break;
      case 15:
        lookup_exit();
        break;
      case 16:
        /* These two are defined despite gcc whining. It doesn't matter
           which way around they get evaluated */
        if (variables[*pc++] == variables[*pc++])
          pc = address();
        else
          skipaddress();
        break;
      case 17:
        if (variables[*pc++] != variables[*pc++])
          pc = address();
        else
          skipaddress();
        break;
      case 18:
        tmp = *pc++;
        if (variables[tmp] < variables[*pc++])
          pc = address();
        else
          skipaddress();
        break;
      case 19:
        tmp = *pc++;
        if (variables[tmp] > variables[*pc++])
          pc = address();
        else
          skipaddress();
        break;
      case 24:
        if (variables[*pc++] == constant())
          pc = address();
        else
          skipaddress();
        break;
      case 25:
        if (variables[*pc++] != constant())
          pc = address();
        else
          skipaddress();
        break;
      case 26:
        if (variables[*pc++] < constant())
          pc = address();
        else
          skipaddress();
        break;
      case 27:
        if (variables[*pc++] > constant())
          pc = address();
        else
          skipaddress();
        break;
      case 21:
        /* clear screen */
        pc++;	/* value indicates screen to clear */
        break;
      case 22:
        /* picture */
        pc++;
        break;
      case 20:
        /* graphics mode */
      case 23:
        /* getnextobject */
      case 28:
        /* print input */      
      default:
        fprintf(stderr, "bad op %d\n", opcode);
        exit(1);
    }
  }
}


int main(int argc, char *argv[])
{
  int fd;
  uint8_t off = 4;
  int i;
  
  if (argc == 1) {
    error("l9x [game.dat]\n");
    exit(1);
  }
  fd = open(argv[1], O_RDONLY);
  if (fd == -1) {
    perror(argv[1]);
    exit(1);
  }
  /* FIXME: allocate via sbrk once removed stdio usage */
  if ((gamesize = read(fd, game, sizeof(game))) < 1024) {
    error("l9x: not a valid game\n");
    exit(1);
  }
  close(fd);

  /* Header starts with message and decompression dictionary */
  messages = game + (game[0] | (game[1] << 8));
  worddict = game + (game[2] | (game[3] << 8));
  printf("Messages at %04lx\n", messages - game);
  printf("Word Dictionary at %04lx\n", worddict - game);
  /* Then the tables for list ops */
  for (i = 0; i  < 12; i++) {
    uint16_t v = game[off] | (game[off + 1] << 8);
    printf("Table %d at %04x\n", i, v);
    if (i != 11 && (v & 0x8000))
      tables[i] = lists + (v & 0x7FFF);
    else
      tables[i] = game + v;
    off += 2;
  }
  /* Some of which have hard coded uses */
  exitmap = tables[0];
  dictionary = tables[1];
  pcbase = pc = tables[11];
  /* 3 and 4 are used for getnextobject and friends on later games,
     9 is used for driver magic and ramsave stuff */
  
  printf("Beginning execution PC = %04lx\n", pc - game);
  
  seed = time(NULL);

  print_message(1);
  execute();
}
