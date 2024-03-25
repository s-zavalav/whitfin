/*
 * Copyright (c) 2014 Joris Vink <joris@coders.se>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <kore/kore.h>
#include <kore/http.h>

#include <limits.h>

/*
 * KTunnel shows how Kore exposes its net internals to its libraries
 * and how we can "abuse" these internals to create a "anything"
 * over HTTPS tunnel.
 */

int		open_connection(struct http_request *);

static int	ktunnel_pipe_data(struct netbuf *);
static void	ktunnel_pipe_disconnect(struct connection *);
static int	ktunnel_pipe_create(struct connection *,
		    const char *, const char *);

/*
 * Receive a request to open a new connection.
 */
int
open_connection(struct http_request *req)
{
	char			*host, *port;

	/* Don't want to deal with SPDY connections. */
	if (req->owner->proto != CONN_PROTO_HTTP) {
		http_response(req, HTTP_STATUS_BAD_REQUEST, NULL, 0);
		return (KORE_RESULT_OK);
	}

	/* Parse the query string and grab our arguments. */
	http_populate_arguments(req);
	if (!http_argument_get_string("host", &host, NULL) ||
	    !http_argument_get_string("port", &port, NULL)) {
		http_response(req, HTTP_STATUS_BAD_REQUEST, NULL, 0);
		return (KORE_RESULT_OK);
	}

	/* Create our tunnel. */
	if (!ktunnel_pipe_create(req->owner, host, port)) {
		http_response(req, HTTP_STATUS_INTERNAL_ERROR, NULL, 0);
		return (KORE_RESULT_OK);
	}

	/*
	 * Hack so http_response() doesn't end up queueing a new
	 * netbuf for receiving more HTTP requests on the same connection.
	 */
	req->owner->flags |= CONN_CLOSE_EMPTY;

	/* Respond to the client now that we're good to go. */
	http_response(req, HTTP_STATUS_OK, NULL, 0);

	/* Unset this so we don't disconnect after returning. */
	req->owner->flags &= ~CONN_CLOSE_EMPTY;

	return (KORE_RESULT_OK);
}

/*
 * Connect to our target host:port and attach it to a struct connection that
 * Kore understands. We set the disconnect method so we get a callback
 * whenever either of the connections will go away so we can cleanup the
 * one it is attached to.
 *
 * We are storing the "piped" connection in hdlr_extra.
 */
static int
ktunnel_pipe_create(struct connection *c, const char *host, const char *port)
{
	struct sockaddr_in	sin;
	struct connection	*pipe;
	u_int16_t		nport;
	int			fd, err;
	struct netbuf		*nb, *next;

	nport = kore_strtonum(port, 10, 1, SHRT_MAX, &err);
	if (err == KORE_RESULT_ERROR) {
		kore_log(LOG_ERR, "invalid port given %s", port);
		return (KORE_RESULT_ERROR);
	}

	if ((fd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
		kore_log(LOG_ERR, "socket(): %s", errno_s);
		return (KORE_RESULT_ERROR);
	}

	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_port = htons(nport);
	sin.sin_addr.s_addr = inet_addr(host);

	kore_log(LOG_NOTICE, "Attempting to connect to %s:%s", host, port);

	if (connect(fd, (struct sockaddr *)&sin, sizeof(sin)) == -1) {
		close(fd);
		kore_log(LOG_ERR, "connect(): %s", errno_s);
		return (KORE_RESULT_ERROR);
	}

	if (!kore_connection_nonblock(fd)) {
		close(fd);
		return (KORE_RESULT_ERROR);
	}

	pipe = kore_connection_new(c);
	pipe->fd = fd;
	pipe->addr.ipv4 = sin;
	pipe->read = net_read;
	pipe->write = net_write;
	pipe->addrtype = AF_INET;
	pipe->proto = CONN_PROTO_UNKNOWN;
	pipe->state = CONN_STATE_ESTABLISHED;

	c->hdlr_extra = pipe;
	pipe->hdlr_extra = c;
	c->disconnect = ktunnel_pipe_disconnect;
	pipe->disconnect = ktunnel_pipe_disconnect;

	kore_worker_connection_add(pipe);
	kore_connection_start_idletimer(pipe);

	for (nb = TAILQ_FIRST(&(c->recv_queue)); nb != NULL; nb = next) {
		next = TAILQ_NEXT(nb, list);
		TAILQ_REMOVE(&(c->recv_queue), nb, list);
		kore_mem_free(nb->buf);
		kore_pool_put(&nb_pool, nb);
	}

	kore_platform_event_all(pipe->fd, pipe);

	net_recv_queue(c, NETBUF_SEND_PAYLOAD_MAX,
	    NETBUF_CALL_CB_ALWAYS, NULL, ktunnel_pipe_data);
	net_recv_queue(pipe, NETBUF_SEND_PAYLOAD_MAX,
	    NETBUF_CALL_CB_ALWAYS, NULL, ktunnel_pipe_data);

	printf("connection started to %s (%p -> %p)\n", host, c, pipe);
	return (KORE_RESULT_OK);
}

/*
 * Called everytime new data is read from any of the connections
 * that are part of a pipe.
 */
static int
ktunnel_pipe_data(struct netbuf *nb)
{
	struct connection	*src = nb->owner;
	struct connection	*dst = src->hdlr_extra;

	printf("received %d bytes on pipe %p (-> %p)\n", nb->s_off, src, dst);

	net_send_queue(dst, nb->buf, nb->s_off, NULL, NETBUF_LAST_CHAIN);
	net_send_flush(dst);

	/* Reuse the netbuf so we don't have to recreate them all the time. */
	nb->s_off = 0;

	return (KORE_RESULT_OK);
}

/*
 * Called when either part of the pipe disconnects.
 */
static void
ktunnel_pipe_disconnect(struct connection *c)
{
	struct connection	*pipe = c->hdlr_extra;

	printf("ktunnel_pipe_disconnect(%p)->%p\n", c, pipe);

	if (pipe != NULL) {
		/* Prevent Kore from calling kore_mem_free() on hdlr_extra. */
		c->hdlr_extra = NULL;
		kore_connection_disconnect(pipe);
	}
}
