/* Многопроцессный клиент-сервер
 * При запуске случающий (главный) сервер порождает пул заранее готовых 
 * процессов/потоков с обслуживающими серверами.
 * Слушающий сервер следит за пулом обслуживающих серверов
 * и при подключении клиента идентификатор свободного сервера возвращает
 * клиенту, клиент подключается к нему.
 * При количестве клиентов больше количества обслуживающих серверов
 * создается новая порция обслуживающих серверов.
 * Если количество клиентов уменьшается, лишние процессы уничтожаются.
 * Для уведомления слушающего сервера можно использовать массив
 * (только для потоков - слушающий сервер будет искать в массиве 0 и 
 * перераспределять на него нагрузку), канал, очереди сообщений, сигналы.
 * Реализовать для TCP или UDP. */

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <pthread.h>
#include <limits.h>
#include <errno.h>
#include <err.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define DEFAULT_MODE 0 // режим работы программы по умолчанию: >0 клиент<, 1 сервер
#define DEFAULT_SERVER_IP INADDR_LOOPBACK // IP адрес сервера по умолчанию (127.0.0.1)
#define DEFAULT_PORT_SERVER 8000 // номер порта сервера по умолчанию (8000)
#define DEFAULT_PORT_CLIENT 8001 // номер порта клиента по умолчанию (8001)
// N.B. для использования порта нужно ввести команду ufw allow 8888

// параметры сервера
#define SERVICE_ID_NOT_DEFINED USHRT_MAX
#define DEFAULT_SERVICE_MESSAGING_KEY 12345 // значение ключа для создания очереди для взаимодействия сервера и сервисов
#define DEFAULT_POOL_SIZE 10 // размер пула сервисов по умолчанию (10 сервисов для обслуживания клиентов)

// параметры клиента
#define RECEIVE_MESSAGE_MAX_ATTEMPTS 5 // количество запросов на получение сообщения до экстренного выхода из чата

// параметры UDP сообщения
#define UDP_HEADER_SIZE 8 // стандартный размер заголовка UPD-пакета в байтах
#define MESSAGE_DATA_LENGTH 20 // размер данных сообщения в байтах (sizeof char = 1 байт)
#define MESSAGE_FULL_LENGTH MESSAGE_DATA_LENGTH + 8 // полная длина сообщения в байтах (sizeof unsigned short = 2 байта)
#define UDP_MESSAGE_LENGTH UDP_HEADER_SIZE +  MESSAGE_FULL_LENGTH // полная длина UDP-сообщения пересылаемого по сети
#define IP_PLUS_UDP_SIZE 20 + 8
#define RAW_MESSAGE_LEN IP_PLUS_UDP_SIZE + MESSAGE_FULL_LENGTH

// типы сообщения
#define NO_MESSAGE 0
#define MESSAGE_CONNECT 1
#define MESSAGE_CLOSE 2
#define MESSAGE_DATA 3

// состояния сообщения
#define MESSAGE_PREPARED 0
#define MESSAGE_RECEIVED 1
#define MESSAGE_SENT 2
#define MESSAGE_ERROR 3

// состояния сервиса
#define SERVICE_AVAILABLE 1
#define SERVICE_BUSY 2
#define SERVICE_ERROR 3

////////////////////////////////////////////////////////////////////////////////
// СТРУКТУРЫ ИСПОЛЬЗУЕМЫЕ ВНУТРИ СЕРВЕРА (ЛОКАЛЬНО)
////////////////////////////////////////////////////////////////////////////////

// структура сервиса
struct service {
    unsigned short id; // id сервиса
    pthread_t thread; // поток POSIX
    unsigned short state; // состояние сервиса (свободен, доступен, занят или не инициализирован)
    struct sockaddr_in client_address; // адрес клиента назначенного сервером
};

struct service_message_data {
	unsigned short state;
	char data[MESSAGE_DATA_LENGTH];
};

// сообщение сервиса
struct service_message {
	long type;
	struct service_message_data data;
};

////////////////////////////////////////////////////////////////////////////////
// СТРУКТУРЫ КОТОРЫЕ ИСПОЛЬЗУЮТСЯ ДЛЯ ПЕРЕДАЧИ ДАННЫХ ПО СЕТИ (КЛИЕНТ-СЕРВЕР)
////////////////////////////////////////////////////////////////////////////////

// структура UDP-заголовка
struct udp_header {
	unsigned short source_port;  /* порт источника */
	unsigned short dest_port;    /* порт назначения */
	unsigned short packet_len;   /* длина UDP-пакета = 8 байт (заголовок) + MESSAGE_FULL_LENGTH (сообщение) */
	unsigned short check_sum;    /* чек-сумма для проверки целостности пакета = 0 (мы не проверяем)*/
};

// структура сообщения
struct message {
	unsigned short sid; // id привязанного сервиса
	unsigned short mid; // id сообщения
	unsigned short type; // тип сообщения (0 запрос подключения, 1 запрос отключения, 2 передача данных)
	unsigned short state; // состояние сообщения (0 подготовлено, 1 получено, 2 отправлено, 3 ошибка)
	char data[MESSAGE_DATA_LENGTH]; // данные сообщения
};

// структура UDP-сообщения
struct udp_message {
	struct udp_header udph;
	struct message msg;
};

// структура сообщения с адресом отправителя
struct message_ext {
	struct sockaddr_in addr;
	struct message msg;
};

struct message_raw {
	char buf[IP_PLUS_UDP_SIZE];
	struct message msg;
};

////////////////////////////////////////////////////////////////////////////////

// флаги управления работой программы
static volatile int program_exit_flag = 0; // флаг обработки выхода из программы
static volatile int verbose = 0; // флаг управления выводом служебных сообщений

////////////////////////////////////////////////////////////////////////////////

/* Функции обмена сообщений с сервисами */

 // отправка сообщения в очередь
unsigned short send_message_to_queue(int queue_id, int receiver_id, char* message_data) {
	// создаем инстанс сообщения
	struct service_message message;
	
	// заполняем данные сообщения	
	message.type = receiver_id; // указываем тип сообщения
	memset(message.data.data, '\0', sizeof(message.data.data));
	strcpy(message.data.data, message_data);
	message.data.state = MESSAGE_PREPARED; // указываем состояние сообщения
	
	// пытаемся отправить сообщение в очередь, проверяем ошибку
	if (msgsnd(queue_id, &message, sizeof(message.data), IPC_NOWAIT) == -1) {
    	// показываем ошибку, выходим из программы с кодом завершения с ошибкой
    	perror("msgsnd error");
    	// устанавливаем флаг ошибки на состояние
    	message.data.state = MESSAGE_ERROR;
    } else {
    	// устанавливаем флаг успешной отправки сообщения
    	message.data.state = MESSAGE_SENT;
   }
   return message.data.state;
 };

// получение сообщения из очереди
struct service_message receive_message_from_queue(int queue_id, int receiver_id) {
	// создаем инстанс структуры сообщения
	struct service_message message;
	// пытаемся получить сообщение из очереди в наш инстанс, проверяем ошибки
	
	if (msgrcv(queue_id, (void *) &message, sizeof(message.data), receiver_id, MSG_NOERROR | IPC_NOWAIT) == -1) {
		// это не ошибка отсуствия сообщения?
		if (errno != ENOMSG) {
		// нет это другая ошибка
			perror("msgrcv error");
     	} else {
       		// сообщения нет в очереди
       		message.data.state = NO_MESSAGE;
     	}
     	// устанавливаем флаг ошибки на состояние
		message.data.state = MESSAGE_ERROR;
		} else {
			// сообщение успешно получено из очереди
			message.data.state = MESSAGE_RECEIVED;
	}
	return message;
 };

/* Сетевые функции для коммуникации клиента и сервера */

// отправка raw UDP сообщения в сокет
int send_message(int socket_fd, struct sockaddr_in address, unsigned short port_sender, unsigned short port_receiver, unsigned short sid, unsigned short mid, unsigned short type, char* message_data){
	struct udp_message udp_message;
	
	// формируем UPD заголовок сообщения
	udp_message.udph.source_port=htons(port_sender); // Порт отправителя
	udp_message.udph.dest_port=htons(port_receiver); // Порт получателя
	udp_message.udph.packet_len=htons(UDP_MESSAGE_LENGTH); // Длина пакета
	udp_message.udph.check_sum=0; // Контрольная сумма (не проверяем)

	// подготавливаем данные к отправке
	memset(&udp_message.msg.data, '\0', MESSAGE_DATA_LENGTH);
	strcpy(udp_message.msg.data, message_data);
	udp_message.msg.type = type;
	udp_message.msg.sid = sid;
	udp_message.msg.mid = mid;
	udp_message.msg.state = MESSAGE_PREPARED;

	// printf("SEND___\n");
	// printf("%s\n", udp_message.msg.data);
	// printf("_______\n");

	/* Копируем в сокет содержимое буфера запроса */
	if (sendto(socket_fd, &udp_message, UDP_MESSAGE_LENGTH, 0, (struct sockaddr*) &address, sizeof(struct sockaddr_in)) == -1) {
		udp_message.msg.state = MESSAGE_ERROR;
		perror("sendto");
	} else {
		udp_message.msg.state = MESSAGE_SENT;
	}
	return udp_message.msg.state;
}
	
// получение сообщения из сокета вместе с адресом отправителя (DATAGRAM)
struct message_ext receive_message(int socket_fd) {
	struct message_ext message_ext;
	socklen_t addr_len = sizeof(struct sockaddr_in);
	ssize_t number_of_bytes = recvfrom(socket_fd, &message_ext.msg, MESSAGE_FULL_LENGTH, 0, (struct sockaddr*) &message_ext.addr, &addr_len);
		
	// printf("RECIVE___\n");
	// printf("%s\n", message_ext.msg.data);
	// printf("_________\n");
		
	if (number_of_bytes == -1) {
		message_ext.msg.state = MESSAGE_ERROR;
		perror("recvfrom");
	} else {
		message_ext.msg.state = MESSAGE_RECEIVED;
	}
	return message_ext;
}

// отправка сообщения в сокет по адресу (DATAGRAM)
int send_message_reply(int socket_fd, struct sockaddr_in address, unsigned short sid, unsigned short mid, unsigned int type, char* message_data) {
	struct message message;
	socklen_t addr_len = sizeof(struct sockaddr_in);
	
	// подготавливаем сообщение для отправки в сокет
	memset(&message.data, '\0', MESSAGE_DATA_LENGTH);
	strcpy(message.data, message_data);
	message.type = type;
	message.sid = sid;
	message.mid = mid;
	message.state = MESSAGE_PREPARED;

	// printf("REPLY___\n");
	// printf("%s\n", message.data);
	// printf("________\n");

	if (sendto(socket_fd, &message, MESSAGE_FULL_LENGTH, 0, (struct sockaddr*) &address, addr_len) == -1) {
		message.state = MESSAGE_ERROR;
		perror("sendto");
	} else {
		message.state = MESSAGE_SENT;
	}
	return message.state; // возврат состояния сообщения (2 отправлено, -1 ошибка)
}

// получение raw UDP сообщения из сокета
struct message receive_message_reply(int socket_fd, struct sockaddr_in address) {
	struct message_raw message_raw;
	memset(&message_raw.msg.data, '\0', MESSAGE_DATA_LENGTH);
	socklen_t addr_len = sizeof(struct sockaddr_in);
	
	ssize_t number_of_bytes = recvfrom(socket_fd, &message_raw, RAW_MESSAGE_LEN , 0, (struct sockaddr*) &address, &addr_len);
	
	// printf("ANSWER___\n");
	// printf("%s\n", message_raw.msg.data);
	// printf("_________\n");
	
	if (number_of_bytes == -1) {
		message_raw.msg.state = MESSAGE_ERROR;
		perror("recvfrom");
	} else {
		message_raw.msg.state = MESSAGE_RECEIVED;
	}
	return message_raw.msg;
}

// функция предотвращающая считывание не релевантных сообщений из сокета (то есть считвание идет только по сообщениям с подходящими sid и mid)
struct message receive_message_reply_safe(int so2cket_fd, struct sockaddr_in address, unsigned short sid, unsigned short mid) {
	struct message message;
	int attempts = 0;
	
	while(attempts < RECEIVE_MESSAGE_MAX_ATTEMPTS) {
		message = receive_message_reply(socket_fd, address);
		if ((sid == SERVICE_ID_NOT_DEFINED || message.sid == sid) && message.mid == mid) {
			return message;
    	}
    	attempts++;
    	usleep(500);
    }
    message.state = MESSAGE_ERROR;
    return message;
}

/* Функция работы сервиса
 * сервис просто читает сообщения из очереди и отправляет с нужным приоритетом для считывания сервером
 * сервер определяет по приоритету какому клиенту нужно отправить ответное сообщение */
static void* service_fun(void* service_ptr) {
	struct service* service = (struct service* ) service_ptr;
	int service_messaging_queue_id;
	
	// получение идентификатора очереди сообщений для коммуникации с сервером
	service_messaging_queue_id = msgget(DEFAULT_SERVICE_MESSAGING_KEY, 0);
	
	if (service_messaging_queue_id == -1) {
		perror("msgget error");
		exit(EXIT_FAILURE);
	}
	
	while(!program_exit_flag) {
		struct service_message message = receive_message_from_queue(service_messaging_queue_id, service->id + 1);
		if (message.data.state == MESSAGE_RECEIVED) {
			printf("[service service_id = %d get message: %s]\n", service->id, message.data.data);
        	fflush(stdout);
        	message.data.data[0] = 'Z';
        	// реагируем на сообщение и отправляем дополненное сообщение клиенту
        	int state = send_message_to_queue(service_messaging_queue_id, USHRT_MAX/2 - (service->id + 1), &message.data.data);
        	if (state == MESSAGE_SENT) {
        		printf("[service service_id = %d message sent!]\n", service->id);
        		fflush(stdout);
        	}
        }
	}
};

// функция работы сервера, создающего пул обслуживающих сервисов
void run_server(unsigned int server_ip, unsigned short port_server) {
	// Содаем слушающий сервер
	int socket_fd;
	struct sockaddr_in server_address;
	int service_messaging_queue_id;
	struct service* services_pool;
	int services_pool_size = DEFAULT_POOL_SIZE;
	char message_data[MESSAGE_DATA_LENGTH];
	int state;
	
	// получаем файл-дескриптор сокета
	socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (socket_fd == -1)
		perror("socket");
	
	// инициализируем адрес слушающего сервера
	memset(&server_address, 0, sizeof(struct sockaddr_in));
	server_address.sin_family = AF_INET;
	server_address.sin_port = htons(port_server);
	server_address.sin_addr.s_addr = htonl(server_ip);
	
	// подключаем сокет
	if (bind(socket_fd, (struct sockaddr *) &server_address, sizeof(struct sockaddr_in)) == -1)
		perror("bind");
		
	// создание очереди сообщений для взаимодействия с сервисами
	service_messaging_queue_id = msgget(DEFAULT_SERVICE_MESSAGING_KEY, IPC_EXCL | IPC_CREAT | 0600);
	if (service_messaging_queue_id == -1) {
		perror("msgget error");
		exit(EXIT_FAILURE);
	}
	
	// При запуске случающий (главный) сервер порождает пул заранее готовых процессов/потоков
	// инициализируем и запускаем пул потоков с работающими сервисами
	services_pool = malloc(services_pool_size * sizeof(struct service));
	for(int i = 0; i < services_pool_size; i++) {
		// инициализируем id сервиса
		services_pool[i].id = i;
		// создаем и запускаем отдельные потоки для сервисов
		int result_state = pthread_create(&(services_pool[i].thread), NULL, &service_fun, &services_pool[i]);
		// проверяем инициализацию потока
		if(result_state == 0) {
			services_pool[i].state = SERVICE_AVAILABLE; // устанавливаем состояние сервиса
		} else if (result_state != 0) {
			services_pool[i].state = SERVICE_ERROR;
		}
	}
	
	// Слушающий сервер следит за пулом обслуживающих серверов
	while(!program_exit_flag) {
    	// получаем сообщение от клиента
    	struct message_ext message_ext = receive_message(socket_fd);
    
	    printf("[server receive message from client: %s (type %hu)]\n", message_ext.msg.data, message_ext.msg.type);
	    fflush(stdout);

	    // производим обработку сообщения
	    if (message_ext.msg.type == MESSAGE_CONNECT) {
	    	// запрос на установ соединения
	    	printf("[server client connect request]\n");
	    	fflush(stdout);
	    	
	    	// ищем свободный сервис
	    	int available_service_id = -1;
	    	for(int i = 0; i < services_pool_size; i++) {
	    		if(services_pool[i].state == SERVICE_AVAILABLE) {
	    			available_service_id = i;
	    			break;
	    		}
	    	}
			
			// свободный сервис не найден?
			if (available_service_id == -1) {
        		// TODO нужно добавить и запустить некоторое количество сервисов, а затем снова получить available_service_id
        		// увеличиваем размер пула на DEFAULT_POOL_SIZE
        		int new_services_pool_size = services_pool_size + DEFAULT_POOL_SIZE;
        		// перераспределяем область памяти на новый размер пула
        		struct service* ptr = realloc(services_pool, services_pool_size * sizeof(struct service));
        		// убеждаемся что память успешно была перераспределена
        		if (ptr != NULL) {
        			services_pool = ptr;
        			services_pool_size = new_services_pool_size;
        			
        			// инициализируем новые сервисы
        			for(int i = services_pool_size - DEFAULT_POOL_SIZE; i < services_pool_size; i++) {
        				// инициализируем id сервиса
        				services_pool[i].id = i;
        				// создаем и запускаем отдельные потоки для сервисов
        				int result_state = pthread_create(&(services_pool[i].thread), NULL, &service_fun, &services_pool[i]);
        				// проверяем инициализацию потока
        				if(result_state == 0) {
        					services_pool[i].state = SERVICE_AVAILABLE; // устанавливаем состояние сервиса
        				} else if (result_state != 0) {
        					services_pool[i].state = SERVICE_ERROR;
        				}
        			}
        		
				// сервисы инициализированы, теперь можем снова получить наш available_service_id
				available_service_id = services_pool_size - DEFAULT_POOL_SIZE;
				} else {
					// если произошла ошибка, то можно ее обработать
					printf("[server increase pool errror!]\n");
					break;
				}
			}
		
			if (available_service_id != -1) {
				printf("[server available service id = %d]\n", available_service_id);
				fflush(stdout);

        		// назначаем client_address сервису (
       			// В идеале каждый сервис должен работать на своем порту,
        		// а слушающий сервер должен раздавать команды на подключение и отключение клиентов
        		// но здесь мы просто организуем работу сервисов через IPC очереди по заданию
        		struct service* service = &services_pool[available_service_id];
        		service->state = SERVICE_BUSY; // обновляем состояние сервиса

        		// копируем значения адреса (не обязательно)
        		service->client_address = message_ext.addr;

        		// получаем ip и порт клиента (только для справки)
        		unsigned int client_ip = ntohl(service->client_address.sin_addr.s_addr);
        		unsigned short port_client = ntohs(service->client_address.sin_port);
        		printf("[server client port: %hu]\n", port_client);
        		printf("[server client ip: %u]\n", client_ip);

        		// и при подключении клиента идентификатор свободного сервера возвращает клиенту (c)
        		// формируем сообщение для клиента содержащий id сервиса
      			memset(&message_data, '\0', MESSAGE_DATA_LENGTH);
        		strcpy(message_data, "Ok!");
        		printf("[server prepare message reply: %s (service_id = %d)]\n", message_data, available_service_id);

        		// получаем адрес клиента
        		struct sockaddr_in client_address;
        		memset(&client_address, 0, sizeof(struct sockaddr_in));
        		client_address.sin_family = AF_INET;
        		client_address.sin_port = htons(port_client);
        		client_address.sin_addr.s_addr = htonl(client_ip);

        		// отправляем сообщение в сокет по адресу клиента
        		state = send_message_reply(socket_fd, client_address, available_service_id, message_ext.msg.mid + 1, MESSAGE_CONNECT, message_data);
        	
        		if (state == MESSAGE_SENT) {
        			printf("[server client message reply sent]\n");
        		} else {
        			printf("[server client message reply sent error!]\n");
        		}
			}
		} else if (message_ext.msg.type == MESSAGE_DATA) {
			// пришли данные от клиента, нужно оповестить назначенный сервис
		  	int service_id = message_ext.msg.sid;
		  	// отправляем сообщение в очередь указывая service_id
			state = send_message_to_queue(service_messaging_queue_id, service_id + 1, message_ext.msg.data);
			// если сообщение отправлено, то ожидаем ответного сообщения от потока
			if (state == MESSAGE_SENT) {
				printf("[server message sent to service: %s (service_id = %d)]\n", message_ext.msg.data, service_id);
				sleep(1); // небольшая задержка чтобы сообщение успел прочитать сервис и отправить ответное сообщение
				// запрашиваем сообщение из очереди
				struct service_message message = receive_message_from_queue(service_messaging_queue_id, USHRT_MAX/2 - (service_id + 1)); // здесь client_id определяется как LONG_MAX - service_id
				if (message.data.state == MESSAGE_RECEIVED) {
					/// если сообщение получено
					printf("[server message received from service: %s (service_id = %d)]\n", message.data.data, service_id);
					// отправляем сообщение в сокет по адресу клиента
					state = send_message_reply(socket_fd, message_ext.addr, service_id, message_ext.msg.mid + 1, MESSAGE_DATA, message.data.data);
					if (state == MESSAGE_SENT) {
						printf("[server client message reply sent]\n");
					} else {
						printf("[server client message reply error!]\n");
					}
					else if (message.data.state == MESSAGE_ERROR) {
						printf("[server client message recive error (service_id = %d)]\n", service_id);
						// здесь можно обработать ошибку получения сообщения из очереди
					} else { // NO_MESSAGE
						// может оказаться так, что поток еще не успел записать данные в очередь, тогда мы потеряем информацию об ответе сервиса
						// чтобы недопусить этого, нужно гарантировать получение состояния сообщения MESSAGE_RECEIVED
					}
				} else {
					// сообщение не отправлено в очередь
					printf("[server message send error (service_id = %d)]\n", service_id);
					// здесь можно заново перепослать сообщение в очередь если случилась ошибка
					// или придумать как гарантировать 100% отправку сообщения в очередь
				}
			} else if (message_ext.msg.type == MESSAGE_CLOSE) {
				// запрос на закрытие соединения
				printf("[server receive client close mesage: %s]\n", message_ext.msg.data);
				fflush(stdout);
				// если клиент завершил работу, нужно оповестить сервис об этом
				int service_id = message_ext.msg.sid;
				services_pool[service_id].state = SERVICE_AVAILABLE;
				// как только сервис освободился, мы проверяем, нужно ли уменьшить размер пула
				if (services_pool_size > DEFAULT_POOL_SIZE) {
					int decrease_condition = 1;
					for(int i = services_pool_size - DEFAULT_POOL_SIZE; i < services_pool_size; i++) {
							if (services_pool[i].state != SERVICE_AVAILABLE) {
								decrease_condition = 0;
								break;
							}
					}
					// завершаем работу лишних потоков
					if (decrease_condition) {
						for(int i = services_pool_size - DEFAULT_POOL_SIZE; i < services_pool_size; i++) {
							printf("[server sending cancellation request to thread (thread_id = %d)]\n", i);
							int s = pthread_cancel(services_pool[i].thread);
							if (s == 0) {
								s = pthread_join(services_pool[i].thread, &res);
								if (s == 0) {
									if (res == PTHREAD_CANCELED) {
										printf("[server cancel thread (tread_id = %d) ok!]\n", i);
									} else {
											decrease_condition = 0;
											services_pool[i].state = SERVICE_ERROR;
											printf("[server error cancel thread (tread_id = %d)]\n", i);
									}
								}
							}
						}
					}
					// если работа всех потоков успешно заверешена, то уменьшаем размер пула
					if (decrease_condition) {
						// уменьшаем размер пула на DEFAULT_POOL_SIZE
						int new_services_pool_size = services_pool_size - DEFAULT_POOL_SIZE;
				    	// перераспределяем область памяти на новый размер пула
				    	struct service* ptr = realloc(services_pool, new_services_pool_size * sizeof(struct service));
				    	// убеждаемся что память успешно была перераспределена
				    	if (ptr != NULL) {
				    		services_pool = ptr;
							services_pool_size = new_services_pool_size;
						} else {
							// если произошла ошибка, то можно ее обработать
							printf("[server decrease pool errror!]\n");
							break;
				    	}
				    }
				}
			// размер пула уменьшается только в том, случае, если все крайние сервисы освободились
			}
			sleep(1);
		}
	printf("close server...\n");
	// отменяем потоки и оповещаем клиентов о завершении работы программы
	//...
	// ждем успешного заверешения работы потоков
  	for(int i = 0; i < services_pool_size; i++) {
    	void* res;
    	int s = pthread_join(services_pool[i].thread, &res);
    }
    // удаляем очередь по выходу из цикла
    msgctl(service_messaging_queue_id, IPC_RMID, NULL);
    // освобождаем пул
    free(services_pool);
    // закрываем сокет
    close(socket_fd);
    printf("close server... Ok!\n");
}

void run_client(unsigned int server_ip, unsigned short port_server, unsigned short port_client) {
    int socket_fd, socket_fd_rcv;
  	struct sockaddr_in server_address;
    unsigned short sid = SERVICE_ID_NOT_DEFINED;
    char message_data[MESSAGE_DATA_LENGTH];
    char mid = 0; // id сообщения
    int state;

  	/* Создаем клиентский сокет; привязываем его к уникальному пути
  	(основанному на PID) */
  	socket_fd = socket(AF_INET, SOCK_RAW, IPPROTO_UDP);
  	if (socket_fd == -1)
  		perror("perror");

  	/* Формируем адрес сервера */
  	memset(&server_address, 0, sizeof(struct sockaddr_in));
  	server_address.sin_family = AF_INET;
  	server_address.sin_port = htons(port_server);
  	server_address.sin_addr.s_addr = htonl(server_ip); // inet_addr("127.0.0.1");

    // формируем сообщение установа соединения
  	memset(&message_data, '\0', MESSAGE_DATA_LENGTH);
    strcpy(message_data, "Hello!");
    printf("[client send message: %s]\n", message_data);

    // отправляем raw UDP сообщение в сокет по адресу сервера
    state = send_message(socket_fd, server_address, port_client, port_server, sid, mid, MESSAGE_CONNECT, message_data);

    if (state == MESSAGE_SENT) {
    	printf("[client message connect sent]\n");
    } else {
    	printf("[client message connect sent error!]\n");
    }

    struct message message = receive_message_reply_safe(socket_fd, server_address, sid, mid + 1);

    if (message.state == MESSAGE_RECEIVED) {
    	printf("[client receive message: %s]\n", message.data);
    	sid = message.sid;
    	printf("[client receive service_id = %hu]\n", sid);
    } else {
    	printf("[client receive message error!]\n");
    	exit(EXIT_FAILURE);
    }

    while (!program_exit_flag) {
    	// ждем ввода сообщения пользователя
  	  	memset(&message_data, '\0', MESSAGE_DATA_LENGTH);
      	printf(">> ");
      	gets(message_data);

      	// считываем команду заверешения работы клиента
      	if (strcmp(message_data, "q") == 0 || strcmp(message_data, "quit") == 0) {
      		printf("[client end work]\n");
      		printf("Bye! Bye!\n");
      		break;
      	}

      	printf("[client send message: %s]\n", message_data);

      	// отправляем raw UDP сообщение в сокет по адресу сервера
      	state = send_message(socket_fd, server_address, port_client, port_server, sid, mid, MESSAGE_DATA, message_data);

      	if (state == MESSAGE_SENT) {
      		printf("[client message data sent]\n");
+
	      	// получаем ответ от сервиса
	        struct message message = receive_message_reply_safe(socket_fd, server_address, sid, mid + 1);

	        if (message.state == MESSAGE_RECEIVED) {
	        	printf("[client receive service reply: %s]\n", message.data);
	        	printf("service[%hu]>> %s\n", sid, message.data);
				mid++;
			if (mid == USHRT_MAX) {
				mid = 0;
			}

	        } else {
	        	printf("[client receive service reply error!]\n");
	        	break; // количество попыток превысило максимальное значение
	        }
    	} else {
    		printf("[client message data sent error!]\n");
    		// здесь мы можем организовать сохранение сообщения для последующей переотправки
      	}
    }

    // формируем сообщение закрытия сессии
  	memset(&message_data, '\0', MESSAGE_DATA_LENGTH);
    strcpy(message_data, "Bye!");

    printf("[client send message: %s]\n", message_data);

    // отправляем raw UDP сообщение в сокет по адресу сервера
    state = send_message(socket_fd, server_address, port_client, port_server, sid, mid, MESSAGE_CLOSE, message_data);

    if (state == MESSAGE_SENT) {
    	printf("[client message close sent]\n");
    } else {
    	printf("[client message close sent error!]\n");
    }

  	// обязательно закрываем сокет
  	close(socket_fd);
}

// отображение справки
static void print_help(char * program_name, char * message) {
	if (message != NULL) fputs(message, stderr);
  	fprintf(stderr, "Usage: %s [options]\n", program_name);
  	fprintf(stderr, "Options are:\n");
  	fprintf(stderr, "-c client mode read/send messages (by default)\n");
  	fprintf(stderr, "-s server mode control messages/clients\n");
  	fprintf(stderr, "-v verbosity level: 0 just chat, 1 explain job client-server\n");
  	fprintf(stderr, "-p <port_number> client-server port number from 0 to 65535\n");
  	exit(EXIT_FAILURE);
};

void exit_program() {
  	program_exit_flag = 1;
}

// точка входа в программу
int main(int argc, char * argv[]) {
	// установ сигнала выхода из программы по команде ctrl-c из терминала
	signal(SIGINT, exit_program);

	int mode = DEFAULT_MODE; // режим работы программы: 0 - клиент, 1 - сервер
  	int server_ip = DEFAULT_SERVER_IP;
  	int port_server = DEFAULT_PORT_SERVER;
  	int port_client = DEFAULT_PORT_CLIENT;

  	// читаем параметры аргументов командной строки
  	int opt;
  	while ((opt = getopt(argc, argv, "csvip:")) != -1) {
    	switch (opt) {
    	// опция клиента
    	case 'c':
     		mode = 0;
      		break;
   		// опция сервера
    	case 's':
      		mode = 1;
      		break;
    	// опция болтать
    	case 'v':
      		verbose = 1;
      		break;
    	// опция IP-адресс сервера (должен соответствовать формату XXX.XXX.XXX.XXX где XXX принимает значения от 0 до 255)
    	case 'i':
      		server_ip = inet_addr(optarg);
     		break;
    	// опция номер порта сервера и клиента (должен соответствовать значению от 0 до 65535)
   		case 'p':
      		port_server = port_client = atoi(optarg);
      		break;
    	default:
      		print_help(argv[0], "Unrecognized option\n");
    	}
    }

    // запускаем сервер или клиент
    if (mode == 1) {
    	run_server(server_ip, port_server);
    } else {
    	run_client(server_ip, port_server, port_client); // здесь указывается IP-адрес и порт сервера, а затем порт клиента
    }

    // выход из программы с флагом успешного завершения
    exit(EXIT_SUCCESS);
}
// TODO сделать для TCP
