#include "svr_multi.h"

pid_t ppid = 0;
child_procs *procs_list = NULL;
int svr_sock = 0;

// инициализация списка child процессов
int svrmulti_initlist()
{
    procs_list = (child_procs *)malloc(sizeof(child_procs));
    if (!procs_list) {
	fprintf(stderr, "error alloc mem for list of chuld procs!\n");
	return -1;
    }
    // очистить заголовок списка
    memset(procs_list, 0, sizeof(child_procs));
    procs_list->child = (child_proc *)malloc(sizeof(child_proc)); // выделить память для header
    if (!procs_list->child) {
	fprintf(stderr, "svrmulti_initlist(): error alloc mem for header!\n");
	return -1;
    }
    memset(procs_list->child, 0, sizeof(child_proc));	// очистка памяти
    procs_list->count = 0;
    procs_list->child->next = NULL;	// следующий пустышка
    
    return 0;
}

// удалить из памяти весь список
int svrmulti_rmlist()
{
    child_proc *child = NULL;
    child_proc *prev = NULL;
    
    child = procs_list->child;	// header
    while(child) {
	prev = child;
	child = child->next;	// следующий элемент
	free(prev);
    };
    free(procs_list);
    procs_list = NULL;
    
    return 0;
}

// добавление элемента в список
int svrmulti_addchild(pid_t pid, int sock)
{
    child_proc *child = NULL;
    child_proc *prev = NULL;
    int i = 0;
    
    // header
    child = procs_list->child;
    while(child) {
	prev = child;	// предыдущий элемент
	child = child->next;	// следующий
	if (child == NULL) {	// выделение памяти под новый child процесс
	    child = (child_proc *)malloc(sizeof(child_proc));
	    if (!child) {
		fprintf(stderr, "error alloc mem for child struct!\n");
		return -1;
	    }
	    memset(child, 0, sizeof(child_proc));
	    prev->next = child;
	    child->child_pid = pid;
	    child->finished = 0;
	    child->next = NULL;
	    child->conn = sock;
	    procs_list->count++;	// увеличить количество элементов в списке
	    break;
	}
	i++;
    };
    
    return (i);	// возвращение номера элемента в списке
}

// удалить из списка child процесс
int svrmulti_rmchild(pid_t pid)
{
    child_proc *child = NULL;
    child_proc *prev = NULL;
    child_proc *next = NULL;
    
    child = procs_list->child;
    while(child) {
	prev = child;
	next = procs_list->child->next;
	if (child->child_pid == pid) {
	    prev->next = next->next;	// переместить указатель на следующий элемент
	    free(child);	// освободить память элемента
	    child = NULL;
	    procs_list->count--;
	}
	child = child->next;	// следующий элемент
    };
    free(child);
    child = NULL;
    
    return 0;
}

// шлепнуть child процесс
int svrmulti_killchild(pid_t pid)
{
    kill(pid, SIGTERM);
    
    return 0;
}

// прибить родительский процесс
void svrmulti_kill()
{
    kill(procs_list->ppid, SIGTERM);
    procs_list->finished = 1;
    procs_list->count = 0;
}

// питить все процессы
void svrmulti_killall()
{
    child_proc *child = NULL;
    int i = 0;
    
    child = procs_list->child; // header
    // убить все child процессы в цикле
    for(i = 0;i < procs_list->count;i++) {
	svrmulti_killchild(child->child_pid);
	child->finished = 1;	// признак завершения процесса
	fprintf(stderr, "killall(): child proc pid=%lu killed!\n",
	    child->child_pid);
	child = child->next;	// следующий
    };
    
    close(svr_sock);
    svrmulti_kill();	// родительский прибить
}

// получить элемент списка по его номеру
child_proc* svrmulti_getchild(int num)
{
    child_proc *child = NULL;
    int i = 0;
    
    child = procs_list->child;	// header
    while(child)
    {	// найден элемент списка
	if (i == num) {
	    break;
	}
	i++;
	child = child->next;	// следующий элемент
    };
    fprintf(stderr, "got child proc: %lu, num %d\n",
	child->child_pid, num);
	
    return (child);
}

// поиск child процесса по его pid
child_proc* svrmulti_searchchild(pid_t pid)
{
    child_proc *child = NULL;
    
    child = procs_list->child;	// header
    while(child) {
	if (child->child_pid == pid) {
	    return (child);
	}
	child = child->next;
    };
    
    return NULL;
}

// выход из сервера
void svrmulti_exit()
{
    svrmulti_killall();
    svrmulti_rmlist();
}

// обработчик завершения child proc
void mark_proc_ended()
{
    // получить id процесса завершенного
    pid_t pid = waitpid(-1, NULL, 0);
    child_proc *child = NULL;
    
    if (pid == ppid) {	// родительский процесс
	for(int i = 0;i < sizeof(procs_list->count);i++) {
	    child = svrmulti_getchild(i);
	    pid = child->child_pid;
	    close(child->conn); 	// закрыть клиентский сокет
	    child->conn = -1;		// пустышка, сокет закрыт
	    child->finished = 1;	// признак завершения
	    svrmulti_killchild(pid);	// завершить child процесс
	    fprintf(stderr, "child proc: pid = %ld ended.\n", child->child_pid);
	};
	svrmulti_kill();
    } else {
	child = svrmulti_searchchild(pid);	// поиск этого child процесса
	close(child->conn);	// закрыть сокет
	svrmulti_killchild(pid);	// шлепнуть child процесс
	fprintf(stderr, "child proc: pid = %ld ended.\n", child->child_pid);
	svrmulti_rmchild(pid);	// удалить из списка child процесс
    }
}

// многопроцессный сервер
int svrmulti_listen(char *iface, int port, int count)
{
    int sock = 0;	// номер сокета fd
    struct sockaddr_in	*addr = NULL;
    
    // Получаем UID пользователя
    uid_t uid = getuid();
    // Получаем имя пользователя
    struct passwd * p = getpwuid(uid);
    if (p == NULL) {
	fprintf(stderr, "Error get uid!\n");
	return -1;
    }
    fprintf(stderr, "Username: %s\n", p->pw_name);
    fprintf(stderr, "Listen %s:%d\n", iface, port);
    // по умолчанию создается TCP сокет
    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == -1) {
	fprintf(stderr, "error creating socket: %s:%d!\n",
	    iface, port);
	    return -1;
    }
    addr = (struct sockaddr_in *)malloc(sizeof(struct sockaddr_in));
    if (!addr) {
	fprintf(stderr, "error alloc mem for sockaddr_in!\n");
	close(sock);
	return -1;
    }
    memset(addr, 0, sizeof(*addr));
    addr->sin_family = AF_INET;
    addr->sin_addr.s_addr = htonl(INADDR_ANY);	// или inet_addr(iface)
    addr->sin_port = htons(port);
    if (bind(sock, (struct sockaddr *)addr, sizeof(*addr)) == -1) {	// привязка к любому интерфейсу
	fprintf(stderr, "error bind()!\n");
	free(addr);
	close(sock);
	return -1;
    }
    int res = listen(sock, count);	// count = количество клиентов
    if (res == 0) {
	free(addr);
	return (sock);	// вернуть fd сокета
    }
    free(addr);
    fprintf(stderr, "error listen() %d!\n", res);
    // закрытие сокета
    close(sock);

    return -1;
}

void usage(void)
{
    fprintf(stderr, "./svr_multi <interface> <port>\n");
}

// главная функция
int main(int argc, char **argv, char **env)
{
    pid_t pid;	// pid родительского процесса
    unsigned int status;
    int port = 0, l = 0;
    char *iface = NULL;
    int count = 5;	// количество клиентов
    int sock = 0, conn = 0;
    struct sockaddr_in client;
    time_t ticks;
    char buf[BUF_SIZE];
    int in_size = 0, out_size = 0;
    pid_t child;
    
    fprintf(stderr, "%s hello where!\n", argv[0]);
    if (argc < 3) {
	usage();
	exit(1);
    }
    // параметры командной строки
    iface = (char *)malloc(strlen(argv[1]));
    if (!iface) {
	fprintf(stderr, "error alloc mem!\n");
	return 1;
    }
    strcpy(iface, argv[1]);
    port = atoi(argv[2]);	// конвертация строки в целочисленное
    // вывод переменных окружения
    for(int i=0;i<sizeof(env);i++) {
	fprintf(stderr, "%s\n", env[i]);
    };
    // установка обработчиков сигналов
    signal(SIGINT, svrmulti_killall);
    signal(SIGTERM, svrmulti_exit);
    signal(SIGCHLD, mark_proc_ended);
    if (svrmulti_initlist() < 0) {
	fprintf(stderr, "Fatal error!\n");
	exit(1);
    }
    procs_list->ppid = getpid();	// получить мой pid
    procs_list->finished = 0;
    // старт сервера
    if ((sock = svrmulti_listen(iface, port, count)) < 0) {
	free(iface);
	exit(1);
    }
    svr_sock = sock;
    memset(&client, 0, sizeof(client));
    memset(&buf, 0, BUF_SIZE);	// очистка буфера , 4096 байт
    while(1) {
again:	// ожидание соединения
	conn = accept(sock, (struct sockaddr *)&client, (socklen_t *)sizeof(client));
	if (conn)	// подключился клиент
	    break;
    };
    // создание child процесса
    pid = fork();	// и передача клиента!
    if (pid == 0) { // child proc
	child = getpid();
	fprintf(stderr, "I'm child proc pid=%lu!\n", child);
	fprintf(stderr, "client connected: %s:%d\n", inet_ntoa(client.sin_addr), htons(client.sin_port));
	if ((in_size = recv(conn, (void *)&buf, BUF_SIZE, MSG_WAITALL)) > 0) {	// принять данные от клиента
	    fprintf(stderr, "recv(): incoming data %s, size: %lu bytes\n", 
		buf, in_size);
	    if ((strcasecmp(buf, "Hi") == 0) || (strcasecmp(buf, "Hello") == 0)) { // получили привет от клиента
		memset(&buf, 0, BUF_SIZE); // очистка буфера
		ticks = time(NULL);
		snprintf(buf, sizeof(buf), "%.24s\n", ctime(&ticks));
		if ((out_size = send(conn, (void *)&buf, sizeof(&buf), 0)) > 0) {
		    fprintf(stderr, "sent data: %s to client: %s:%d, %lu bytes\n",
			buf, inet_ntoa(client.sin_addr), htons(client.sin_port), out_size);
		}
	    } else {	// другие данные пришли от клиента
		memset(&buf, 0, BUF_SIZE);	// очистить буфер данных
		strcpy(buf, "Bye\n");
		if ((out_size = send(conn, (void *)&buf, sizeof(&buf), 0)) > 0) {
		    fprintf(stderr, "sent data: %s to client: %s:%d, %lu bytes\n",
			buf, inet_ntoa(client.sin_addr), htons(client.sin_port), out_size);
		}
	    }
	    if ((in_size > 0) || (out_size > 0)) {
		fprintf(stderr, "recv(): %lu bytes\n", in_size);
		fprintf(stderr, "send(): %lu bytes\n", out_size);
	    }
	}
	close(conn);	// закрыть сокет клиента
	memset(&client, 0, sizeof(client));	// очистить данные клиентского подключения
	memset(&buf, 0, BUF_SIZE);
	exit(0);	// выход из child proc
    }
//    while(wait(&status) > 0) {	// waiting child!
    fprintf(stderr, "I'm parent proc pid=%lu, new child pid=%lu\n",
	pid, child);
    if ((l = svrmulti_addchild(child, conn)) != -1) {
	fprintf(stderr, "Added child proc pid=%lu, element=%d\n", child, l);
    }
    goto again;	// идем слушать серверный сокет
//    };
    svrmulti_killall();	// убить все процессы
    svrmulti_rmlist();	// освободить память списка
    
    return 0;
}
