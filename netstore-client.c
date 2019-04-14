#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdlib.h>

#include "err.h"

#define DEF_PORT_NUM "6543"
#define BUFFER_SIZE 512000
#define MAX_FILE_NAME_LEN 257
#define FILES_NAMES_REQUEST 1
#define FILE_FRAGMENT_REQUEST 2
#define SERVER_REFUSAL 2
#define WRONG_FILE_NAME 1
#define WRONG_FRAGMENT_ADDRESS 2
#define NO_FRAGMENT_SIZE 3

#define DEBUG

struct __attribute__((__packed__)) file_fragment_request {
  uint16_t msg_type;
  uint32_t start_addr;
  uint32_t bytes_to_send;
  uint16_t file_name_len;
};

struct __attribute__((__packed__)) msg_server {
    uint16_t msg_type;
    uint32_t param;
};

struct user_command {
    int32_t file_id;
    uint32_t start_addr;
    uint32_t end_addr;
};

size_t min(size_t a, size_t b) {
    return a < b ? a : b;
}

void check_argc(int argc, char *argv[]) {
    if (argc != 2 && argc != 3)
        fatal("Usage %s <nazwa-lub-adres-IP4-serwera> [<numer-portu-serwera>]", argv[0]);
}

/******************************************** CONNECTION ******************************************/

void set_addr_hints(struct addrinfo *addr_hints) {
    memset(addr_hints, 0, sizeof(struct addrinfo));
    addr_hints->ai_family = AF_INET; // IPv4
    addr_hints->ai_socktype = SOCK_STREAM;
    addr_hints->ai_protocol = IPPROTO_TCP;

#ifdef DEBUG_DETAILED
    printf("addr_hints set\n");
#endif
}

void get_address_info(char const *host, char const *port, struct addrinfo *addr_hints, struct addrinfo **addr_result) {
    int err;

    err = getaddrinfo(host, port, addr_hints, addr_result);
    if (err == EAI_SYSTEM) // system error
      syserr("getaddrinfo: %s", gai_strerror(err));
    else if (err != 0) // other error (host not found, etc.)
      fatal("getaddrinfo: %s", gai_strerror(err));

#ifdef DEBUG_DETAILED
    printf("address info acquired\n");
#endif
}

void init_socket(int *sockfd, struct addrinfo *addr_result) {
    *sockfd = socket(addr_result->ai_family, addr_result->ai_socktype, addr_result->ai_protocol);
    if (*sockfd < 0)
        syserr("socket");

#ifdef DEBUG_DETAILED
    printf("socket initialized\n");
#endif
}

void connect_socket(int sockfd, struct addrinfo *addr_result) {
    if (connect(sockfd, addr_result->ai_addr, addr_result->ai_addrlen) < 0)
        syserr("connect");
}

void connect_with_server(int *sockfd, char const *host, char const *port) {
#ifdef DEBUG
    printf("port num = %s\n"
           "trying to connect to the server...\n", port);
#endif
    struct addrinfo addr_hints;
    struct addrinfo *addr_result = NULL;

    set_addr_hints(&addr_hints);
    get_address_info(host, port, &addr_hints, &addr_result);
    init_socket(sockfd, addr_result);
    connect_socket(*sockfd, addr_result);
    freeaddrinfo(addr_result);

#ifdef DEBUG
    printf("server connected\n");
#endif

}

void close_socket(int sockfd) {
    if (close(sockfd) < 0)
        syserr("close");
}

/********************************************* MESSAGES *******************************************/

void send_files_names_request(int sockfd) {
#ifdef DEBUG
    printf("sending files names request\n");
#endif

    uint16_t msg_code = htons(FILES_NAMES_REQUEST);
    size_t msg_len = sizeof(msg_code);

    if (write(sockfd, &msg_code, msg_len) != msg_len)
        syserr("partial / failed write");

#ifdef DEBUG
    printf("files names request sent...\n");
#endif
}

void handle_server_refusal(char const *msg) {
    printf("%s\n", msg);
    exit(0);
}

void parse_server_msg(struct msg_server *msg) {
#ifdef DEBUG_DETAILED
    printf("parsing server msg\n");
#endif
    msg->msg_type = ntohs(msg->msg_type);
    msg->param = ntohl(msg->param);
}

void check_server_msg(struct msg_server *msg) {
#ifdef DEBUG_DETAILED
    printf("checking server msg\n");
#endif
    if (msg->msg_type == SERVER_REFUSAL) {
        switch (msg->param) {
            case WRONG_FILE_NAME:
                handle_server_refusal("file transfer: wrong file name");

            case WRONG_FRAGMENT_ADDRESS:
                handle_server_refusal("file transfer: wrong fragment address");

            case NO_FRAGMENT_SIZE:
                handle_server_refusal("file transfer: no fragment size");

            default:
                handle_server_refusal("server_msg unknown error");
        }
    }
}

/// Read exactly 6 bits ~ 2 + 4
void receive_server_msg(int sockfd, struct msg_server *msg) {
#ifdef DEBUG
    printf("receiving server msg...\n");
#endif
    ssize_t read_curr;
    size_t read_all = 0, remains;

    do {
        remains = sizeof(struct msg_server) - read_all;
        read_curr = read(sockfd, msg + read_all, remains);
        if (read_curr < 0)
            syserr("receiving message from server");

        read_all += read_curr;
    } while (read_curr > 0);

    parse_server_msg(msg);
    check_server_msg(msg);
#ifdef DEBUG
    printf("server msg received: %d %d\n", msg->msg_type, msg->param);
#endif
}

void receive_files_names(int sockfd, char **files_names_buffer, uint32_t *list_len) {
#ifdef DEBUG
    printf("receive_files_names\n");
#endif
    ssize_t read_curr;
    size_t read_all = 0, remains;
    struct msg_server server_msg;

    receive_server_msg(sockfd, &server_msg);
    *list_len = server_msg.param;
    if (!(*files_names_buffer = malloc(sizeof(char) * (*list_len))))
        syserr("malloc");

    do {
        remains = *list_len - read_all;
        read_curr = read(sockfd, *files_names_buffer + read_all, remains);
        if (read_curr < 0)
            syserr("reading files names list");

        read_all += read_curr;
    } while (read_curr > 0);

#ifdef DEBUG
    printf("end of receive_files_names\n");
#endif
}

void display_files_names_list(char files_names_buffer[], int32_t len) {
    uint16_t id = 0;
    for (int i = 0; i < len; i++) {
        printf("%d.", id++);
        while (i < len && files_names_buffer[i] != '|')
            printf("%c", files_names_buffer[i++]);
        printf("\n");
    }
}

void read_user_command(struct user_command *comm) {
//  comm->file_id = 2;
//  comm->start_addr = 0;
//  comm->end_addr = 1000000;
  scanf("%d", &comm->file_id);
  scanf("%d", &comm->start_addr);
  scanf("%d", &comm->end_addr);
}


/// Returns file's name length
/// file_name ends with \0
uint16_t save_file_name(int32_t file_id, char file_name[], const char files_names_buffer[]) {
    int32_t curr_file_id = 0;
    uint16_t file_name_length = 0;
    int32_t i = 0, j = 0;

    while (curr_file_id < file_id) {
        if (files_names_buffer[i] == '|')
            curr_file_id++;
        i++;
    }

    while (files_names_buffer[i] != '|') {
        file_name[j++] = files_names_buffer[i++];
        file_name_length++;
    }
    file_name[file_name_length] = '\0';

    return file_name_length;
}


/// Returns data length in buffer
size_t fill_buffer_with_fragment_request(const struct user_command *comm, char file_name[], uint16_t file_name_length, char buffer[]) {
#ifdef DEBUG
    printf("setting file fragment request\n");
#endif
    struct file_fragment_request msg;

    msg.msg_type = htons(FILE_FRAGMENT_REQUEST);
    msg.start_addr = htonl(comm->start_addr);
    msg.bytes_to_send = htonl(comm->end_addr - comm->start_addr);
    msg.file_name_len = htons(file_name_length);
    memcpy(buffer, &msg, sizeof(struct file_fragment_request));
    memcpy(buffer + sizeof(struct file_fragment_request), file_name, file_name_length);

#ifdef DEBUG
    printf("msg: %d %d %d %d\n", ntohs(msg.msg_type), ntohl(msg.start_addr), ntohl(msg.bytes_to_send), ntohs(msg.file_name_len));
    printf("\nfile fragment request sent\n");
#endif
    return sizeof(struct file_fragment_request) + file_name_length;
}

void send_file_fragment_request(int sockfd, struct user_command* comm, char files_name[], uint16_t file_name_length, char buffer[]) {
    size_t data_len = fill_buffer_with_fragment_request(comm, files_name, file_name_length, buffer);

    if (write(sockfd, buffer, data_len) != data_len)
        syserr("partial / failed write");
}

int open_tmp_directory(DIR **dir_stream) {
    int dir_fd;
    struct stat stat_buff = {0};

    if (stat("./tmp", &stat_buff) == -1) {
        if (mkdir("./tmp", 0777) < 0)
            syserr("dir creation");
    }

    *dir_stream = opendir("./tmp");
    if (*dir_stream == NULL)
        syserr("opening directory");

    if ((dir_fd = dirfd(*dir_stream)) < 0)
        syserr("dirfd");

    return dir_fd;
}

void save_file_fragment(int fd, char buffer[], size_t data_len, uint32_t *start_addr) {
#ifdef DEBUG
    printf("save_file_fragment\n");
#endif

    if (lseek(fd, *start_addr, SEEK_SET) < 0)
        syserr("lseek");
    if (write(fd, buffer, data_len) != data_len)
        syserr("partial / failed write to file");
    *start_addr += data_len;

#ifdef DEBUG
    printf("end of save_file_fragment\n");
#endif
}

void receive_file_fragment(int sockfd, char file_name[], char buffer[], uint32_t start_addr) {
#ifdef DEBUG
    printf("receive_file_fragment\n");
#endif
    ssize_t read_curr_inner;
    size_t read_all = 0, read_all_inner = 0, remains, remains_inner, bytes_to_receive;
    struct msg_server msg;
    DIR *dir_stream;
    int fd, dir_fd;

    receive_server_msg(sockfd, &msg);
    bytes_to_receive = msg.param;

    dir_fd = open_tmp_directory(&dir_stream);
    if ((fd = openat(dir_fd, file_name, O_CREAT|O_RDWR, 0777)) < 0)
        syserr("file opening");

    do {
        remains = bytes_to_receive - read_all;

        read_all_inner = 0;
        do {
            remains_inner = min(BUFFER_SIZE, remains) - read_all_inner;
            if ((read_curr_inner = read(sockfd, buffer + read_all_inner, remains_inner)) < 0)
                syserr("file fragment reading");

            read_all_inner += read_curr_inner;
        } while (read_curr_inner > 0);

        save_file_fragment(fd, buffer, read_all_inner, &start_addr);
        read_all += read_all_inner;
    } while (read_all_inner > 0);


    if (close(fd) < 0)
        syserr("file closing");
    if (closedir(dir_stream) < 0)
        syserr("directory closing");

#ifdef DEBUG
    printf("end of receive_file_fragment\n");
#endif
}

int main(int argc, char *argv[]) {
    int sockfd;
    char *files_names_buffer = NULL;
    char buffer[BUFFER_SIZE];
    char file_name[MAX_FILE_NAME_LEN];
    uint32_t list_len = 0;
    uint16_t file_name_length;
    struct user_command comm;

    check_argc(argc, argv);
    char const *port = argc == 3 ? argv[2] : DEF_PORT_NUM;

    connect_with_server(&sockfd, argv[1], port);
    send_files_names_request(sockfd);
    receive_files_names(sockfd, &files_names_buffer, &list_len);
    display_files_names_list(files_names_buffer, list_len);
    read_user_command(&comm);
    file_name_length = save_file_name(comm.file_id, file_name, files_names_buffer);
    free(files_names_buffer); // No more needed
    send_file_fragment_request(sockfd, &comm, file_name, file_name_length, buffer);
    receive_file_fragment(sockfd, file_name, buffer, comm.start_addr);

    return 0;
}