#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <netdb.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <errno.h>
#include "header.h"

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, BUFSIZ);

    DIE(argc != 4, "numar gresit de argumente!");
    int port_client = atoi(argv[3]);

    struct sockaddr_in server_data;
    server_data.sin_family = AF_INET;
    server_data.sin_port = htons(port_client);

    int tcp_socket = socket(AF_INET, SOCK_STREAM, 0);
    DIE(tcp_socket < 0, "eroare la functia socket");

    int socket_max = tcp_socket;

    inet_aton(argv[2], &server_data.sin_addr);

    fd_set fd_out;
    FD_SET(socket_max, &fd_out);
    FD_SET(STDIN_FILENO, &fd_out);

    // ne conectam la server prin socketul TCP
    DIE(connect(socket_max, (struct sockaddr *)&server_data, sizeof(server_data)) < 0, "eroare la connect server");

    // trimitem IP-ul cu send
    DIE(send(socket_max, argv[1], 10, 0) < 0, "eroare la send IP");

    // pachetul care va fi trimis la server
    struct command_tcp command_for_server;

    int enable = 1;
    setsockopt(socket_max, IPPROTO_TCP, TCP_NODELAY, (char *)&enable, sizeof(int));

    // putem aveam inputuri fie de la SERVER, fie de la STDIN
    while (1)
    {
        fd_set fd_aux = fd_out;
        select(socket_max + 1, &fd_aux, NULL, NULL, NULL);

        // daca primim informatie de la SERVER
        if (FD_ISSET(socket_max, &fd_aux))
        {
            int tcp_pack_len = sizeof(struct tcp_struct);
            char info_server[tcp_pack_len];

            if (!recv(socket_max, info_server, sizeof(struct tcp_struct), 0))
                break;

            struct tcp_struct *pack_send = (struct tcp_struct *)info_server;
            printf("%s:%u - %s - %s - %s\n", pack_send->ip, pack_send->port, pack_send->topic, pack_send->type, pack_send->buff);
        }

        // daca avem input de la STDIN
        if (FD_ISSET(STDIN_FILENO, &fd_aux))
        {
            char command[101];
            fgets(command, 101, stdin);

            // daca se doreste subscribe la un topic
            if (strncmp(command, "subscribe", 9) == 0)
            {
                char *string = strtok(command, " ");
                string = strtok(NULL, " ");
                strncpy(command_for_server.topic, string, strlen(string));
                command_for_server.topic[strlen(string) - 1] = '\0';

                command_for_server.type = 'S';

                DIE(send(socket_max, &command_for_server, sizeof(struct command_tcp), 0) < 0, "send subscribe");

                printf("Subscribed to topic %s\n", command_for_server.topic);
            }
            // daca se doreste unsubscribe de la un topic
            if (strncmp(command, "unsubscribe", 11) == 0)
            {
                char *string = strtok(command, " ");
                string = strtok(NULL, " ");
                strncpy(command_for_server.topic, string, strlen(string));
                command_for_server.topic[strlen(string) - 1] = '\0';

                command_for_server.type = 'U';

                DIE(send(socket_max, &command_for_server, sizeof(struct command_tcp), 0) < 0, "send unsubscribe");

                printf("Unsubscribed from topic %s\n", command_for_server.topic);
            }
            if (strncmp(command, "exit", 4) == 0)
            {
                command_for_server.type = 'E';
                DIE(send(socket_max, &command_for_server, sizeof(struct command_tcp), 0) < 0, "send exit");
                break;
            }
            if (strncmp(command, "subscribe", 9) != 0 &&
                strncmp(command, "unsubscribe", 11) != 0 &&
                strncmp(command, "exit", 4) != 0)
            {
                fprintf(stderr, "nu ai introdus un input potrivit!!\n");
                break;
            }
        }
    }

    close(socket_max);
    return 0;
}
