// SPDX-License-Identifier: BSD-3-Clause
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/sendfile.h>
#include <sys/eventfd.h>
#include <libaio.h>
#include <errno.h>

#include "aws.h"
#include "utils/util.h"
#include "utils/debug.h"
#include "utils/sock_util.h"
#include "utils/w_epoll.h"

// definim un macro pentru numarul maxim de evenimente posibile simultan
// in w_epoll.h, w_epoll_create() foloseste valoarea 10, motiv pentru care am ales si eu aceeasi valoare
#define EPOLL_QUEUE_LEN 10

/* server socket file descriptor */
static int listenfd;

/* epoll file descriptor */
static int epollfd;

static io_context_t ctx;

static int aws_on_path_cb(http_parser *p, const char *buf, size_t len)
{
	struct connection *conn = (struct connection *)p->data;

	memcpy(conn->request_path, buf, len);
	conn->request_path[len] = '\0';
	conn->have_path = 1;

	return 0;
}

static void connection_prepare_send_reply_header(struct connection *conn)
{
	// printam mesajul
	int content_length = conn->file_size;
	char buffer_helper[200];

	strcpy(conn->send_buffer, "HTTP/1.1 200 OK\r\n");
	strcat(conn->send_buffer, "Content-Length: ");
	sprintf(buffer_helper, "%d", content_length);
	strcat(conn->send_buffer, buffer_helper);
	strcat(conn->send_buffer, "\r\n");
	strcat(conn->send_buffer, "Connection: close\r\n");
	strcat(conn->send_buffer, "\r\n");

	conn->send_len = strlen(conn->send_buffer);
	conn->send_pos = 0;

	// schimbam starea conexiunii
	conn->state = STATE_SENDING_HEADER;
}

static void connection_prepare_send_404(struct connection *conn)
{
	// printam mesajul de eroare
	strcpy(conn->send_buffer, "HTTP/1.1 404 Not Found\r\n");
	strcat(conn->send_buffer, "Content-Length: 0\r\n");
	strcat(conn->send_buffer, "Connection: close\r\n");
	strcat(conn->send_buffer, "\r\n");

	conn->send_len = strlen(conn->send_buffer);
	conn->send_pos = 0;

	// schimbam starea conexiunii
	conn->state = STATE_SENDING_404;
}

static enum resource_type connection_get_resource_type(struct connection *conn)
{
	if (strstr(conn->request_path, "static") != NULL) {
		// inseamna ca avem folder static
		conn->res_type = RESOURCE_TYPE_STATIC;
	} else if (strstr(conn->request_path, "dynamic") != NULL) {
		// inseamna ca avem folder dinamic
		conn->res_type = RESOURCE_TYPE_DYNAMIC;
	} else {
		conn->res_type = RESOURCE_TYPE_NONE;
	}
	return conn->res_type;
}


struct connection *connection_create(int sockfd)
{
	struct connection *conn = malloc(sizeof(*conn));

	if (conn == NULL) {
		printf("eroare la alocarea memoriei");
		exit(1);
	}

	conn->sockfd = sockfd;
	conn->file_size = 0;
	conn->recv_len = 0;
	conn->send_len = 0;
	conn->send_pos = 0;
	conn->file_pos = 0;
	conn->async_read_len = 0;
	conn->state = STATE_INITIAL;
	conn->eventfd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	if (conn->eventfd < 0) {
		free(conn);
		return NULL;
	}

	w_epoll_add_ptr_in(epollfd, conn->eventfd, conn);

	conn->filename[0] = '\0';
	conn->send_buffer[0] = '\0';
	conn->recv_buffer[0] = '\0';
	conn->request_path[0] = '\0';
	return conn;
}

void connection_start_async_io(struct connection *conn)
{
	// definim starea
	conn->state = STATE_ASYNC_ONGOING;

	//pregatim biletul
	io_prep_pread(&conn->iocb, conn->fd, conn->send_buffer, BUFSIZ, conn->file_pos);

	// punem eticheta
	conn->iocb.data = conn;

	// setam soneria
	io_set_eventfd(&conn->iocb, conn->eventfd);

	// definim structura
	struct iocb *piocb = &conn->iocb;
	int return_number = io_submit(ctx, 1, &piocb);

	if (return_number != 1)
		conn->state = STATE_CONNECTION_CLOSED;
}

void connection_remove(struct connection *conn)
{
	// eliminam socket-ul de pe panou
	w_epoll_remove_ptr(epollfd, conn->sockfd, conn);
	close(conn->sockfd);
	if (conn->fd > 0)
		close(conn->fd);

	w_epoll_remove_ptr(epollfd, conn->eventfd, conn);
	close(conn->eventfd);
	free(conn);
}

void handle_new_connection(void)
{
	struct connection *conn_nou;

	// creem structura pentru client si facem totodata socket-ul sa fie non-blocking
	struct sockaddr_in adresa_client;
	socklen_t client_address_len = sizeof(adresa_client);

	int clientfd = accept4(listenfd, (struct sockaddr *)&adresa_client, &client_address_len, SOCK_NONBLOCK);

	if (clientfd < 0) {
		printf("a survenit o eroare");
		return;
	}

	// creem o noua conexiune pentru client
	conn_nou = connection_create(clientfd);

	w_epoll_add_ptr_in(epollfd, clientfd, conn_nou);

	// initializam un nou HTTP_REQUEST parser
	http_parser_init(&conn_nou->request_parser, HTTP_REQUEST);
	conn_nou->request_parser.data = conn_nou;
}

void receive_data(struct connection *conn)
{
	ssize_t bytes_total = recv(conn->sockfd, conn->recv_buffer + conn->recv_len, BUFSIZ - conn->recv_len, 0);

	if (bytes_total > 0) {
		// extragem cate litere am mai adaugat acum in buffer
		conn->recv_len = conn->recv_len + bytes_total;
	}

	if (conn->state == STATE_INITIAL) {
		if (strstr(conn->recv_buffer, "\r\n\r\n") != NULL) {
			// inseamna ca am ajuns la finalul citirii, putem rezolva cerinta
			// apelam parserul pentru a ne parsa buffer-ul recv_buffer
			parse_header(conn);
			if (conn->request_parser.method == HTTP_GET) {
				//construim calea pentru functia connection_open_file
				// 1. aflam cu ce fel de fisier avem de lucru
				connection_get_resource_type(conn);
				if (conn->res_type == RESOURCE_TYPE_NONE) {
					connection_prepare_send_404(conn);

					w_epoll_update_ptr_out(epollfd, conn->sockfd, conn);
					return;
				}
				// extragem filename-ul din request_path
				char *filename = strrchr(conn->request_path, '/');

				if (filename != NULL)
					filename++;  // sarim peste '/'
				else
					filename = conn->request_path;

				strcpy(conn->filename, AWS_DOCUMENT_ROOT);
				if (conn->res_type == RESOURCE_TYPE_STATIC)
					strcat(conn->filename, AWS_REL_STATIC_FOLDER);
				else if (conn->res_type == RESOURCE_TYPE_DYNAMIC)
					strcat(conn->filename, AWS_REL_DYNAMIC_FOLDER);

				// 2. pentru a crea calea completa, lipim si numele fisierului
				strcat(conn->filename, filename);

				int return_number = connection_open_file(conn);

				if (return_number == -1) {
					connection_prepare_send_404(conn);

					w_epoll_update_ptr_out(epollfd, conn->sockfd, conn);
					return;
				}
				// pregatim header-ul si trecem in starea de trimitere header pentru ambele tipuri
				connection_prepare_send_reply_header(conn);
				conn->state = STATE_SENDING_HEADER;
				w_epoll_update_ptr_out(epollfd, conn->sockfd, conn);
			} else {
				connection_prepare_send_404(conn);
				w_epoll_update_ptr_out(epollfd, conn->sockfd, conn);
			}
		}
	}
}

int connection_open_file(struct connection *conn)
{
	int filefd = open(conn->filename, O_RDONLY | O_NONBLOCK);

	conn->fd = filefd;

	if (filefd == -1)
		return -1;

	// initializam un element stat pentru apelarea functiei fstat
	struct stat stat;
	int return_number = fstat(filefd, &stat);

	if (return_number == -1)
		return -1;

	conn->file_size = stat.st_size;
	conn->file_pos = 0;

	return 0;
}

void connection_complete_async_io(struct connection *conn)
{
	struct io_event eveniment;
	uint64_t buf;

	read(conn->eventfd, &buf, sizeof(buf));

	io_getevents(ctx, 1, 1, &eveniment, NULL);

	if (eveniment.res > 0) {
		conn->send_len = eveniment.res;
		conn->send_pos = 0;
		conn->file_pos = conn->file_pos + eveniment.res;
		conn->state = STATE_SENDING_DATA;
	} else if (eveniment.res == 0) {
		conn->state = STATE_DATA_SENT;
	} else {
		conn->state = STATE_CONNECTION_CLOSED;
	}
}

int parse_header(struct connection *conn)
{
	http_parser_settings settings_on_path = {
		.on_message_begin = 0,
		.on_header_field = 0,
		.on_header_value = 0,
		.on_path = aws_on_path_cb,
		.on_url = 0,
		.on_fragment = 0,
		.on_query_string = 0,
		.on_body = 0,
		.on_headers_complete = 0,
		.on_message_complete = 0
	};

	int dimensiune_parser = http_parser_execute(&conn->request_parser, &settings_on_path,
		conn->recv_buffer, conn->recv_len);

	if (dimensiune_parser != conn->recv_len) {
		// inseamna ca avem o eroare
		connection_prepare_send_404(conn);
	}
	return 0;
}

enum connection_state connection_send_static(struct connection *conn)
{
	off_t offset = conn->file_pos;
	ssize_t bytes_interchanged = sendfile(conn->sockfd, conn->fd, &offset, conn->file_size - conn->file_pos);

	if (bytes_interchanged > 0)
		conn->file_pos = offset;

	if (conn->file_pos != conn->file_size && bytes_interchanged != -1) {
		// inseamna ca nu am scris toti bytes
		conn->state = STATE_SENDING_DATA;
	} else if (bytes_interchanged == -1) {
		// a survenit o eroare, inchidem socket-ul
		conn->state = STATE_CONNECTION_CLOSED;
	} else {
		conn->state = STATE_DATA_SENT;
	}
	return conn->state;
}

int connection_send_data(struct connection *conn)
{
	int send_bytes = send(conn->sockfd, conn->send_buffer + conn->send_pos, conn->send_len - conn->send_pos, 0);

	if (send_bytes == -1)
		return -1;

	// mutam cursorul intrucat am mai adaugat ceva
	conn->send_pos = conn->send_pos + send_bytes;

	// verificam daca am adaugat tot ce era de adaugat
	if (conn->send_pos == conn->send_len) {
		// inseamna ca s-a trimis intregul string
		if (conn->state == STATE_SENDING_404) {
			// inseamna ca tocmai am venit din functia connection_prepare_send_404
			conn->state = STATE_404_SENT;
		}
	}
	return send_bytes;
}


int connection_send_dynamic(struct connection *conn)
{
	if (conn->state == STATE_SENDING_DATA) {
		// mai avem date de trimis
		int return_number = connection_send_data(conn);

		if (return_number == -1) {
			// inseamna ca s-a semnalat o eroare in functia precedenta
			return -1;
		}
	}

	if (conn->send_pos == conn->send_len) {
		// daca mai avem bucati de citit, punem socket-ul pe IN si pornim urmatoarea citire AIO
		if (conn->file_pos < conn->file_size) {
			w_epoll_update_ptr_in(epollfd, conn->sockfd, conn);
			connection_start_async_io(conn);
		} else {
			conn->state = STATE_DATA_SENT;
		}
	}
	return 0;
}


void handle_input(struct connection *conn)
{
	switch (conn->state) {
	case STATE_INITIAL:
		receive_data(conn);
		break;

	case STATE_RECEIVING_DATA:
		receive_data(conn);
		break;

	case STATE_ASYNC_ONGOING:
		connection_complete_async_io(conn);
		break;

	default:
		printf("shouldn't get here %d\n", conn->state);
	}
}

void handle_output(struct connection *conn)
{
	switch (conn->state) {
	case STATE_SENDING_DATA:
		if (conn->res_type == RESOURCE_TYPE_STATIC)
			connection_send_static(conn);
		else
			connection_send_dynamic(conn);
		break;

	case STATE_SENDING_HEADER:
		// trimitem header-ul pe socket
		connection_send_data(conn);
		if (conn->send_pos == conn->send_len) {
			if (conn->res_type == RESOURCE_TYPE_STATIC) {
				// pentru fisiere statice, intram in trimitere de date prin sendfile
				conn->state = STATE_SENDING_DATA;
				connection_send_static(conn);
			} else if (conn->res_type == RESOURCE_TYPE_DYNAMIC) {
				// dupa ce s-a trimis header-ul complet, pornim citirea asincrona
				if (conn->file_size > 0) {
					w_epoll_update_ptr_in(epollfd, conn->sockfd, conn);
					connection_start_async_io(conn);
				} else {
					conn->state = STATE_DATA_SENT;
					connection_remove(conn);
				}
			}
		}
		break;

	case STATE_SENDING_404:
		connection_send_data(conn);
		break;

	case STATE_ASYNC_ONGOING:
		break;

	case STATE_DATA_SENT:
		connection_remove(conn);
		break;

	case STATE_404_SENT:
		connection_remove(conn);
		break;

	case STATE_CONNECTION_CLOSED:
		connection_remove(conn);
		break;

	default:
		ERR("Unexpected state\n");
		exit(1);
	}
}

void handle_client(uint32_t event, struct connection *conn)
{
	if (event == EPOLLIN) {
		handle_input(conn);
		// daca s-a terminat o citire AIO, avem date de trimis
		if (conn->state == STATE_SENDING_DATA)
			w_epoll_update_ptr_out(epollfd, conn->sockfd, conn);
	}
	if (event == EPOLLOUT)
		handle_output(conn);
}

int main(void)
{
	struct sockaddr_in adresa_server;

	// creem descriptorul de la epoll
	epollfd = epoll_create(EPOLL_QUEUE_LEN);

	// avem initializat contextul ctx static
	io_setup(EPOLL_QUEUE_LEN, &ctx);

	// creem socket-ul
	int sockfd = socket(AF_INET, SOCK_STREAM, 0);

	if (sockfd < 0) {
		printf("Eroare la crearea socket-ului pentru server");
		exit(1);
	}

	int enable = 1;

	setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int));

	memset(&adresa_server, 0, sizeof(adresa_server));
	adresa_server.sin_family = AF_INET;
	adresa_server.sin_port = htons(AWS_LISTEN_PORT);
	adresa_server.sin_addr.s_addr = INADDR_ANY;

	int return_socket;
	// assigning a name to the socket
	return_socket = bind(sockfd, (const struct sockaddr *)&adresa_server, sizeof(adresa_server));
	if (return_socket != 0) {
		perror("bind failed");
		exit(1);
	}

	int return_listen;
	// folosim listen pentru a asculta conexiunile
	return_listen = listen(sockfd, 10);
	if (return_listen != 0) {
		// inseamna ca a survenit o eroare
		printf("a survenit o eroare");
		exit(1);
	}
	listenfd = sockfd;

	w_epoll_add_fd_in(epollfd, listenfd);

	/* Uncomment the following line for debugging. */
	// dlog(LOG_INFO, "Server waiting for connections on port %d\n", AWS_LISTEN_PORT);

	struct epoll_event evenimente[EPOLL_QUEUE_LEN];
	/* server main loop */
	while (1) {
		int epoll_wait_ret = epoll_wait(epollfd, evenimente, EPOLL_QUEUE_LEN, -1);

		if (epoll_wait_ret < 0) {
			printf("eroare");
			exit(1);
		}

		for (int i = 0; i < epoll_wait_ret; i++) {
			struct epoll_event eveniment_curent = evenimente[i];

			if ((eveniment_curent.data.fd == listenfd) && ((eveniment_curent.events & EPOLLIN) != 0)) {
				handle_new_connection();
			} else {
				struct connection *conn = (struct connection *)eveniment_curent.data.ptr;

				handle_client(eveniment_curent.events, conn);
			}
		}
	}
	return 0;
}
