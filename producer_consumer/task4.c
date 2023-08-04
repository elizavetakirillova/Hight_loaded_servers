/*
 * Клиент-сервер на основе модели Производитель-потребитель.
 *
 * Запускаем слушающий сервер, он создает очередь обслуживания и пул обслуживающих серверов.
 * Есть клиент, он посылает заявку серверу, сервер ставит заявку в очередь запросов.
 * В это время любой свободный сервер забирает заявку, решает ее
 * ставит в очередь ответов, слушающий сервер забирает ее оттуда и передает ее клиенту.
 * Либо обслуживающий сервер сам может передать заявку клиенту обратно
 * (для этого в очереди запросов должен быть идентификатор клиента).
 * Реализовать для TCP или UDP.
 */

#include <pthread.h>

#include <stdio.h>

#include <stdlib.h>

#include <unistd.h>

#include <fcntl.h>

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
// N.B. для использования портов нужно ввести команду$ ufw allow 8000 && ufw allow 8001

// параметры сервера
#define SERVER_ID 1
#define SERVICE_ID_ANY 2
#define DEFAULT_SERVICE_MESSAGING_KEY_1 12345 // значение ключа для создания очереди для взаимодействия сервера и сервисов
#define DEFAULT_SERVICE_MESSAGING_KEY_2 54321
#define DEFAULT_POOL_SIZE 10 // размер пула сервисов по умолчанию (10 сервисов для обслуживания клиентов)

// параметры клиента
#define RECEIVE_MESSAGE_MAX_ATTEMPTS 1 // количество запросов на получение сообщения до экстренного выхода из чата

// параметры времени задержки
#define DEFAULT_SERVER_CYCLE_DELAY 1
#define DEFAULT_CLIENT_CYCLE_DELAY 1
#define DEFAULT_CLIENT_ATTEMPT_DELAY 1

// параметры UDP сообщения
#define IP_HEADER_SIZE 20 // стандартный размер IP-заголовка в байтах
#define UDP_HEADER_SIZE 8 // стандартный размер заголовка UPD-пакета в байтах
#define MESSAGE_DATA_LENGTH 20 // размер данных сообщения в байтах (sizeof char = 1 байт)
#define MESSAGE_FULL_LENGTH MESSAGE_DATA_LENGTH + 6 // полная длина сообщения в байтах (sizeof unsigned short = 2 байта)
#define UDP_MESSAGE_LENGTH UDP_HEADER_SIZE + MESSAGE_FULL_LENGTH // полная длина UDP-сообщения пересылаемого по сети
#define IP_PLUS_UDP_SIZE IP_HEADER_SIZE + UDP_HEADER_SIZE
#define RAW_MESSAGE_LEN IP_PLUS_UDP_SIZE + MESSAGE_FULL_LENGTH

// типы сообщения
#define NO_MESSAGE 0
#define MESSAGE_DATA 1

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
// СТРУКТУРЫ КОТОРЫЕ ИСПОЛЬЗУЮТСЯ ДЛЯ ПЕРЕДАЧИ ДАННЫХ ПО СЕТИ (КЛИЕНТ-СЕРВЕР)
////////////////////////////////////////////////////////////////////////////////

// структура UDP-заголовка
struct udp_header {
  unsigned short source_port; /* порт источника */
  unsigned short dest_port; /* порт назначения */
  unsigned short packet_len; /* длина UDP-пакета = 8 байт (заголовок) + MESSAGE_FULL_LENGTH (сообщение) */
  unsigned short check_sum; /* чек-сумма для проверки целостности пакета = 0 (мы не проверяем)*/
};

// структура сообщения
struct message {
  unsigned short id; // id сообщения
  unsigned short type; // тип сообщения
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
// СТРУКТУРЫ ИСПОЛЬЗУЕМЫЕ ВНУТРИ СЕРВЕРА (ЛОКАЛЬНО)
////////////////////////////////////////////////////////////////////////////////

// структура сервиса
struct service {
  unsigned short id; // id сервиса
  pthread_t thread; // поток POSIX
  unsigned short state; // состояние сервиса (свободен, доступен, занят или не инициализирован)
  struct sockaddr_in client_address; // адрес клиента назначенного сервером
};

// сообщение сервиса
struct service_message {
  long type;
  struct message_ext data;
};

// длина данных сообщения сервиса
ssize_t service_data_length() {
  return sizeof(short) + sizeof(unsigned short) + sizeof(unsigned long) + sizeof(char) * 8 + MESSAGE_FULL_LENGTH;
}

////////////////////////////////////////////////////////////////////////////////

// флаги управления работой программы
static volatile int program_exit_flag = 0; // флаг обработки выхода из программы
static volatile int verbose = 0; // флаг управления выводом служебных сообщений

////////////////////////////////////////////////////////////////////////////////

/*
 * Функции обмена сообщений с сервисами
 */

// отправка сообщения в очередь
unsigned short send_message_to_queue(int queue_id, int receiver_id, struct message_ext message_ext) {

  // создаем инстанс сообщения
  struct service_message service_message;

  // заполняем данные сообщения
  service_message.type = receiver_id; // указываем тип сообщения
  service_message.data = message_ext;
  memset(service_message.data.msg.data, '\0', sizeof(service_message.data.msg.data));
  strcpy(service_message.data.msg.data, message_ext.msg.data);

  // пытаемся отправить сообщение в очередь, проверяем ошибку
  if (msgsnd(queue_id, & service_message, service_data_length(), IPC_NOWAIT) == -1) {

    // показываем ошибку, выходим из программы с кодом завершения с ошибкой
    perror("msgsnd error");

    // устанавливаем флаг ошибки на состояние
    service_message.data.msg.state = MESSAGE_ERROR;

  } else {

    // устанавливаем флаг успешной отправки сообщения
    service_message.data.msg.state = MESSAGE_SENT;
  }

  return service_message.data.msg.state;
};

// получение сообщения из очереди
struct message_ext receive_message_from_queue(int queue_id, int receiver_id) {

  // создаем инстанс структуры сообщения
  struct service_message message;

  // пытаемся получить сообщение из очереди в наш инстанс, проверяем ошибки
  if (msgrcv(queue_id, (void * ) & message, service_data_length(), receiver_id, MSG_NOERROR | IPC_NOWAIT) == -1) {

    // это не ошибка отсуствия сообщения?
    if (errno != ENOMSG) {

      // нет это другая ошибка
      // perror("msgrcv error");

    } else {

      // сообщения нет в очереди
      message.data.msg.state = NO_MESSAGE;
    }

    // устанавливаем флаг ошибки на состояние
    message.data.msg.state = MESSAGE_ERROR;

  } else {

    // сообщение успешно получено из очереди
    message.data.msg.state = MESSAGE_RECEIVED;
  }

  return message.data;
};

/*
 * Сетевые функции для коммуникации клиента и сервера
 */

// отправка raw UDP сообщения в сокет
int send_message(int socket_fd, struct sockaddr_in address, unsigned short port_sender, unsigned short port_receiver, unsigned short mid, unsigned short type, char * message_data) {

  struct udp_message udp_message;

  // формируем UPD заголовок сообщения
  udp_message.udph.source_port = htons(port_sender); // Порт отправителя
  udp_message.udph.dest_port = htons(port_receiver); // Порт получателя
  udp_message.udph.packet_len = htons(UDP_MESSAGE_LENGTH); // Длина пакета
  udp_message.udph.check_sum = 0; // Контрольная сумма (не проверяем)

  // подготавливаем данные к отправке
  memset( & udp_message.msg.data, '\0', MESSAGE_DATA_LENGTH);
  strcpy(udp_message.msg.data, message_data);
  udp_message.msg.type = type;
  udp_message.msg.id = mid;
  udp_message.msg.state = MESSAGE_PREPARED;

  // printf("SEND___\n");
  // printf("%s\n", udp_message.msg.data);
  // printf("_______\n");

  /* Копируем в сокет содержимое буфера запроса */
  if (sendto(socket_fd, & udp_message, UDP_MESSAGE_LENGTH, 0, (struct sockaddr * ) & address, sizeof(struct sockaddr_in)) == -1) {

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

  ssize_t number_of_bytes = recvfrom(socket_fd, & message_ext.msg, MESSAGE_FULL_LENGTH, 0, (struct sockaddr * ) & message_ext.addr, & addr_len);

  // printf("RECIVE___\n");
  // printf("%s\n", message_ext.msg.data);
  // printf("_________\n");

  if (number_of_bytes == -1) {

    message_ext.msg.state = MESSAGE_ERROR;

    // perror("recvfrom");

  } else {

    message_ext.msg.state = MESSAGE_RECEIVED;
  }

  return message_ext;
}

// отправка сообщения в сокет по адресу (DATAGRAM)
int send_message_reply(int socket_fd, struct sockaddr_in address, unsigned short mid, unsigned int type, char * message_data) {

  struct message message;

  socklen_t addr_len = sizeof(struct sockaddr_in);

  // подготавливаем сообщение для отправки в сокет
  memset( & message.data, '\0', MESSAGE_DATA_LENGTH);
  strcpy(message.data, message_data);
  message.type = type;
  message.id = mid;
  message.state = MESSAGE_PREPARED;

  // printf("REPLY___\n");
  // printf("%s\n", message.data);
  // printf("________\n");

  if (sendto(socket_fd, & message, MESSAGE_FULL_LENGTH, 0, (struct sockaddr * ) & address, addr_len) == -1) {

    message.state = MESSAGE_ERROR;

    perror("sendto");

  } else {

    message.state = MESSAGE_SENT;
  }

  return message.state;
}

// получение raw UDP сообщения из сокета
struct message receive_message_reply(int socket_fd, struct sockaddr_in address, unsigned short mid) {

  struct message_raw message_raw;
  message_raw.msg.id = (mid != USHRT_MAX) ? mid + 1 : 0;
  memset( & message_raw.msg.data, '\0', MESSAGE_DATA_LENGTH);

  socklen_t addr_len = sizeof(struct sockaddr_in);

  ssize_t number_of_bytes = recvfrom(socket_fd, & message_raw, RAW_MESSAGE_LEN, 0, (struct sockaddr * ) & address, & addr_len);

  // printf("ANSWER___\n");
  // printf("%s\n", message_raw.msg.data);
  // printf("_________\n");

  if (number_of_bytes == -1 || message_raw.msg.id != mid) {

    message_raw.msg.state = MESSAGE_ERROR;

    // perror("recvfrom");

  } else {

    message_raw.msg.state = MESSAGE_RECEIVED;
  }

  return message_raw.msg;
}

/*
 * Функция работы сервиса
 *
 * сервис просто читает сообщения из очереди и отправляет с нужным приоритетом для считывания сервером
 *
 * сервер определяет по приоритету какому клиенту нужно отправить ответное сообщение
 */
static void * service_fun(void * service_ptr) {

  struct service * service = (struct service * ) service_ptr;
  int request_queue_id, reply_queue_id;

  // получение идентификатора очереди сообщений для коммуникации с сервером
  request_queue_id = msgget(DEFAULT_SERVICE_MESSAGING_KEY_1, 0);
  if (request_queue_id == -1) {
    perror("msgget error");
    exit(EXIT_FAILURE);
  }

  // получение идентификатора очереди сообщений для коммуникации с сервером
  reply_queue_id = msgget(DEFAULT_SERVICE_MESSAGING_KEY_2, 0);
  if (reply_queue_id == -1) {
    perror("msgget error");
    exit(EXIT_FAILURE);
  }

  while (!program_exit_flag) {

    struct message_ext message = receive_message_from_queue(request_queue_id, SERVICE_ID_ANY);

    if (message.msg.state == MESSAGE_RECEIVED) {

      if (verbose) printf("[service service_id = %d get message: %s]\n", service -> id, message.msg.data);
      fflush(stdout);

      message.msg.data[0] = 'Z';

      if (verbose) printf("[service service_id = %d processed message: %s]\n", service -> id, message.msg.data);
      fflush(stdout);

      // реагируем на сообщение и отправляем дополненное сообщение клиенту
      int state = send_message_to_queue(reply_queue_id, SERVER_ID, message);

      if (state == MESSAGE_SENT) {

        if (verbose) printf("[service service_id = %d message sent!]\n", service -> id);
        fflush(stdout);
      } else {

        // можно попытаться переотправить сообщение в очередь здесь
      }
    }
  }
};

// функция работы сервера создающего пул обслуживающих сервисов
void run_server(unsigned int server_ip, unsigned short port_server) {

  // Содаем слушающий сервер
  int socket_fd;
  struct sockaddr_in server_address;
  int request_queue_id, reply_queue_id;
  struct service * services_pool;
  int services_pool_size = DEFAULT_POOL_SIZE;
  char message_data[MESSAGE_DATA_LENGTH];
  int state;

  // получаем файл-дескриптор сокета
  socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (socket_fd == -1)
    perror("socket");
  fcntl(socket_fd, F_SETFL, O_NONBLOCK);

  // инициализируем адрес слушающего сервера
  memset( & server_address, 0, sizeof(struct sockaddr_in));
  server_address.sin_family = AF_INET;
  server_address.sin_port = htons(port_server);
  server_address.sin_addr.s_addr = htonl(server_ip);

  // подключаем сокет
  if (bind(socket_fd, (struct sockaddr * ) & server_address, sizeof(struct sockaddr_in)) == -1)
    perror("bind");

  // создание очереди запросов (к сервисам)
  request_queue_id = msgget(DEFAULT_SERVICE_MESSAGING_KEY_1, IPC_EXCL | IPC_CREAT | 0600);
  if (request_queue_id == -1) {
    perror("msgget error");
    exit(EXIT_FAILURE);
  }

  // создание очереди ответов (от сервисов)
  reply_queue_id = msgget(DEFAULT_SERVICE_MESSAGING_KEY_2, IPC_EXCL | IPC_CREAT | 0600);
  if (reply_queue_id == -1) {
    perror("msgget error");
    exit(EXIT_FAILURE);
  }

  // инициализируем и запускаем пул потоков с работающими сервисами
  services_pool = malloc(services_pool_size * sizeof(struct service));
  for (int i = 0; i < services_pool_size; i++) {

    // инициализируем id сервиса
    services_pool[i].id = i;

    // создаем и запускаем отдельные потоки для сервисов
    int result_state = pthread_create( & (services_pool[i].thread), NULL, & service_fun, & services_pool[i]);

    // проверяем инициализацию потока
    if (result_state == 0) {

      services_pool[i].state = SERVICE_AVAILABLE; // устанавливаем состояние сервиса

    } else if (result_state != 0) {

      services_pool[i].state = SERVICE_ERROR;
    }
  }

  while (!program_exit_flag) {

    // получаем сообщение от клиента
    struct message_ext client_message = receive_message(socket_fd);

    // производим обработку сообщения
    if (client_message.msg.type == MESSAGE_DATA) {

      // пришли данные от клиента, нужно поставить их в очередь запросов
      if (verbose) printf("[server receive message from client: %s (type %hu)]\n", client_message.msg.data, client_message.msg.type);

      // отправляем сообщение в очередь указывая service_id
      state = send_message_to_queue(request_queue_id, SERVICE_ID_ANY, client_message);

      // если сообщение отправлено, то ожидаем ответного сообщения от потока
      if (state == MESSAGE_SENT) {

        if (verbose) printf("[server message sent to the request queue: %s]\n", client_message.msg.data);

        // отправляем сообщение в сокет по адресу клиента
        state = send_message_reply(socket_fd, client_message.addr, client_message.msg.id + 1, MESSAGE_DATA, client_message.msg.data);

        if (state == MESSAGE_SENT) {

          if (verbose) printf("[server client message reply sent]\n");

        } else {

          if (verbose) printf("[server client message reply error!]\n");
        }

      } else {

        // сообщение не отправлено в очередь
        if (verbose) printf("[server message send error]\n");

        // здесь можно заново перепослать сообщение в очередь если случилась ошибка
        // или придумать как гарантировать 100% отправку сообщения в очередь
      }
    }

    // запрашиваем сообщение из очереди
    struct message_ext service_message = receive_message_from_queue(reply_queue_id, SERVER_ID);

    if (service_message.msg.state == MESSAGE_RECEIVED) {

      // если сообщение получено
      if (verbose) printf("[server message processed by service %s]\n", service_message.msg.data);

      // отправляем сообщение в сокет по адресу клиента
      state = send_message_reply(socket_fd, service_message.addr, service_message.msg.id + 1, MESSAGE_DATA, service_message.msg.data);

      if (state == MESSAGE_SENT) {

        if (verbose) printf("[server client message reply sent]\n");

      } else {

        if (verbose) printf("[server client message reply error!]\n");
      }
    }

    fflush(stdout);

    sleep(DEFAULT_SERVER_CYCLE_DELAY);
  }

  printf("close server...\n");

  // отменяем потоки и оповещаем клиентов о завершении работы программы
  //...

  // ждем успешного заверешения работы потоков
  for (int i = 0; i < services_pool_size; i++) {
    void * res;
    int s = pthread_join(services_pool[i].thread, & res);
  }

  // удаляем очередь по выходу из цикла
  msgctl(request_queue_id, IPC_RMID, NULL);

  // освобождаем пул
  free(services_pool);

  // закрываем сокет
  close(socket_fd);

  printf("close server... Ok!\n");
}

void run_client(unsigned int server_ip, unsigned short port_server, unsigned short port_client) {

  int socket_fd, socket_fd_rcv;
  struct sockaddr_in server_address;
  char message_data[MESSAGE_DATA_LENGTH];
  char mid = 0; // id сообщения
  int state;
  int wait_server_reply_flag = 0;

  /* Создаем клиентский сокет; привязываем его к уникальному пути
  (основанному на PID) */
  socket_fd = socket(AF_INET, SOCK_RAW, IPPROTO_UDP);
  if (socket_fd == -1)
    perror("perror");
  fcntl(socket_fd, F_SETFL, O_NONBLOCK);

  /* Формируем адрес сервера */
  memset( & server_address, 0, sizeof(struct sockaddr_in));
  server_address.sin_family = AF_INET;
  server_address.sin_port = htons(port_server);
  server_address.sin_addr.s_addr = htonl(server_ip); // inet_addr("127.0.0.1");

  while (!program_exit_flag) {

    if (!wait_server_reply_flag) {

      // ждем ввода сообщения пользователя
      memset( & message_data, '\0', MESSAGE_DATA_LENGTH);
      printf(">> ");
      gets(message_data);

      // считываем команду заверешения работы клиента
      if (strcmp(message_data, "q") == 0 || strcmp(message_data, "quit") == 0) {

        if (verbose) printf("[client end work]\n");

        printf("Bye! Bye!\n");

        break;
      }

      if (verbose) printf("[client send message request: %s]\n", message_data);

      // отправляем raw UDP сообщение в сокет по адресу сервера
      state = send_message(socket_fd, server_address, port_client, port_server, mid, MESSAGE_DATA, message_data);

      if (state == MESSAGE_SENT) {

        if (verbose) printf("[client message request data sent]\n");

        wait_server_reply_flag = 2;

      } else {

        if (verbose) printf("[client message request data sent error!]\n");

        // здесь мы можем организовать сохранение сообщения для последующей переотправки
      }
    } else {

      // получаем ответ от сервиса
      struct message message = receive_message_reply(socket_fd, server_address, mid + 1);

      if (message.state == MESSAGE_RECEIVED) {

        if (verbose) printf("[client receive service reply: %s]\n", message.data);

        printf("server reply %s\n", message.data);

        wait_server_reply_flag--;

        if (wait_server_reply_flag == 0) {
          mid++;
          if (mid == USHRT_MAX) {
            mid = 0;
          }
        }

      }
    }
    sleep(DEFAULT_CLIENT_CYCLE_DELAY);
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
// TODO сделать пункт осчастливливания
