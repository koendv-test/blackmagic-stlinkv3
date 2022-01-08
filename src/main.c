/*
 * This file is part of the Black Magic Debug project.
 *
 * Copyright (C) 2011  Black Sphere Technologies Ltd.
 * Written by Gareth McMullin <gareth@blacksphere.co.nz>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/* Provides main entry point.  Initialise subsystems and enter GDB
 * protocol loop.
 */

#include "general.h"
#include "gdb_if.h"
#include "gdb_main.h"
#include "target.h"
#include "exception.h"
#include "gdb_packet.h"
#include "morse.h"

#include <sforth/engine.h>
#include <sforth/sf-arch.h>


int sfgetc(void) { return gdb_if_getchar_blocking(); }
int sffgetc(cell file_id) { return EOF; }
int sfputc(int c) { bool flag = (c == '\n') ? true : false; gdb_if_putchar_blocking(c, true); }
int sfsync(void) { return EOF; }
cell sfopen(const char * pathname, int flags) { return 0; }
int sfclose(cell file_id) { return EOF; }
int sffseek(cell stream, long offset) { return -1; }


void hw_init(void);

int
main(int argc, char **argv)
{
#if PC_HOSTED == 1
	platform_init(argc, argv);
#else
	(void) argc;
	(void) argv;
	platform_init();
#endif

	sf_reset();
	hw_init();
	while (1)
	{
		//gdb_if_putchar_blocking(gdb_if_getchar_blocking() + 1, true);
		do_quit();
	}

	while (true) {
		volatile struct exception e;
		TRY_CATCH(e, EXCEPTION_ALL) {
			gdb_main();
		}
		if (e.type) {
			gdb_putpacketz("EFF");
			target_list_free();
			morse("TARGET LOST.", 1);
		}
	}

	/* Should never get here */
	return 0;
}

