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

/* This file implements a transparent channel over which the GDB Remote
 * Serial Debugging protocol is implemented.  This implementation for Linux
 * uses a TCP server on port 2000.
 */

#if defined(_WIN32) || defined(__CYGWIN__)
#   define __USE_MINGW_ANSI_STDIO 1
#   include <winsock2.h>
#   include <windows.h>
#   include <ws2tcpip.h>
#else
#   include <sys/socket.h>
#   include <netinet/in.h>
#   include <netinet/tcp.h>
#   include <sys/select.h>
#   include <fcntl.h>
#endif

#include "general.h"
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <unistd.h>

#include "gdb_if.h"

enum
{
	MAX_SOCKET_RETRIES	= 4,
	SIDEKICK_PORT		= 4000,
	DEFAULT_PORT		= 2000,
};


pthread_mutex_t pandora_box = PTHREAD_MUTEX_INITIALIZER;
static struct
{
	pthread_t thread;
	pthread_attr_t attr;
}
sidekick_data;


static int socket_accept(int listening_socket_fd)
{
	int socket_fd;
#if defined(_WIN32) || defined(__CYGWIN__)
	int iResult;
	unsigned long opt;
#else
	int flags;
#endif
#if defined(_WIN32) || defined(__CYGWIN__)
	opt = 1;
	iResult = ioctlsocket(listening_socket_fd, FIONBIO, &opt);
	if (iResult != NO_ERROR) {
		DEBUG_WARN("ioctlsocket failed with error: %ld\n", iResult);
	}
#else
	flags = fcntl(listening_socket_fd, F_GETFL);
	fcntl(listening_socket_fd, F_SETFL, flags | O_NONBLOCK);
#endif
	while(1) {
		socket_fd = accept(listening_socket_fd, NULL, NULL);
		if (socket_fd == -1) {
#if defined(_WIN32) || defined(__CYGWIN__)
			if (WSAGetLastError() == WSAEWOULDBLOCK) {
#else
			if (errno == EWOULDBLOCK) {
#endif
				SET_IDLE_STATE(1);
				platform_delay(100);
			} else {
#if defined(_WIN32) || defined(__CYGWIN__)
				DEBUG_WARN("error when accepting connection: %d",
						   WSAGetLastError());
#else
				DEBUG_WARN("error when accepting connection: %s",
						   strerror(errno));
#endif
				exit(1);
			}
		} else {
#if defined(_WIN32) || defined(__CYGWIN__)
			opt = 0;
			ioctlsocket(listening_socket_fd, FIONBIO, &opt);
#else
			fcntl(listening_socket_fd, F_SETFL, flags);
#endif
			break;
		}
	}
	DEBUG_INFO("Got connection\n");
#if defined(_WIN32) || defined(__CYGWIN__)
	opt = 0;
	ioctlsocket(socket_fd, FIONBIO, &opt);
#else
	flags = fcntl(socket_fd, F_GETFL);
	fcntl(socket_fd, F_SETFL, flags & ~O_NONBLOCK);
#endif
	return socket_fd;
}

static int create_socket(int port, int * socket_fd)
{
	struct sockaddr_in addr;
	int opt, tries = 0;

	do {
		port ++;
		tries ++;
		if (tries > MAX_SOCKET_RETRIES)
			return - 1;
		addr.sin_family = AF_INET;
		addr.sin_port = htons(port);
		addr.sin_addr.s_addr = htonl(INADDR_ANY);

		* socket_fd = socket(PF_INET, SOCK_STREAM, 0);
		if (* socket_fd == -1) {
			DEBUG_WARN("PF_INET %d\n",* socket_fd);
			continue;
		}

		opt = 1;
		if (setsockopt(* socket_fd, SOL_SOCKET, SO_REUSEADDR, (void*)&opt, sizeof(opt)) == -1) {
#if defined(_WIN32) || defined(__CYGWIN__)
		    DEBUG_WARN("error setsockopt SOL_SOCKET : %d error: %d\n", * socket_fd,
			WSAGetLastError());
#else
			DEBUG_WARN("error setsockopt SOL_SOCKET : %d error: %d\n", * socket_fd,
			strerror(errno));
#endif
			close(* socket_fd);
			continue;
		}
		if (setsockopt(* socket_fd, IPPROTO_TCP, TCP_NODELAY, (void*)&opt, sizeof(opt)) == -1) {
#if defined(_WIN32) || defined(__CYGWIN__)
			DEBUG_WARN("error setsockopt IPPROTO_TCP : %d error: %d\n", * socket_fd,
			WSAGetLastError());
#else
			DEBUG_WARN("error setsockopt IPPROTO_TCP : %d error: %d\n", * socket_fd,
			strerror(errno));
#endif
			close(* socket_fd);
			continue;
		}
		if (bind(* socket_fd, (void*)&addr, sizeof(addr)) == -1) {
#if defined(_WIN32) || defined(__CYGWIN__)
			DEBUG_WARN("error when binding socket: %d error: %d\n", * socket_fd,
			WSAGetLastError());
#else
			DEBUG_WARN("error when binding socket: %d error: %d\n", * socket_fd,
			strerror(errno));
#endif
			close(* socket_fd);
			continue;
		}
		if (listen(* socket_fd, 1) == -1) {
			DEBUG_WARN("listen closed %d\n",* socket_fd);
			close(* socket_fd);
			continue;
		}
		break;
	} while(1);
	return port;
}

static void * sidekick(void * param)
{
int server_socket_fd, connection_socket_fd;

	(void) param;
	int t = create_socket(SIDEKICK_PORT - 1, & server_socket_fd);
	DEBUG_WARN("sidekick listening on port %i\n", t);
	fflush(stderr);

	while (1)
	{
		connection_socket_fd = socket_accept(server_socket_fd);
		(void) connection_socket_fd;
		DEBUG_WARN("sidekick got a connection\n");
		char buf[512];
		target_lock();
		int i = snprintf(buf, sizeof buf, "bmp connection, target at 0x%p, shutting down\n", cur_target);
		target_unlock();
		fflush(stderr);
		send(connection_socket_fd, buf, i, 0);
		shutdown(connection_socket_fd, /* SHUT_RDWR */ 2);
		close(connection_socket_fd);
	}
	shutdown(server_socket_fd, /* SHUT_RDWR */ 2);
	close(server_socket_fd);

	DEBUG_WARN("sidekick thread bailing out\n");
	fflush(stderr);
	return 0;
}

static int gdb_if_serv, gdb_if_conn;

int gdb_if_init(void)
{
#if defined(_WIN32) || defined(__CYGWIN__)
	int iResult;
	WSADATA wsaData;
	iResult =  WSAStartup(MAKEWORD(2, 2), &wsaData);
	if (iResult != NO_ERROR) {
		DEBUG_WARN("WSAStartup failed with error: %ld\n", iResult);
		exit(1);
	}
#endif
	/* Deploy sidekick thread. */
	if (pthread_attr_init(&sidekick_data.attr))
	{
		DEBUG_WARN("pthread_attr_init() error\n");
		exit(1);
	}
	if (pthread_create(&sidekick_data.thread, &sidekick_data.attr, sidekick, 0))
	{
		DEBUG_WARN("pthread_create() error\n");
		exit(1);
	}
	int t = create_socket(DEFAULT_PORT - 1, & gdb_if_serv);
	DEBUG_WARN("Listening on TCP: %4d\n", t);

	return 0;
}


unsigned char gdb_if_getchar(void)
{
	unsigned char ret;
	int i = 0;
	while(i <= 0) {
		if(gdb_if_conn <= 0)
			gdb_if_conn = socket_accept(gdb_if_serv);
		i = recv(gdb_if_conn, (void*)&ret, 1, 0);
		if(i <= 0) {
			gdb_if_conn = -1;
#if defined(_WIN32) || defined(__CYGWIN__)
			DEBUG_INFO("Dropped broken connection: %d\n", WSAGetLastError());
#else
			DEBUG_INFO("Dropped broken connection: %s\n", strerror(errno));
#endif
			/* Return '+' in case we were waiting for an ACK */
			return '+';
		}
	}
	return ret;
}

unsigned char gdb_if_getchar_to(int timeout)
{
	fd_set fds;
# if defined(__CYGWIN__)
        TIMEVAL tv;
#else
	struct timeval tv;
#endif

	if(gdb_if_conn == -1) return -1;

	tv.tv_sec = timeout / 1000;
	tv.tv_usec = (timeout % 1000) * 1000;

	FD_ZERO(&fds);
	FD_SET(gdb_if_conn, &fds);

	if(select(gdb_if_conn+1, &fds, NULL, NULL, &tv) > 0)
		return gdb_if_getchar();

	return -1;
}

void gdb_if_putchar(unsigned char c, int flush)
{
#if defined(__WIN32__) || defined(__CYGWIN__)
	static char buf[2048];
#else
	static uint8_t buf[2048];
#endif
	static int bufsize = 0;
	if (gdb_if_conn > 0) {
		buf[bufsize++] = c;
		if (flush || (bufsize == sizeof(buf))) {
			send(gdb_if_conn, buf, bufsize, 0);
			bufsize = 0;
		}
	}
}
