#include "client.h"

pid_t ppid = 0;
child_procs_list *child_list = NULL;
char *iface = NULL;
struct sockaddr_in *svr_addr = NULL;

// инициализация списка child процессов
int client_initlist()
{
    child_list = (child_procs_list *)malloc(sizeof(child_procs_list));
    if (!child_list) {
	fprintf(stderr, "client_initlist(): error alloc mem for struct child_list!\n");
	return -1;
    }
    memset(child_list, 0, sizeof(child_procs_list));
    child_list->head = (child_proc *)malloc(sizeof(child_proc));
    if (!child_list->head) {
	fprintf(stderr, "client_initlist(): error alloc mem for head struct of list!\n");
	free(child_list);
	return -1;
    }
    child_list->count = 0;
    child_list->head->next = NULL;
    
    return 0;
}

// удаление всего списка child процессов из памяти
void client_rmlist()
{
    child_proc *child = NULL;
    child_proc *prev = NULL;
    pid_t pid;
    int i = 0;
    // начало списка header
    child = child_list->head;
    while(child) {
	pid = child->pid;	// child pid
	if (child->finished) {
	    prev = child;
	    child = prev->next;
	    free(child);	// освободить память
	    child = prev;
	} else {
	    fprintf(stderr, "client_rmlist(): error child pid=%d not ended!\n",
		pid);
	}
	i++;
    };
    child_list->count = 0;
    free(child_list);
    child_list = NULL;
}

// добавление элемента в список
int client_addchild(pid_t pid, int conn)
{
    child_proc *child = NULL;
    int i = 0;
    
    child = child_list->head;	// header списка
    while(child) {
	if ((!child->next) && (child_list->count == 0)) {	// NULL! первый элемент
	    child->pid = pid;
	    child->finished = 0;
	    child->conn = conn; 	// fd сокета
	    i++;
	    break;
	} else if (child_list->count > 0) {
	    child = child->next;
	    if (!child) {
		child = (child_proc *)malloc(sizeof(child_proc));
		if (!child) {
		    fprintf(stderr, "client_addchild(): error alloc mem for new element!\n");
		    return -1;
		}
		memset(child, 0, sizeof(child_proc));
		child->next = NULL;
		child->pid = pid;
		child->conn = conn;
		i++;
		break;
	    }
	}
    };
    child_list->count++;
    
    return (i);	// вернуть номер элемента списка
}

// получить указатель на child процесс
child_proc* client_getchild(pid_t pid)
{
    child_proc *child = NULL;
    
    child = child_list->head;
    while(child) {
	if (child->pid == pid) {	// найден child процесс
	    break;
	}
	child = child->next;
    };
    
    return (child);
}

// удалить элемент из списка
int client_rmchild(pid_t pid)
{
    child_proc *child = NULL;
    child_proc *prev = NULL;
    child_proc *next = NULL;

    child = child_list->head;
    while(child) {
	if (child->pid == pid) {	// найден элемент
	    prev = child;
	    next = child->next;
	    prev->next = next;
	    child_list->count--;
	    free(child);
	    return 0;
	}
    };

    return -1;
}

// убить основной процесс
void client_kill()
{
    kill(SIGTERM, child_list->ppid);
}

// убить все процессы
void client_killall()
{
    child_proc *child = NULL;

    child = child_list->head;
    while(child) {
	close(child->conn);
	kill(SIGTERM, child->pid);
	child->finished = 1;
	child = child->next;
    };
    
    client_kill();	// убить родительский процесс
}

// обработчик завершения основного процесса
void client_exit()
{
    client_killall();
    client_rmlist();
    free(iface);
    free(svr_addr);
}

// обработчик сигнала завершения child процесса
void client_child()
{
   // получить id процесса завершенного
    pid_t pid = waitpid(-1, NULL, 0);
    child_proc *child = NULL;

    child = client_getchild(pid);	// получить данные о child процессе
    if (!child) {	// NULL, не найден такой
	fprintf(stderr, "client_child(): not found child proc with pid=%d!\n",
	    pid);
	return;
    }
    child->finished = 1;	// завершен
    fprintf(stderr, "client_child(): child proc pid=%d ended.\n",
	pid);
    close(child->conn);	// закрыть сокет
    client_rmchild(child->pid);
}

void usage(char *name)
{
    fprintf(stderr, "%s <interface> <server_port>\n", name);
    exit(1);
}

// главная функция программы
int main(int argc, char **argv, char **env)
{
    int sock = 0, port = 0;
    char buf[BUF_SIZE];
    pid_t pid, child;
    int count_child = 0;
    ssize_t bytes = 0;
    
    if (argc < 3)
        usage(argv[0]);

    ppid = getpid();	// получить pid parent процесса
    memset(&buf, 0, BUF_SIZE);
    if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
	fprintf(stderr, "error creating socket!\n");
	return 1;
    }
    svr_addr = (struct sockaddr_in *)malloc(sizeof(struct sockaddr_in));
    if (!svr_addr) {
	fprintf(stderr, "error alloc mem for svr_addr struct!\n");
	return 1;
    }
    iface = (char *)malloc(strlen(argv[1])+1);
    if (!iface) {
	fprintf(stderr, "Error alloc mem for iface!\n");
	close(sock);
	free(svr_addr);
	return 1;
    }
    // установка обработчиков сигналов
    signal(SIGINT, client_killall);
    signal(SIGCHLD, client_child);
    signal(SIGTERM, client_exit);
    if (client_initlist() < 0) {
	fprintf(stderr, "Error init list of child procs!\n");
	free(svr_addr);
	free(iface);
	return 1;
    }
    memset(iface, 0, strlen(argv[1]));
    strcpy(iface, argv[1]);
    port = atoi(argv[2]);	// server port
    memset(svr_addr, 0, sizeof(struct sockaddr_in));
    svr_addr->sin_family = AF_INET;
    svr_addr->sin_port = htons(port);
    svr_addr->sin_addr.s_addr = inet_addr(iface);	// конвертация IP адреса
    while(1) {
	// соединение с сервером
again:
	fprintf(stderr, "Connecting to %s:%d...",
	    iface, port);
	if (connect(sock, (struct sockaddr *)svr_addr, sizeof(struct sockaddr)) < 0) {
	    close(sock);
	    fprintf(stderr, "connect() error!\n");
	    free(svr_addr);
	    return 1;
	}
	fprintf(stderr, "connected.\n");
	pid = fork();
	if (pid == 0) {	// child proc
	    child = getpid();
	    fprintf(stderr, "i'm child proc pid=%d\n", child);
	    memset(&buf, 0, BUF_SIZE);	// очистка буфера
	    strcpy(buf, "Hi");	// отправить Hi серверу
	    if (send(sock, (void *)&buf, strlen(buf), MSG_NOSIGNAL) != -1) {
		fprintf(stderr, "sent data %s to server %s:%d...\n",
		    iface, port);
		memset(&buf, 0, BUF_SIZE);
		if ((bytes = recv(sock, (void *)&buf, BUF_SIZE, MSG_WAITALL)) != -1) {
		    fprintf(stderr, "incoming data: %s, size=%lu bytes\n",
			buf, bytes);
		} else {
		    fprintf(stderr, "recv() error!\n");
		}
	    } else {
		fprintf(stderr, "send() error!\n");
	    }
	    close(sock);
	    memset(&buf, 0, BUF_SIZE);
	    exit(0);
	}
    };
    // добавить child процесс в список
    if (client_addchild(child, sock) < 0) {
	fprintf(stderr, "error adding child proc to list!\n");
	free(iface);
	free(svr_addr);
	client_killall();	// убить все child процессы
	client_rmlist();	// очистить память за собой
	return 1;	// выход с ошибкой
    }
    goto again;	// next child proc
    free(iface);
    free(svr_addr);
    client_killall();
    client_rmlist();

    return 0;
}
