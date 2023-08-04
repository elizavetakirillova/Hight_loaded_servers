#ifndef __SERVER_H__
#define __SERVER_H__

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
#include <sched.h>
#include <pthread.h>

#define BUF_SIZE 0x1000

typedef struct thread_params_t {
    int sock;		// fd сокета клиента
    int sock_type;	// протокол TCP/UDP
    struct sockaddr_in *clt_addr;	// адрес клиента
} thread_params;

typedef struct thread_exit_t {
    ssize_t out_size;	// переданные данные в байтах
    ssize_t in_size;	// полученные данные в байтах
} thread_exit;

typedef struct thread_t {
    pthread_t *tid;	// id потока
    thread_params *params;	// параметры переданные потоку
    char finished;	// признак завершения потока
    struct thread_t *next;	// указатель на следующий элемент
} thread;

typedef struct thread_list_t {
    pid_t pid;	// id процесса в системе
    struct thread_t *head;	// список потоков процесса
    int count;
} thread_list;

// инициализация списка потоков
int threadlist_init();
int threadlist_rmall();	// удаление списка потоков
thread* threadlist_add(pthread_t *, thread_params *);
thread* threadlist_get(pthread_t *);	// получить thread_t указатель
int threadslist_rm(pthread_t *);	// удалить из списка поток
// поток сервера
void* server_thread(void *);
void server_exit();	// выход из сервера

#endif	// __SERVER_H__

