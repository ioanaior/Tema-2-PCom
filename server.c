#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <limits.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include "header.h"

int max(int a, int b)
{
	if (a > b)
		return a;
	return b;
}

int matches_topic(char *subscription, char *topic)
{
	// verificam mai intai daca topic-ul contine wildcards,
	// altfel nu ne intereseaza
	int contains_wildcard = 0;
	if (strchr(topic, '*') != NULL || strchr(topic, '+') != NULL)
		contains_wildcard = 1;

	if (!contains_wildcard)
		return -1;

	// daca avem wildcard(uri):
	// implementam logica pentru potrivirea lor
	int i = 0, j = 0;

	while (i < strlen(subscription) && j < strlen(topic))
	{
		// daca caracterul curent din subscription este '*' sau '+',
		// continuam pana gasim urmatorul '/'
		if (subscription[i] == '*' || subscription[i] == '+')
		{
			i++;
			while (j < strlen(topic) && topic[j] != '/')
				j++;
			continue;
		}
		// verificam daca caracterele la care am ajuns sunt identice
		if (subscription[i++] != topic[j++])
			// nu exista potrivire
			return 0;
	}

	// daca NU au fost ambele siruri parcurse complet, nu exista potivire
	if (i == strlen(subscription) || j == strlen(topic))
	{
		return 0;
	}

	// in orice alt caz, exista potrivire
	return 1;
}

void add_subscription(struct client *client, char *topic)
{
	int contains_wildcard = 0;
	if (strchr(topic, '*') != NULL || strchr(topic, '+') != NULL)
		contains_wildcard = 1;

	if (!contains_wildcard)
		return;

	// implementam logica pentru wildcard-uri
	// parcurgem toate abonarile clientului si verificam daca exista
	// deja o abonare similara
	for (int i = 0; i < client->topics_len; i++)
	{
		if (strcmp(client->topics[i], topic) == 0)
		{
			return;
		}
	}
	// adaugam abonarea la topic folosind wildcard-uri
	strcpy(client->topics[client->topics_len++], topic);
}

void process_udp_packet(int udp_sock, struct sockaddr_in udp_addr, socklen_t udp_len, struct client *clients, int socket_max, fd_set fd_out, char buffer[])
{
	// ne gandim la structura-desen din enunt
	// serverul functioneaza ca un intermediar de informatie
	// intre clientii UDP si clientii TCP
	struct udp_struct *udp = (struct udp_struct *)buffer; // ce primim de la clientii UDP
	struct tcp_struct tcp;								  // ce trimitem catre clientii TCP

	DIE(recvfrom(udp_sock, buffer, sizeof(struct udp_struct), 0, (struct sockaddr *)&udp_addr, &udp_len) < 0, "eroare la recvfrom UDP");

	// pregatim structura TCP pentru a fi trimisa
	tcp.port = htons(udp_addr.sin_port);
	char udp_ip[16];
	strcpy(udp_ip, inet_ntoa(udp_addr.sin_addr));
	strcpy(tcp.ip, udp_ip);
	strcpy(tcp.topic, udp->topic);
	// adaugam terminatorul de sir la topic
	tcp.topic[50] = '\0';

	// daca tipul de date al pachetului UDP este INT
	if (udp->type == 0)
	{
		strcpy(tcp.type, "INT");

		// adunam 1 pentru ca primul octet este de semn
		char *udp_payload = udp->buff + 1;
		uint32_t *data_uint32 = (uint32_t *)udp_payload;
		uint32_t network_order_data = *data_uint32;
		// numarul fara semn (momentan)
		int value = ntohl(network_order_data);

		if (udp->buff[0] == 1) // numar negativ
			value *= -1;

		sprintf(tcp.buff, "%d", value);
	}
	// daca tipul de date al pachetului UDP este SHORT_REAL
	if (udp->type == 1)
	{
		strcpy(tcp.type, "SHORT_REAL");

		// extragem numarul in format de 16 biti in bufferul UDP
		char *udp_payload = udp->buff;
		uint16_t *data_uint16 = (uint16_t *)udp_payload;
		uint16_t network_order_data = *data_uint16;

		// convertim numarul din formatul retelei in formatul
		// pe care ni-l dorim (short)
		uint16_t value = ntohs(network_order_data);

		// acum value reprezinta numarul inmultit cu 100,
		// de aceea impartim ca sa ajungem la valoarea reala
		float real_value = abs(value);
		real_value /= 100;

		// formatam valoarea reala ca un sir de caractere cu doua
		// zecimale si o atribuim lui tcp.buff
		sprintf(tcp.buff, "%.2f", real_value);
	}
	// daca tipul de date al pachetului UDP este FLOAT
	if (udp->type == 2)
	{
		strcpy(tcp.type, "FLOAT");

		// adunam 1 pentru ca primul octet este de semn
		char *udp_payload = udp->buff + 1;
		uint32_t *data_uint32 = (uint32_t *)udp_payload;
		uint32_t network_order_data = *data_uint32;

		// conf. enunt: udp->buff[5] = modulul puterii negative a lui 10 cu care
		// trebuie inmultit modulul pentru a obtine numarul original (in modul)
		int power = 1;
		for (int i = 0; i < udp->buff[5]; i++)
			power *= 10;

		// convertim numarul din formatul retelei in formatul dorit si ajustam virgula
		double real_value = ntohl(network_order_data) / (double)power;

		// again, daca primul byte de semn ne zice ca numarul este negativ
		if (udp->buff[0] == 1)
			real_value *= -1;

		// formatam valoarea ca un sir de caractere si o atribuim lui tcp.buff
		sprintf(tcp.buff, "%lf", real_value);
	}
	// daca tipul de date al pachetului UDP este STRING
	if (udp->type == 3)
	{
		strcpy(tcp.type, "STRING");
		// nu mai este nevoie de nicio prelucrare
		strcpy(tcp.buff, udp->buff);
	}

	// parcurgem toti clientii (desi aici ne intereseaza doar cei TCP)
	for (int i = 0; i <= socket_max; i++)
		for (int k = 0; k < clients[i].topics_len; k++)
		{
			// daca clientul TCP e abonat la topicul pe care l-am primit
			// SI e conectat (ca nu vrem sa primeasca mesaj daca e offline)
			if (strcmp(tcp.topic, clients[i].topics[k]) == 0 && clients[i].connected)
			{
				// ii trimitem informatia
				// if (matches_topic(clients[i].topics[k], tcp.topic) == 1)
				DIE(send(clients[i].socket, &tcp, sizeof(struct tcp_struct), 0) < 0,
					"eroare la transmiterea mesajui catre clientul TCP");
				break;
			}
		}
}

void add_new_client(struct client *clients, int socket_new_client, char buffer[], int *socket_max, fd_set *fd_out)
{
	// adaugam noul socket in setul de descriptori de fisiere
	FD_SET(socket_new_client, fd_out);
	*socket_max = max(*socket_max, socket_new_client);

	strncpy(clients[*socket_max].id, buffer, sizeof(clients[*socket_max].id) - 1);
	// ne asiguram ca stringul se termina in '\0'
	clients[*socket_max].id[sizeof(clients[*socket_max].id) - 1] = '\0';
	clients[*socket_max].socket = socket_new_client;
	clients[*socket_max].connected = 1;
}

void reconnect_client(struct client *clients, int index, int socket_reconnected_client, fd_set *fd_out)
{
	FD_SET(socket_reconnected_client, fd_out);
	clients[index].socket = socket_reconnected_client;
	clients[index].connected = 1;
}

void handle_tcp_connection(int tcp_sock, struct sockaddr_in new_tcp, socklen_t udp_len, struct client *clients, int *socket_max, fd_set *fd_out, char buffer[])
{
	int new_socket = accept(tcp_sock, (struct sockaddr *)&new_tcp, &udp_len);
	DIE(new_socket < 0, "eroare la acccept socket tcp");

	// Primim id-ul clientului
	recv(new_socket, buffer, 10, 0);

	// cautam clientul in vectorul existent
	int client_index = -1;
	for (int i = 0; i <= *socket_max; i++)
	{
		if (strcmp(clients[i].id, buffer) == 0)
		{
			client_index = i;
			break;
		}
	}

	if (client_index < 0)
	{
		// client nou? il adaugam! si afisam mesajul
		add_new_client(clients, new_socket, buffer, socket_max, fd_out);
		printf("New client %s connected from %s:%d.\n", clients[*socket_max].id, inet_ntoa(new_tcp.sin_addr), ntohs(new_tcp.sin_port));
	}
	else
	{
		// client existent?
		if (clients[client_index].connected)
		{
			// daca este deja conectat
			// nu are rost sa mai lasam socketul deschis
			close(new_socket);
			printf("Client %s already connected.\n", clients[client_index].id);
		}
		else
		{
			// daca e un client deconectat care vrea sa se conecteze
			reconnect_client(clients, client_index, new_socket, fd_out);
			printf("New client %s connected from %s:%d.\n", clients[client_index].id, inet_ntoa(new_tcp.sin_addr), ntohs(new_tcp.sin_port));
		}
	}
}

int main(int argc, char **argv)
{

	setvbuf(stdout, NULL, _IONBF, BUFSIZ);

	DIE(argc != 2, "numar gresit de argumente!");

	int not_exit = 1;
	int server_port = atoi(argv[1]);

	// avem un vector de clienti pe care il alocam (am presupus dimensiunea maxima 1000)
	struct client *clients = malloc(1000 * sizeof(struct client));

	// initializam cele doua socket-uri (TCP si UDP)
	int tcp_sock = socket(AF_INET, SOCK_STREAM, 0);
	int udp_sock = socket(AF_INET, SOCK_DGRAM, 0);

	struct sockaddr_in serv_addr;
	struct sockaddr_in udp_addr;
	struct sockaddr_in new_tcp;

	// initializam serv_addr si udp_addr (la fel)
	udp_addr.sin_port = serv_addr.sin_port = htons(server_port);
	udp_addr.sin_addr.s_addr = serv_addr.sin_addr.s_addr = INADDR_ANY;
	udp_addr.sin_family = serv_addr.sin_family = AF_INET;

	bind(tcp_sock, (struct sockaddr *)&serv_addr, sizeof(struct sockaddr));
	bind(udp_sock, (struct sockaddr *)&udp_addr, sizeof(struct sockaddr));

	// setam optiunile socket-ului TCP (cu optiunea TCP_NODELAY pentru
	// activarea/dezactivarea algoritmului Nagle)
	int enable = 1;
	setsockopt(tcp_sock, IPPROTO_TCP, TCP_NODELAY, (char *)&enable, sizeof(int));

	// serverul "asculta" (se face socketul pasiv)
	int tcp_listen = listen(tcp_sock, INT_MAX);
	DIE(tcp_listen < 0, "eroare de conectare TCP (listen)");

	fd_set fd_out;
	FD_SET(tcp_sock, &fd_out);
	FD_SET(udp_sock, &fd_out);
	FD_SET(STDIN_FILENO, &fd_out);

	socklen_t udp_len = 16; // sizeof(struct sockaddr)

	int socket_max = max(tcp_sock, udp_sock);

	while (not_exit)
	{

		// folosim functia select pentru a astepta evenimente pe unul
		// sau mai multi descriptori
		// functia select blocheaza executia
		// programului pana cand apare un eveniment pe unul dintre descriptorii
		// monitorizati sau pana cand expira timpul de asteptare
		int socket_num_select = socket_max + 1;
		fd_set fd_select = fd_out;
		DIE(select(socket_num_select, &fd_select, NULL, NULL, NULL) < 0, "eroare la select fd");

		char buffer[102];

		// parcurgem toate socketurile
		for (int i = 0; i <= socket_max; i++)
		{
			int socket_recv = i;
			if (FD_ISSET(socket_recv, &fd_select))
			{
				// golim bufferul la fiecare pas
				memset(buffer, 0, 102);

				if (socket_recv == udp_sock)
				{
					process_udp_packet(udp_sock, udp_addr, udp_len, clients, socket_max, fd_out, buffer);
				}
				if (socket_recv == tcp_sock)
				{
					handle_tcp_connection(tcp_sock, new_tcp, udp_len, clients, &socket_max, &fd_out, buffer);
				}
				// socketul serverului (stdin)
				if (socket_recv == STDIN_FILENO)
				{
					// citim mesajul de la tastatura
					fgets(buffer, 100, stdin);
					// si verificam daca am primit comanda de oprire
					if (strncmp(buffer, "exit", 4) == 0)
					{
						not_exit = 0;
						// iesim din bucla
						break;
					}
				}
				// altfel, e posibil sa avem clienti noi
				if (socket_recv != udp_sock && socket_recv != tcp_sock && socket_recv != STDIN_FILENO)
				{
					// am primit pachet nou de la client
					if (recv(socket_recv, buffer, sizeof(struct command_tcp), 0))
					{
						client *command_client;
						struct command_tcp *packet_received = (struct command_tcp *)buffer;

						// cautam clientul dupa socket
						for (int j = 0; j <= socket_max; j++)
						{
							if (socket_recv == clients[j].socket)
							{
								command_client = &clients[j];
								break;
							}
						}

						// daca pachetul este de tip subscribe ('S')
						if (packet_received->type == 'S')
						{
							// verificam daca suntem deja abonati la topicul respectiv
							int topic_index = -1;
							for (int j = 0; j < command_client->topics_len; j++)
							{
								if (strcmp(command_client->topics[j], packet_received->topic) == 0)
								{
									topic_index = j;
									break;
								}
							}

							// daca nu, ne abonam la topic (il adaugam in vectorul de topicuri ale clientului)
							if (topic_index == -1)
								strcpy(command_client->topics[command_client->topics_len++], packet_received->topic);
							// add_subscription(command_client, packet_received->topic);
						}
						// daca pachetul este de tip unsubscribe ('U')
						if (packet_received->type == 'U')
						{
							// cautam prin lista de topicuri a clientului si il eliminam pe cel cautat
							for (int j = 0; j < command_client->topics_len; j++)
							{
								if (strcmp(command_client->topics[j], packet_received->topic) == 0)
								{
									int topic_index = j;
									// deplasam elementele ulterioare pentru a umple golul lasat de elementul eliminat
									for (int k = topic_index; k < command_client->topics_len - 1; k++)
										strcpy(command_client->topics[k], command_client->topics[k + 1]);
									// reducem lungimea listei de topicuri
									command_client->topics_len--;
									break;
								}
							}
						}
						// daca pachetul este de tip exit ('E')
						if (packet_received->type == 'E')
						{ 
							// identificam clientul si il eliminam din lista de clienti
							for (int j = 0; j <= socket_max; j++)
								if (clients[j].socket == socket_recv)
								{
									FD_CLR(socket_recv, &fd_out);
									clients[j].socket = -1;
									clients[j].connected = 0;
									printf("Client %s disconnected.\n", clients[j].id);
									break;
								}
						}
					}
				}
			}
		}
	}
	for (int i = 0; i <= socket_max; i++)
	{
		if (FD_ISSET(i, &fd_out))
			close(i);
	}

	return 0;
}