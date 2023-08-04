#ifndef __SVR_PROC_H__
#define __SVR_PROC_H__

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <wait.h>
#include <signal.h>
#include <math.h>
#include <string.h>
#include <pwd.h>
#include <errno.h>
#include <time.h>

#define BUF_SIZE 0x1000

int svrproc_listen(char *, int, int);

#endif	// __SVR_PROC_H__
