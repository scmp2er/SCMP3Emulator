/*
 *	A minimal INS8073 machine
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <signal.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <sys/select.h>
#include "ns807x.h"

#define PATCH_ROM 1

static uint8_t ramrom[65536];
static uint8_t fast;
static volatile unsigned done;
static unsigned trace;
struct ns8070 *cpu;
static uint8_t pendc;			/* Pending char */

struct ns8070 {
    uint16_t pc;
    uint16_t t;
    uint16_t sp;
    uint16_t p2;
    uint16_t p3;
    uint8_t a;
    uint8_t e;
    uint8_t s;
    uint8_t *rom;
    uint8_t ram[64];

    uint8_t i;
    uint8_t int_latch;
#define INT_A	1
#define INT_B	2
    uint8_t input;

    int trace;			/* TODO */
};


/* Our little I/O ROM so we don't have decode bitbanging */
static const uint8_t iorom[] = {
/* FD00 */	0x00,			/* external I/O */
/* FD01 */	0x05,0xFD,		/* address of get helper */
/* FD03 */	0x0A,0xFD,		/* address of put helper */
/* FD05 */	0xC2,0xFF,		/* LD A,-1(P2)   P2 is FD00 so this reads FCFF */
/* FD07 */	0x6C,0xFC,		/* BZ -5 */
/* FD09 */	0x5C,			/* RET */
/* FD0A */	0xCA,0xF5,		/* ST A,-0B(P2)  again FCFF */
/* FD0C */	0x5C			/* RET */
};
	
#define TRACE_MEM	1
#define TRACE_IO	2
#define TRACE_CPU	4

int check_chario(void)
{
	fd_set i, o;
	struct timeval tv;
	unsigned int r = 0;

	FD_ZERO(&i);
	FD_SET(0, &i);
	FD_ZERO(&o);
	FD_SET(1, &o);
	tv.tv_sec = 0;
	tv.tv_usec = 0;

	if (select(2, &i, &o, NULL, &tv) == -1) {
		if (errno == EINTR)
			return 0;
		perror("select");
		exit(1);
	}
	if (FD_ISSET(0, &i))
		r |= 1;
	if (FD_ISSET(1, &o))
		r |= 2;
	return r;
}

unsigned int next_char(void)
{
	char c;
	if (read(0, &c, 1) != 1) {
		printf("(tty read without ready byte)\n");
		return 0xFF;
	}
	if (c == 0x0A)
		c = '\r';

	if (c == 0x1a) { // ^Z
		exit(1);
	}
	if (c == 0x1b) { // ESC
		exit(1);
	}
	if (c == 0x7f) { // ESC
		c = 0x08;
	}
	if( (c>='a')&&(c<='z') ) {c=c-0x20;}
	
	
	return c;
}

uint8_t ns8070_read_op(struct ns8070 *cpu, uint16_t addr, int debug)
{
	if (addr == 0xFCFF) {
		uint8_t r = pendc;
		if (r) {
			pendc = 0;
			ns8070_set_a(cpu, 1);
			write(1, &r, 1);
		}
		return r;
	}
	if (addr >= 0xFD00 && addr < 0xFD00 + sizeof(iorom))
		return iorom[addr - 0xFD00];
        if (addr >= 0x2000)
            return 0xFF;
        return ramrom[addr];
}

uint8_t mem_debug_read(struct ns8070 *cpu, uint16_t addr)
{
	return ns8070_read_op(cpu, addr, 1);
}

uint8_t mem_read(struct ns8070 *cpu, uint16_t addr)
{
	return ns8070_read_op(cpu, addr, 0);
}

void mem_write(struct ns8070 *cpu, uint16_t addr, uint8_t val)
{
    if (addr == 0xFCFF) {
    	val &= 0x7F;
    	write(1, &val, 1);
    }
    /* BASIC and spare ROM */
    if (addr < 0x1000)
        return;
    if (addr < 0x2000)
        ramrom[addr] = val;
}

void flag_change(struct ns8070 *cpu, uint8_t fbits)
{
}

/*static void poll_irq_event(void)
{
} */

static struct termios saved_term, term;

static void cleanup(int sig)
{
	tcsetattr(0, TCSADRAIN, &saved_term);
	done = 1;
}

static void exit_cleanup(void)
{
	tcsetattr(0, TCSADRAIN, &saved_term);
}

static void usage(void)
{
	fprintf(stderr,
		"flexbox: [-f] -r rompath] [-d debug]\n");
	exit(EXIT_FAILURE);
}

/****************************************************************************
 *	メモリー内容をダンプ.
 ****************************************************************************
 */
void mem_dump(char *msg,int adr,void *ptr,int len)
{
	unsigned char *p = (unsigned char *)ptr;
	int i;//,j,c;
	fprintf(stderr,"%s:\n",msg);

	for(i=0; i<len; i++) {
		if( (i & 15) == 0 ) fprintf(stderr,"%04x",adr);
		if( (i & 15) == 8 ) fprintf(stderr," ");
		fprintf(stderr," %02x",*p);
		p++;adr++;
		if( (i & 15) == 15 ) {
			fprintf(stderr,"\n");
		}
	}
	fprintf(stderr,"\n");
}

void dumpreg(struct ns8070 *cpu)
{
	mem_dump("ram",0xffc0,&cpu->ram[0]     ,64);
//	mem_dump("rom",0xff00,&cpu->rom[0xff00],256);
}

//#define ROMSIZE	2560
#define ROMSIZE	0x1100

int main(int argc, char *argv[])
{
/*	static struct timespec tc; */
	int opt;
	int fd;
	int rom = 1;
	char *rompath = "8070NIBL.rom";
/*	unsigned int cycles = 0; */

	while ((opt = getopt(argc, argv, "d:fi:r:")) != -1) {
		switch (opt) {
		case 'r':
			rompath = optarg;
			break;
		case 'd':
			trace = atoi(optarg);
			break;
		case 'f':
			fast = 1;
			break;
		default:
			usage();
		}
	}
	if (optind < argc)
		usage();

		
	if (rom) {
		memset(ramrom,0xff,sizeof(ramrom) );

		fd = open(rompath, O_RDONLY);
		if (fd == -1) {
			perror(rompath);
			exit(EXIT_FAILURE);
		}
		if (read(fd, ramrom, ROMSIZE) != ROMSIZE) {
			fprintf(stderr, "nybbles: short rom '%s'.\n",
				rompath);
			exit(EXIT_FAILURE);
		}
		close(fd);

#if PATCH_ROM 
		// TXTBGN PATCH ( =X'8000 )
		ramrom[0x0c]=0x14;  // BASIC PROGRAM TEXT = 0x1400
#endif
	}

	/* Patch in our I/O hooks */
	cpu = ns8070_create(ramrom);
	ns8070_reset(cpu);
	ns8070_trace(cpu, trace & TRACE_CPU);
	ns8070_set_a(cpu, 1);
	ns8070_set_b(cpu, 1);

	/* 5ms - it's a balance between nice behaviour and simulation
	   smoothness */
/*	tc.tv_sec = 0;
	tc.tv_nsec = 5000000L; */

	if (tcgetattr(0, &term) == 0) {
		saved_term = term;
		atexit(exit_cleanup);
		signal(SIGINT, cleanup);
		signal(SIGQUIT, cleanup);
		signal(SIGPIPE, cleanup);
		term.c_lflag &= ~(ICANON | ECHO);
		term.c_cc[VMIN] = 0;
		term.c_cc[VTIME] = 1;
		term.c_cc[VINTR] = 0;
		term.c_cc[VSUSP] = 0;
		term.c_cc[VSTOP] = 0;
		tcsetattr(0, TCSADRAIN, &term);
	}

	/* This is the wrong way to do it but it's easier for the moment. We
	   should track how much real time has occurred and try to keep cycle
	   matched with that. The scheme here works fine except when the host
	   is loaded though */

	while (!done) {
		/* TODO: timing, ints etc */
		ns8070_execute_one(cpu);
#if 0
		// for debug.
		if ( cpu->pc == 0x00d5 ) { dumpreg(cpu);}
		if ( cpu->pc == 0x03c0 ) { dumpreg(cpu);}
		if(( cpu->pc >= 0x0090 ) &&( cpu->pc <= 0x009f ) ){ dumpreg(cpu);}
		if ( cpu->pc == 0x0046 ) { dumpreg(cpu); exit(1);}
#endif		
		if (check_chario() & 1) {
			pendc  = next_char();
			/* The TinyBASIC expects break type conditions
			   that persist for a while - this is a fudge */
			if (pendc == 3)
				ns8070_set_a(cpu, 0);
			else
				ns8070_set_a(cpu, 1);
		}
	}
	exit(0);
}
