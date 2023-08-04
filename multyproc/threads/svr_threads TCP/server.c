#include "server.h"

// список запущенных потоков
thread_list *threads_list = NULL;
struct sockaddr_in svr_addr;	// серверный сокет
int svr_socket = 0;

// инициализация списка потоков
int threadlist_init()
{
    int res = 0;
    
    if (!threads_list) {
	threads_list = (thread_list *)malloc(sizeof(thread_list));
	if (!threads_list) {
	    fprintf(stderr, "threadlist_init(): error alloc mem for threads_list struct!\n");
	    res = -1;
	}
	memset(threads_list, 0, sizeof(thread_list));
	threads_list->count = 0;
	threads_list->pid = getpid();	// мой PID
	threads_list->head = (thread *)malloc(sizeof(thread));
	if (!threads_list->head) {
	    fprintf(stderr, "threadlist_init(): error alloc mem for head!\n");
	    free(threads_list);
	    threads_list = NULL;
	    res = -1;
	}
	threads_list->head->next = NULL;
    }

    return (res);
}

// удаление списка потоков и освобождение памяти
int threadlist_rmall()
{
    int res = 0;
    thread *next = NULL;
    thread *thr = NULL;
    pthread_t *pthr = NULL;
    thread_params *param = NULL;
    
    if (!threads_list) {
	fprintf(stderr, "threadlist_rm(): threads_list is NULL!\n");
	return -1;
    }
    // head списка
    thr = threads_list->head;
    while(thr) {
	//prev = thr;
	next = thr->next;
	close(thr->params->sock);
	free(thr->params->clt_addr);
	free(thr->params);
	free(thr);
	thr = next;
	threads_list->count--;
    };

    return (res);
}

// добавление потока в список
thread* threadlist_add(pthread_t *th, thread_params *param)
{
    thread *thr = NULL;
    thread *next = NULL;
    
    if (threads_list->count == 0) { // первый элемент списка
	threads_list->head->tid = th;
	threads_list->head->params = param;
	threads_list->head->finished = 0;
	threads_list->count++;
	threads_list->head->next = NULL;
	thr = threads_list->head;
    } else {
	thr = threads_list->head;	// первый элемент
	while(thr) {
	    next = thr->next;
	    if (thr->next == NULL) {
		next = (thread *)malloc(sizeof(thread));
		if (!next) {
		    fprintf(stderr, "threadlist_add(): error alloc mem for new thread_t struct!\n");
		    thr = NULL;
		    break;
		}
		memset(thr, 0, sizeof(thread));
		thr->tid = th;
		thr->params = param;
		thr->finished = 0;
		thr->next = NULL;
		threads_list->count++;
		break;
	    }
	    thr = next;	// следующий элемент списка
	};
    }
    
    return (thr);
}

// удаление потока из списка
int threadlist_rm(pthread_t *th)
{
    thread *thr = NULL;
    thread *prev = NULL;
    thread *next = NULL;
    int res = 0;

    if (!th) {
	fprintf(stderr, "threadlist_rm(): th var is NULL!\n");
	return -1;
    }
    // поиск потока th
    thr = threads_list->head;
    while(thr) {
	prev = thr;
	next = thr->next;
	if (thr->tid == th) {	// нашли
	    threads_list->count--;
	    free(thr);
	    res = 0;
	    break;
	} else	// не нашли!
	    res = -1;

	thr = next;	// следующий поток
    };

    return (res);
}

// получить указатель на поток
thread* threadlist_get(pthread_t *th)
{
    thread *thr = NULL;
    thread *next = NULL;
    
    if (!th) {
	fprintf(stderr, "threadlist_get(): th is NULL!\n");
	return NULL;
    }
    // поиск потока
    thr = threads_list->head;
    while(thr) {
	next = thr->next;
	if (thr->tid == th)	// нашли th
	    break;
	thr = next;
    };
    
    return (thr);
}
// новый серверный поток
void* server_thread(void *param)
{
    int res = 1;
    thread_exit *val = NULL;
    int client_sock = 0;
    char *buf = NULL;
    time_t ticks;
    ssize_t in_size = -1;
    ssize_t out_size = -1;
    
    if (!param) {
	fprintf(stderr, "server_thread(): tid = %lx parameters is NULL!\n",
	    pthread_self());
	pthread_exit(NULL);	// выход!
    }
    ticks = time(NULL);
    pthread_t *thr = (pthread_t *)pthread_self();	// мой tid
    thread_params *vars = (thread_params *)param;
    fprintf(stderr, "server_thread(): tid = %lx started.\n", thr);
    fprintf(stderr, "server_thread(): client %s:%d connected.\n",
	inet_ntoa(vars->clt_addr->sin_addr), htons(vars->clt_addr->sin_port));
    client_sock = vars->sock;
    val = (thread_exit *)malloc(sizeof(thread_exit));
    if (!val) {
	fprintf(stderr, "server_thread(): tid = %lx - error alloc mem for return value!\n",
	    thr);
	close(client_sock);	// закрыть клиентский сокет
	pthread_exit(NULL);	// аварийный выход
    }
    memset(val, 0, sizeof(thread_exit));	// очистить
    buf = (char *)malloc(BUF_SIZE);	// выделить память под буфер
    memset(buf, 0, BUF_SIZE);
    // цикл получения и отправки данных
    while(res) {
	switch(vars->sock_type) {	// по протоколу
	    case SOCK_STREAM:{
		// принять данные от клиента
		while((in_size = recv(client_sock, (void *)buf, BUF_SIZE, 0)) > 0) {
		    fprintf(stderr, "recv(): incoming data: %s size: %lu bytes.\n",
			buf, in_size);
		    val->in_size += in_size;	// общее количество принятых байт
		    // отправить ответку
		    if ((strcasecmp(buf, "Hi") == 0) || (strcasecmp(buf, "Hello"))) {
			memset(buf, 0, BUF_SIZE);
			strcpy(buf, ctime(&ticks));	// время
		    } else {
			memset(buf, 0, BUF_SIZE);
			strcpy(buf, "Bye!");
		    }
		    if ((out_size = send(client_sock, (void *)buf, strlen(buf), 0)) > 0) {
			val->out_size += out_size;	// общее количество отправленных байт
			fprintf(stderr, "send(): output data: %s size: %lu bytes.\n",
			buf, out_size);
		    } else {
			fprintf(stderr, "send(): error!\n");
			out_size = -1;
		    }
		};
		
		if (in_size == -1) {	// ошибка приема данных от клиента
		    val->in_size = -1;
		    val->out_size = -1;
		}
		
		if (out_size == -1) {	// ошибка отправки данных
		    val->in_size = -1;
		    val->out_size = -1;
		}
		
		if (val->in_size == -1) {
		    fprintf(stderr, "recv() error!\n");
		    val->in_size = 0;
		    res = 0;
		}
		if (val->out_size == -1) {
		    fprintf(stderr, "send() error!\n");
		    val->out_size = 0;
		    res = 0;
		}
		break;
	    };
	    case SOCK_DGRAM:{
	    };
	    default:{
		fprintf(stderr, "server_thread(): fatal error - unknown proto %d!\n",
		    vars->sock_type);
		res = 0;	// ошибка!
		val->out_size = 0;
		val->in_size = 0;
		break;
	    };
	};
    };

    // ничего не приняли или ничего не отправили
    if ((val->in_size == 0) || (val->out_size == 0)) {
	free(val);
	val = NULL;	// ошибка
	close(client_sock);	// закрыть соединение с клиентом
    }

    pthread_exit(val);	// выход из потока
}

void server_exit()
{
    threadlist_rmall();
}

// вывод инфо о программе
void usage(char *name)
{
    fprintf(stderr, "%s <port> <proto>\n", name);
    fprintf(stderr, "there <proto>: tcp or udp!\n");
    fprintf(stderr, "by default server listen on all net interfaces IPV4!\n");
}

// главная функция
int main(int argc, char **argv, char **env)
{
    thread_exit ret_val;
    thread_params *params = NULL;
    thread *th =  NULL;
    pthread_t *thr = NULL;
    struct sockaddr_in *clt_addr = NULL;	// клиенсткий сокет
    int svr_port = 0;
    int sock_type = 0;
    int res = 1;
    int clt_socket = 0;
    
    if (argc < 3) {
	usage(argv[0]);
	return 1;
    }
    // Ctrl-C
    signal(SIGINT, server_exit);
    // 15 TERMINATE
    signal(SIGTERM, server_exit);

    memset(&ret_val, 0, sizeof(ret_val));
    // очистить память
    memset(&svr_addr, 0, sizeof(svr_addr));
    // номер порта сервера, на котором слушать
    svr_port = atoi(argv[1]);
    if (strcasecmp(argv[2], "tcp") == 0) {
	sock_type = SOCK_STREAM;
    } else if (strcasecmp(argv[3], "udp") == 0) {
	sock_type = SOCK_DGRAM;
    } else {
	fprintf(stderr, "No such %s protocol!\n", argv[2]);
	return 1;
    }
    if ((svr_socket = socket(AF_INET, sock_type, 0)) == -1) {
	fprintf(stderr, "socket() creating error!\n");
	return 1;
    }
    svr_addr.sin_family = AF_INET;
    svr_addr.sin_port = htons(svr_port);
    svr_addr.sin_addr.s_addr = htonl(INADDR_ANY);	// 0.0.0.0 by default
    if (bind(svr_socket, (struct sockaddr *)&svr_addr, sizeof(svr_addr)) < 0) {
	fprintf(stderr, "bind() error!\n");
	return 1;
    }
    if (listen(svr_socket, 5) == -1) {	// слушать сокет, 5 клиентов по умолчанию очередь!
	fprintf(stderr, "listen() error!\n");
	close(svr_socket);
	return 1;
    }
    fprintf(stderr, "Server listen on: 0.0.0.0:%d...\n",
	svr_port);
    // главный цикл сервера
    while(res) {
	clt_addr = (struct sockaddr_in*)malloc(sizeof(struct sockaddr_in));
	if (!clt_addr) {
	    fprintf(stderr, "error alloc mem for struct client_addr!\n");
	    threadlist_rmall();
	    close(svr_socket);
	    res = 0;
	    exit(1);
	}
	memset(clt_addr, 0, sizeof(struct sockaddr_in));
	if ((clt_socket = accept(svr_socket, (struct sockaddr*)clt_addr, (socklen_t *)sizeof(struct sockaddr))) == -1) {
	    fprintf(stderr, "accept() error!\n");
	    threadlist_rmall();
	    close(svr_socket);
	    res = 0;
	    exit(1);
	}
	// выделить память под параметры нового потока!
	params = (thread_params *)malloc(sizeof(thread_params));
	if (!params) {
	    fprintf(stderr, "error alloc mem for params struct!\n");
	    threadlist_rmall();	// удалить список потоков
	    close(clt_socket);	// закрыть все сокеты
	    close(svr_socket);
	    free(clt_addr);
	    res = 0;
	    exit(1);
	}
	// заполняем параметры для нового потока
	params->sock = clt_socket;
	params->sock_type = sock_type;
	params->clt_addr = clt_addr;
	thr = (pthread_t *)malloc(sizeof(pthread_t));
	if (!thr) {
	    fprintf(stderr, "error alloc mem for pthread_t struct!\n");
	    threadlist_rmall();
	    close(clt_socket);
	    close(svr_socket);
	    free(clt_addr);
	    free(params);
	    res = 0;
	    exit(1);
	}
	memset(thr, 0, sizeof(pthread_t));
	// создание и запуск нового потока!
	if (pthread_create(thr, NULL, &server_thread, (void *)params) < 0) {
	    fprintf(stderr, "server_thread(): pthread_create() error!\n");
	    threadlist_rmall();
	    close(clt_socket);
	    close(svr_socket);
	    free(clt_addr);
	    free(params);
	    free(thr);
	    res = 0;
	    exit(1);
	}
	// добавить в список
	th = threadlist_add(thr, params);
	// подключится к созданному потоку, чтобы получить код возврата!
	pthread_join((pthread_t)thr, (void *)&ret_val);
	if ((ret_val.in_size > 0) || (ret_val.out_size > 0)) {
	    fprintf(stderr, "thread: %lx return: in %lu, out %lu bytes.\n",
		ret_val.in_size, ret_val.out_size);
	    if (th) {
		threadlist_rm(thr);	// удалить из списка
		th = NULL;
	    }
	}
    };

    return 0;
}
