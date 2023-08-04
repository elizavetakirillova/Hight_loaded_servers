#ifndef __CLIENT_H__
#define __CLIENT_H__

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

#define BUF_SIZE	0x1000

typedef struct child_proc_t {
    pid_t pid;
    int conn;
    char finished;
    struct child_proc_t *next;
} child_proc;

typedef struct child_procs_list_t {
    child_proc *head;
    pid_t ppid;
    int count;
} child_procs_list;

void client_killall();
void client_child();
void client_kill();
void client_exit();

// функции для работы со списком
int client_addchild(pid_t,int);
int client_rmchild(pid_t);
child_proc* client_getchild(pid_t);
void client_rmlist();
int client_initlist();

#endif // __CLIENT_H__
