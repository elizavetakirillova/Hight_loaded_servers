#include "svr_proc.h"

// многопроцессный сервер
int svrproc_listen(char *iface, int port, int count)
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
    if (sock < 0) {
	fprintf(stderr, "socket(): error creating socket: %s:%d!\n",
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
    if (bind(sock, (struct sockaddr *)addr, sizeof(*addr)) < 0) {	// привязка к любому интерфейсу
	fprintf(stderr, "bind(): error!\n");
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
    fprintf(stderr, "./svr_proc <interface> <port>\n");
}

// главная функция
int main(int argc, char **argv, char **env)
{
    pid_t pid;
    unsigned int status;
    int port = 0;
    char *iface = NULL;
    int count = 5;	// количество клиентов
    int sock = 0, conn = 0;
    struct sockaddr_in client;
    time_t ticks;
    char *buf = NULL;
    int in_size = 0, out_size = 0;
    int res = 1;
    
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
    // старт сервера
    if ((sock = svrproc_listen(iface, port, count)) < 0) {
	free(iface);
	exit(1);
    }
    memset(&client, 0, sizeof(client));
    buf = (char *)malloc(BUF_SIZE);
    if (!buf) {
	fprintf(stderr, "error alloc mem for buffer!\n");
	close(sock);
	exit(1);
    }
    memset(buf, 0, BUF_SIZE);	// очистка буфера , 4096 байт
    // создание child процесса
    pid = fork();
    if (pid == 0) { // child proc
	pid_t child = getpid();
	fprintf(stderr, "I'm child proc pid=%lu!\n", child);
	while(res) {
	    conn = accept(sock, (struct sockaddr *)&client, (socklen_t *)sizeof(client));
	    fprintf(stderr, "client connected: %s:%d\n", inet_ntoa(client.sin_addr), htons(client.sin_port));
	    if ((in_size = recv(conn, (void *)buf, BUF_SIZE, 0)) > 0) {	// принять данные от клиента
		fprintf(stderr, "recv(): incoming data %s, size: %d bytes, size = %d\n", 
		    buf, strlen(buf), in_size);
		if (strcasecmp(buf, "Hi") == 0) { // получили привет от клиента
		    memset(buf, 0, BUF_SIZE); // очистка буфера
            	    ticks = time(NULL);
            	    snprintf(buf, sizeof(*buf), "%.24s\n", ctime(&ticks));
		    if ((out_size = send(conn, (void *)buf, sizeof(*buf), 0)) > 0) {
			fprintf(stderr, "sent data: %s to client: %s:%d\n",
			    buf, inet_ntoa(client.sin_addr), htons(client.sin_port));
		    }
		} else {	// другие данные пришли от клиента
		    memset(buf, 0, BUF_SIZE);	// очистить буфер данных
		    strcpy(buf, "Bye!\n");
		    if ((out_size = send(conn, (void *)buf, sizeof(*buf), 0)) > 0) {
			fprintf(stderr, "sent data: %s to client: %s:%d\n",
			    buf, inet_ntoa(client.sin_addr), htons(client.sin_port));
		    }
		}
		if (out_size < 0)
		    fprintf(stderr, "send(): error!\n");
		else
		    fprintf(stderr, "send(): %d\n", out_size);
	    }
	    if (in_size < 0)
		fprintf(stderr, "recv(): error!\n");
	    else {
		close(conn);	// закрыть сокет клиента
		memset(&client, 0, sizeof(client));	// очистить данные клиентского подключения
		memset(buf, 0, BUF_SIZE);
	    }
	    res = 0;
	};
    }
    while(wait(&status) > 0) {	// waiting child!
	fprintf(stderr, "I'm parent proc pid=%lu, child exit code=%u\n", pid, status>>8);
    };
    free(buf);
    
    return 0;
}
