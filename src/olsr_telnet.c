/*
 * olsr_telnet.h
 *
 *  Created on: 22.03.2013
 *      Author: Christian Pointner <equinox@chaos-at-home.org>
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <ctype.h>

#ifdef __linux__
#include <fcntl.h>
#endif /* __linux__ */

#include "olsr.h"
#include "olsr_types.h"
#include "scheduler.h"

#include "olsr_telnet.h"

#ifdef _WIN32
#define close(x) closesocket(x)
#define SHUT_RD  SD_RECEIVE
#define SHUT_WR  SD_SEND
#define SHUT_RDWR  SD_BOTH
#endif /* _WIN32 */


/* forward declarations for internal functions  */
static void telnet_action(int, void *, unsigned int);
static void telnet_client_cleanup(struct telnet_server*);
static struct telnet_client* telnet_client_add(struct telnet_server*, int);
static void telnet_client_remove(struct telnet_client*);
static void telnet_client_free(struct telnet_client*);
static void telnet_client_action(int, void *, unsigned int);
static void telnet_client_handle_cmd(struct telnet_client*, char*);
static void telnet_client_fetch_lines(struct telnet_client*, ssize_t);
static void telnet_client_read(struct telnet_client*);
static void telnet_client_write(struct telnet_client*);
static void telnet_client_linger_timeout(void*);
static int get_port(struct telnet_server* s);

#define BUF_SIZE 1024
#define TELNET_LINGER_TIMEOUT 42000

/***********************************************************************/
/*   External API                                                      */
/***********************************************************************/

/**
 * Completes the telnet server struct and prepares all values for a call to
 * olsr_telnet_init().
 *
 * @param s the telnet_server struct which will get initialized
 * @param listen_ip the address to bind/listen to
 * @param port the port to bind/listen to
 *
 * @return Nothing
 */
void
olsr_telnet_prepare(struct telnet_server* s, union olsr_ip_addr listen_ip, int port)
{
  if(!s)
    return;

  s->fd = -1;
  s->prompt = NULL;
  s->clients = NULL;
  s->cmd_table = NULL;
  s->default_client_buf_size = BUF_SIZE;

      /* complete the socket structure */
  memset(&(s->sst), 0, sizeof(s->sst));
  if (olsr_cnf->ip_version == AF_INET) {
    s->sst.in4.sin_family = AF_INET;
    s->addrlen = sizeof(struct sockaddr_in);
#ifdef SIN6_LEN
    s->sst.in4.sin_len = s->addrlen;
#endif /* SIN6_LEN */
    s->sst.in4.sin_addr.s_addr = listen_ip.v4.s_addr;
    s->sst.in4.sin_port = htons(port);
  } else {
    s->sst.in6.sin6_family = AF_INET6;
    s->addrlen = sizeof(struct sockaddr_in6);
#ifdef SIN6_LEN
    s->sst.in6.sin6_len = s->addrlen;
#endif /* SIN6_LEN */
    s->sst.in6.sin6_addr = listen_ip.v6;
    s->sst.in6.sin6_port = htons(port);
  }
}

/**
 * Initialize the telnet server socket and bind/listen to the specified address/port.
 * On error the telnet_server struct will be unchanged.
 *
 * @param s a telnet_server struct which got initialized by olsr_telnet_prepare()
 *
 * @return On success 0 is returned, any other value means error
 */
int
olsr_telnet_init(struct telnet_server* s)
{
  uint32_t yes = 1;

  if(!s)
    return 1;

  /* Init telnet socket */
  if ((s->fd = socket(olsr_cnf->ip_version, SOCK_STREAM, 0)) == -1) {
    OLSR_PRINTF(1, "(TELNET) socket()=%s\n", strerror(errno));
    return 1;
  }

  if (setsockopt(s->fd, SOL_SOCKET, SO_REUSEADDR, (char *)&yes, sizeof(yes)) < 0) {
    OLSR_PRINTF(1, "(TELNET) setsockopt()=%s\n", strerror(errno));
    goto close_server_socket;
  }
#if (defined __FreeBSD__ || defined __FreeBSD_kernel__) && defined SO_NOSIGPIPE
  if (setsockopt(s->fd, SOL_SOCKET, SO_NOSIGPIPE, (char *)&yes, sizeof(yes)) < 0) {
    perror("SO_REUSEADDR failed");
    goto close_server_socket;
  }
#endif /* (defined __FreeBSD__ || defined __FreeBSD_kernel__) && defined SO_NOSIGPIPE */

      /* bind the socket to the port number */
  if (bind(s->fd, &(s->sst.in), s->addrlen) == -1) {
    OLSR_PRINTF(1, "(TELNET) bind()=%s\n", strerror(errno));
    goto close_server_socket;
  }

      /* show that we are willing to listen */
  if (listen(s->fd, 1) == -1) {
    OLSR_PRINTF(1, "(TELNET) listen()=%s\n", strerror(errno));
    goto close_server_socket;
  }

      /* Register with olsrd */
  add_olsr_socket(s->fd, &telnet_action, NULL, (void*)s, SP_PR_READ);

  OLSR_PRINTF(2, "(TELNET) listening on port %d\n", get_port(s));

  return 0;

close_server_socket:
  close(s->fd);
  s->fd = -1;
  return 1;
}

/**
 * Closes all client connections and cleans up buffers. Also closes the server
 * socket. The struct may be reused by passing it to olsr_telnet_init() again.
 *
 * @param s a telnet_server struct pointing to the server instance
 *
 * @return Nothing
 */
void
olsr_telnet_exit(struct telnet_server* s)
{
  if(!s)
    return;

  telnet_client_cleanup(s);
      // TODO: cleanup command table???

  if(s->fd != -1) {
    remove_olsr_socket(s->fd, &telnet_action, NULL);
    close(s->fd);
    s->fd = -1;
  }
}

/**
 * Closes the connection to the specified client and cleans up all buffers.
 *
 * @param c a telnet_client struct pointing to the client instance
 * @param now if true client gets removed immediatly, if false the client
 *            gets removed as soon as all output buffers are flushed
 *
 * @return Nothing
 */
void
olsr_telnet_client_quit(struct telnet_client* c, bool now)
{
  if(!c)
    return;

  if(now)
    c->state = destroy;
  else
    c->state = pending;
}

/**
 * Adds a formatted string to the client out buffer. In addition to this
 * all abuf functions may be used on c->out.
 *
 * @param c a telnet_client struct pointing to the client instance
 * @param fmt a printf-style format string
 *
 * @return Nothing
 */
void
olsr_telnet_client_printf(struct telnet_client* c, const char* fmt, ...)
{
  va_list arg_ptr;

  if(!c)
    return;

  va_start(arg_ptr, fmt);
  abuf_vappendf(&(c->out), fmt, arg_ptr);
  va_end(arg_ptr);
}

/***********************************************************************/
/*   End of External API                                               */
/***********************************************************************/
/* Dear plugin developer's: Please don't use the functions below       */

static int
get_port(struct telnet_server* s)
{
  return olsr_cnf->ip_version == AF_INET ? ntohs(s->sst.in4.sin_port) : ntohs(s->sst.in6.sin6_port);
}

static void
telnet_action(int fd, void *data, unsigned int flags __attribute__ ((unused)))
{
  union olsr_sockaddr pin;
  socklen_t addrlen = sizeof(pin);

  char addr[INET6_ADDRSTRLEN];
  int client_fd;

  if ((client_fd = accept(fd, &pin.in, &addrlen)) == -1) {
    OLSR_PRINTF(1, "(TELNET) accept()=%s\n", strerror(errno));
    return;
  }

  if (olsr_cnf->ip_version == AF_INET) {
    if (inet_ntop(olsr_cnf->ip_version, &pin.in4.sin_addr, addr, INET6_ADDRSTRLEN) == NULL)
      addr[0] = '\0';
  } else {
    if (inet_ntop(olsr_cnf->ip_version, &pin.in6.sin6_addr, addr, INET6_ADDRSTRLEN) == NULL)
      addr[0] = '\0';
  }

  OLSR_PRINTF(2, "(TELNET) Connect from %s (client: %d)\n", addr, client_fd);

  telnet_client_add((struct telnet_server*)data, client_fd);
}

static void
telnet_client_cleanup(struct telnet_server* s)
{
  struct telnet_client* c;
  for(c = s->clients; c;) {
    struct telnet_client* deletee = c;
    c = c->next;
    telnet_client_free(deletee);
  }
}

static struct telnet_client*
telnet_client_add(struct telnet_server* s, int fd)
{
  struct telnet_client* c;

  if(!s || fd < 0)
    return NULL;

  c = olsr_malloc(sizeof(struct telnet_client), __func__);
  abuf_init(&(c->out), s->default_client_buf_size);
  abuf_init(&(c->in), s->default_client_buf_size);

  c->fd = fd;
  c->state = active;
  c->server = s;
  c->linger_timer = NULL;
  c->next = s->clients;

  s->clients = c;
  add_olsr_socket(fd, &telnet_client_action, NULL, (void*)c, SP_PR_READ);

  return c;
}

static void
telnet_client_remove(struct telnet_client* c)
{
  struct telnet_server* s = c->server;
  struct telnet_client* ptr;

  if(!s->clients || !c)
    return;

  if(c == s->clients) {
    s->clients = c->next;
  } else {
    for(ptr = s->clients; ptr->next; ptr = ptr->next) {
      if(ptr->next == c) {
        ptr->next = c->next;
        break;
      }
    }
  }
  telnet_client_free(c);
}

static void
telnet_client_free(struct telnet_client* c)
{
  if(c->fd != -1) {
    remove_olsr_socket(c->fd, &telnet_client_action, NULL);
    close(c->fd);
  }
  abuf_free(&(c->out));
  abuf_free(&(c->in));
  if(c->linger_timer)
    olsr_stop_timer(c->linger_timer);

  free(c);
}

static void
telnet_client_handle_cmd(struct telnet_client* c, char* cmd)
{
      // simple echo server
  olsr_telnet_client_printf(c, "%s\n", cmd);
  olsr_telnet_client_quit(c, 0);

  if(c->state != destroy && c->out.len)
    enable_olsr_socket(c->fd, &telnet_client_action, NULL, SP_PR_WRITE);
}

static void
telnet_client_fetch_lines(struct telnet_client* c, ssize_t offset)
{
  ssize_t i;
  for(i = offset; i < c->in.len; ++i) {
    if(c->in.buf[i] == '\n') {
      c->in.buf[i] = 0;
      if(i > 0 && c->in.buf[i-1] == '\r')
        c->in.buf[i-1] = 0;

      telnet_client_handle_cmd(c, c->in.buf);
      if(c->state != active)
        break;

      if(i >= c->in.len) {
        abuf_pull(&(c->in), c->in.len);
        break;
      }

      abuf_pull(&(c->in), i+1);
      offset = 0;
    }
  }
}

static void
telnet_client_action(int fd, void *data, unsigned int flags)
{
  struct telnet_client* c = (struct telnet_client*)data;

  if(!c) {
    remove_olsr_socket(fd, &telnet_client_action, NULL);
    close(fd);
    return;
  }

  if(flags & SP_PR_WRITE)
    telnet_client_write(c);

  if(flags & SP_PR_READ)
    telnet_client_read(c);

  if(c->state == linger && c->linger_timer == NULL) {
    shutdown(c->fd, SHUT_WR);
    c->linger_timer = olsr_start_timer(TELNET_LINGER_TIMEOUT, 0, OLSR_TIMER_ONESHOT, &telnet_client_linger_timeout, (void*)c, 0);
  }
  else if(c->state == destroy)
    telnet_client_remove(c);
}

static void
telnet_client_read(struct telnet_client* c)
{
  char buf[BUF_SIZE];
  ssize_t result = recv(c->fd, (void *)buf, sizeof(buf), 0);
  if (result > 0) {
    ssize_t offset = c->in.len;

    if(c->state != active)
      return;

    abuf_memcpy(&(c->in), buf, result);
    telnet_client_fetch_lines(c, offset);
  }
  else {
    if(result == 0) {
      OLSR_PRINTF(2, "(TELNET) client %i: disconnected\n", c->fd);
      c->state = destroy;
    } else {
      if(errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
        return;

      OLSR_PRINTF(1, "(TELNET) client %i recv(): %s\n", c->fd, strerror(errno));
      c->state = destroy;
    }
  }
}

static void
telnet_client_write(struct telnet_client* c)
{
  ssize_t result = send(c->fd, (void *)(c->out.buf), c->out.len, 0);
  if (result > 0) {
    abuf_pull(&(c->out), result);
    if(c->out.len == 0) {
      disable_olsr_socket(c->fd, &telnet_client_action, NULL, SP_PR_WRITE);
      if(c->state == pending)
        c->state = linger;
    }
  }
  else if(result < 0) {
    if(errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
      return;

    OLSR_PRINTF(1, "(TELNET) client %i write(): %s\n", c->fd, strerror(errno));
    c->state = destroy;
  }
}

static void
telnet_client_linger_timeout(void* data)
{
  struct telnet_client* c = (struct telnet_client*)data;
  OLSR_PRINTF(2, "(TELNET) client %i: disconnected after timeout\n", c->fd);
  c->linger_timer = NULL;
  telnet_client_remove(c);
}