#ifndef __SVR_MULTI_H__
#define __SVR_MULTI_H__

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

// элемент массива для хранения данных о child процессе
typedef struct child_proc_t {
    pid_t child_pid;	// pid child процесса
    int conn;		// номер сокета клиента
    char finished;	// флаг завершения
    struct child_proc_t *next;	// ссылка на следующий элемент
} child_proc;

// массив для хранения списка child процессов
typedef struct child_procs_t {
    pid_t ppid;	// pid родительского процесса
    char finished;	// флаг завершения
    int count;		// количество элементов списка
    child_proc *child;	// элемент массива
} child_procs;

int svrmulti_listen(char *, int, int);	// главная функция сервера по созданию сокета
void mark_proc_ended();		// обработчик сигнала завершения child процесса
void svrmulti_kill();		// убить родительский процесс
void svrmulti_killall();	// убить все процессы
void svrmulti_exit();		// выход из сервера

int svrmulti_initlist();		// создать список child процессов
int svrmulti_addchild(pid_t, int);	// добавить child процесс в список
int svrmulti_killchild(pid_t);		// убить child процесс
int svrmulti_rmchild(pid_t);		// удалить из списка child процесс
child_proc* svrmulti_getchild(int);	// вытащить ссылку на элемент списка
child_proc* svrmulti_searchchild(pid_t);	// поиск child процесса в списке

#endif	// __SVR_MULTI_H__
