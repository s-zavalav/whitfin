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

#include <sys/param.h>
#include <sys/queue.h>

#include <libpq-fe.h>
#include <pg_config.h>

#include "kore.h"
#include "http.h"
#include "pgsql.h"

struct pgsql_job {
	char			*query;
	struct http_request	*req;
	struct kore_pgsql	*pgsql;

	TAILQ_ENTRY(pgsql_job)	list;
};

struct pgsql_wait {
	struct http_request		*req;
	TAILQ_ENTRY(pgsql_wait)		list;
};

#define PGSQL_IS_BLOCKING	0
#define PGSQL_IS_ASYNC		1

#define PGSQL_CONN_MAX		2
#define PGSQL_CONN_FREE		0x01

static void	pgsql_conn_release(struct kore_pgsql *);
static void	pgsql_conn_cleanup(struct pgsql_conn *);
static int	pgsql_conn_create(struct kore_pgsql *);
static void	pgsql_read_result(struct kore_pgsql *, int);
static void	pgsql_queue_add(struct http_request *);
static void	pgsql_queue_wakeup(void);

static int	pgsql_simple_state_init(struct http_request *);
static int	pgsql_simple_state_query(struct http_request *);
static int	pgsql_simple_state_wait(struct http_request *);
static int	pgsql_simple_state_result(struct http_request *);
static int	pgsql_simple_state_done(struct http_request *);

#define PGSQL_SIMPLE_STATE_INIT			0
#define PGSQL_SIMPLE_STATE_QUERY		1
#define PGSQL_SIMPLE_STATE_WAIT			2
#define PGSQL_SIMPLE_STATE_RESULT		3
#define PGSQL_SIMPLE_STATE_DONE			4

static struct http_state	pgsql_states[] = {
	{ "PGSQL_SIMPLE_STATE_INIT",		pgsql_simple_state_init },
	{ "PGSQL_SIMPLE_STATE_QUERY",		pgsql_simple_state_query },
	{ "PGSQL_SIMPLE_STATE_WAIT",		pgsql_simple_state_wait },
	{ "PGSQL_SIMPLE_STATE_RESULT",		pgsql_simple_state_result },
	{ "PGSQL_SIMPLE_STATE_DONE",		pgsql_simple_state_done }
};

static struct kore_pool			pgsql_job_pool;
static struct kore_pool			pgsql_wait_pool;
static TAILQ_HEAD(, pgsql_conn)		pgsql_conn_free;
static TAILQ_HEAD(, pgsql_wait)		pgsql_wait_queue;
static u_int16_t			pgsql_conn_count;
char					*pgsql_conn_string = NULL;
u_int16_t				pgsql_conn_max = PGSQL_CONN_MAX;

void
kore_pgsql_init(void)
{
	pgsql_conn_count = 0;
	TAILQ_INIT(&pgsql_conn_free);
	TAILQ_INIT(&pgsql_wait_queue);

	kore_pool_init(&pgsql_job_pool, "pgsql_job_pool",
	    sizeof(struct pgsql_job), 100);
	kore_pool_init(&pgsql_wait_pool, "pgsql_wait_pool",
	    sizeof(struct pgsql_wait), 100);
}

int
kore_pgsql_async(struct kore_pgsql *pgsql, struct http_request *req,
    const char *query)
{
	int			fd;
	struct pgsql_conn	*conn;

	pgsql->state = KORE_PGSQL_STATE_INIT;
	pgsql->result = NULL;
	pgsql->error = NULL;
	pgsql->conn = NULL;

	if (TAILQ_EMPTY(&pgsql_conn_free)) {
		if (pgsql_conn_count >= pgsql_conn_max) {
			pgsql_queue_add(req);
			return (KORE_RESULT_ERROR);
		}

		if (!pgsql_conn_create(pgsql))
			return (KORE_RESULT_ERROR);
	}

	http_request_sleep(req);
	conn = TAILQ_FIRST(&pgsql_conn_free);
	if (!(conn->flags & PGSQL_CONN_FREE))
		fatal("received a pgsql conn that was not free?");

	conn->flags &= ~PGSQL_CONN_FREE;
	TAILQ_REMOVE(&pgsql_conn_free, conn, list);

	pgsql->conn = conn;
	conn->job = kore_pool_get(&pgsql_job_pool);
	conn->job->query = kore_strdup(query);
	conn->job->pgsql = pgsql;
	conn->job->req = req;

	LIST_INSERT_HEAD(&(req->pgsqls), pgsql, rlist);

	if (!PQsendQuery(conn->db, query)) {
		pgsql_conn_cleanup(conn);
		return (KORE_RESULT_ERROR);
	}

	fd = PQsocket(conn->db);
	if (fd < 0)
		fatal("PQsocket returned < 0 fd on open connection");

	kore_platform_schedule_read(fd, conn);
	pgsql->state = KORE_PGSQL_STATE_WAIT;
	kore_debug("query '%s' for %p sent on %p", query, req, conn);

	return (KORE_RESULT_OK);
}

int
kore_pgsql_run(struct http_request *req, struct kore_pgsql_simple *query)
{
	req->hdlr_extra = query;
	return (http_state_run(pgsql_states, sizeof(pgsql_states), req));
}

void
kore_pgsql_handle(void *c, int err)
{
	struct http_request	*req;
	struct kore_pgsql	*pgsql;
	struct pgsql_conn	*conn = (struct pgsql_conn *)c;

	if (err) {
		pgsql_conn_cleanup(conn);
		return;
	}

	req = conn->job->req;
	pgsql = conn->job->pgsql;
	kore_debug("kore_pgsql_handle: %p (%d)", req, pgsql->state);

	if (!PQconsumeInput(conn->db)) {
		pgsql->state = KORE_PGSQL_STATE_ERROR;
		pgsql->error = kore_strdup(PQerrorMessage(conn->db));
	} else {
		pgsql_read_result(pgsql, PGSQL_IS_ASYNC);
	}

	if (pgsql->state == KORE_PGSQL_STATE_WAIT) {
		http_request_sleep(req);
	} else {
		http_request_wakeup(req);
	}
}

void
kore_pgsql_continue(struct http_request *req, struct kore_pgsql *pgsql)
{
	kore_debug("kore_pgsql_continue: %p->%p (%d)",
	    req->owner, req, pgsql->state);

	if (pgsql->error) {
		kore_mem_free(pgsql->error);
		pgsql->error = NULL;
	}

	if (pgsql->result) {
		PQclear(pgsql->result);
		pgsql->result = NULL;
	}

	switch (pgsql->state) {
	case KORE_PGSQL_STATE_INIT:
	case KORE_PGSQL_STATE_WAIT:
		break;
	case KORE_PGSQL_STATE_DONE:
		http_request_wakeup(req);
		pgsql_conn_release(pgsql);
		break;
	case KORE_PGSQL_STATE_ERROR:
	case KORE_PGSQL_STATE_RESULT:
		kore_pgsql_handle(pgsql->conn, 0);
		break;
	default:
		fatal("unknown pgsql state %d", pgsql->state);
	}
}

void
kore_pgsql_cleanup(struct kore_pgsql *pgsql)
{
	kore_debug("kore_pgsql_cleanup(%p)", pgsql);

	if (pgsql->result != NULL)
		PQclear(pgsql->result);

	if (pgsql->error != NULL)
		kore_mem_free(pgsql->error);

	if (pgsql->conn != NULL)
		pgsql_conn_release(pgsql);

	pgsql->result = NULL;
	pgsql->error = NULL;
	pgsql->conn = NULL;

	LIST_REMOVE(pgsql, rlist);
}

void
kore_pgsql_logerror(struct kore_pgsql *pgsql)
{
	kore_log(LOG_NOTICE, "pgsql error: %s",
	    (pgsql->error) ? pgsql->error : "unknown");
}

int
kore_pgsql_ntuples(struct kore_pgsql *pgsql)
{
	return (PQntuples(pgsql->result));
}

char *
kore_pgsql_getvalue(struct kore_pgsql *pgsql, int row, int col)
{
	return (PQgetvalue(pgsql->result, row, col));
}

void
kore_pgsql_queue_remove(struct http_request *req)
{
	struct pgsql_wait	*pgw, *next;

	for (pgw = TAILQ_FIRST(&pgsql_wait_queue); pgw != NULL; pgw = next) {
		next = TAILQ_NEXT(pgw, list);
		if (pgw->req != req)
			continue;

		TAILQ_REMOVE(&pgsql_wait_queue, pgw, list);
		kore_pool_put(&pgsql_wait_pool, pgw);
		return;
	}
}

static void
pgsql_queue_add(struct http_request *req)
{
	struct pgsql_wait	*pgw;

	http_request_sleep(req);

	pgw = kore_pool_get(&pgsql_wait_pool);
	pgw->req = req;
	pgw->req->flags |= HTTP_REQUEST_PGSQL_QUEUE;

	TAILQ_INSERT_TAIL(&pgsql_wait_queue, pgw, list);
}

static void
pgsql_queue_wakeup(void)
{
	struct pgsql_wait	*pgw, *next;

	for (pgw = TAILQ_FIRST(&pgsql_wait_queue); pgw != NULL; pgw = next) {
		next = TAILQ_NEXT(pgw, list);
		if (pgw->req->flags & HTTP_REQUEST_DELETE)
			continue;

		http_request_wakeup(pgw->req);
		pgw->req->flags &= ~HTTP_REQUEST_PGSQL_QUEUE;

		TAILQ_REMOVE(&pgsql_wait_queue, pgw, list);
		kore_pool_put(&pgsql_wait_pool, pgw);
		return;
	}
}

static int
pgsql_conn_create(struct kore_pgsql *pgsql)
{
	struct pgsql_conn	*conn;

	if (pgsql_conn_string == NULL)
		fatal("pgsql_conn_create: no connection string");

	pgsql_conn_count++;
	conn = kore_malloc(sizeof(*conn));
	kore_debug("pgsql_conn_create(): %p", conn);
	memset(conn, 0, sizeof(*conn));

	conn->db = PQconnectdb(pgsql_conn_string);
	if (conn->db == NULL || (PQstatus(conn->db) != CONNECTION_OK)) {
		pgsql->state = KORE_PGSQL_STATE_ERROR;
		pgsql->error = kore_strdup(PQerrorMessage(conn->db));
		pgsql_conn_cleanup(conn);
		return (KORE_RESULT_ERROR);
	}

	conn->job = NULL;
	conn->flags = PGSQL_CONN_FREE;
	conn->type = KORE_TYPE_PGSQL_CONN;
	TAILQ_INSERT_TAIL(&pgsql_conn_free, conn, list);

	return (KORE_RESULT_OK);
}

static void
pgsql_conn_release(struct kore_pgsql *pgsql)
{
	int		fd;

	if (pgsql->conn == NULL)
		return;

	kore_mem_free(pgsql->conn->job->query);
	kore_pool_put(&pgsql_job_pool, pgsql->conn->job);

	/* Drain just in case. */
	while (PQgetResult(pgsql->conn->db) != NULL)
		;

	pgsql->conn->job = NULL;
	pgsql->conn->flags |= PGSQL_CONN_FREE;
	TAILQ_INSERT_TAIL(&pgsql_conn_free, pgsql->conn, list);

	fd = PQsocket(pgsql->conn->db);
	kore_platform_disable_read(fd);

	pgsql->conn = NULL;
	pgsql->state = KORE_PGSQL_STATE_COMPLETE;

	pgsql_queue_wakeup();
}

static void
pgsql_conn_cleanup(struct pgsql_conn *conn)
{
	struct http_request	*req;
	struct kore_pgsql	*pgsql;

	kore_debug("pgsql_conn_cleanup(): %p", conn);

	if (conn->flags & PGSQL_CONN_FREE)
		TAILQ_REMOVE(&pgsql_conn_free, conn, list);

	if (conn->job) {
		req = conn->job->req;
		pgsql = conn->job->pgsql;
		http_request_wakeup(req);

		pgsql->conn = NULL;
		pgsql->state = KORE_PGSQL_STATE_ERROR;
		pgsql->error = kore_strdup(PQerrorMessage(conn->db));

		kore_mem_free(conn->job->query);
		kore_pool_put(&pgsql_job_pool, conn->job);
		conn->job = NULL;
	}

	if (conn->db != NULL)
		PQfinish(conn->db);

	pgsql_conn_count--;
	kore_mem_free(conn);
}

static void
pgsql_read_result(struct kore_pgsql *pgsql, int async)
{
	if (async) {
		if (PQisBusy(pgsql->conn->db)) {
			pgsql->state = KORE_PGSQL_STATE_WAIT;
			return;
		}
	}

	pgsql->result = PQgetResult(pgsql->conn->db);
	if (pgsql->result == NULL) {
		pgsql->state = KORE_PGSQL_STATE_DONE;
		return;
	}

	switch (PQresultStatus(pgsql->result)) {
	case PGRES_COPY_OUT:
	case PGRES_COPY_IN:
	case PGRES_NONFATAL_ERROR:
	case PGRES_COPY_BOTH:
		break;
	case PGRES_COMMAND_OK:
		pgsql->state = KORE_PGSQL_STATE_DONE;
		break;
	case PGRES_TUPLES_OK:
#if PG_VERSION_NUM >= 90200
	case PGRES_SINGLE_TUPLE:
#endif
		pgsql->state = KORE_PGSQL_STATE_RESULT;
		break;
	case PGRES_EMPTY_QUERY:
	case PGRES_BAD_RESPONSE:
	case PGRES_FATAL_ERROR:
		pgsql->state = KORE_PGSQL_STATE_ERROR;
		pgsql->error = kore_strdup(PQresultErrorMessage(pgsql->result));
		break;
	}
}

static int
pgsql_simple_state_init(struct http_request *req)
{
	struct kore_pgsql_simple	*simple = req->hdlr_extra;

	if (simple->init == NULL || simple->done == NULL)
		fatal("pgsql_simple_state_init: missing callbacks");

	simple->query = NULL;
	simple->udata = NULL;
	simple->sql.state = 0;

	if (simple->init(req, simple) != KORE_RESULT_OK) {
		req->hdlr_extra = NULL;
		return (HTTP_STATE_COMPLETE);
	}

	req->fsm_state = PGSQL_SIMPLE_STATE_QUERY;
	return (HTTP_STATE_CONTINUE);
}

static int
pgsql_simple_state_query(struct http_request *req)
{
	struct kore_pgsql_simple	*simple = req->hdlr_extra;

	if (simple->query == NULL)
		fatal("No string set after pgsql_state_init()");

	req->fsm_state = PGSQL_SIMPLE_STATE_WAIT;

	if (!kore_pgsql_async(&simple->sql, req, simple->query)) {
		if (simple->sql.state == KORE_PGSQL_STATE_INIT) {
			req->fsm_state = PGSQL_SIMPLE_STATE_QUERY;
			return (HTTP_STATE_RETRY);
		}

		return (HTTP_STATE_CONTINUE);
	}

	return (HTTP_STATE_CONTINUE);
}

static int
pgsql_simple_state_wait(struct http_request *req)
{
	struct kore_pgsql_simple	*simple = req->hdlr_extra;

	switch (simple->sql.state) {
	case KORE_PGSQL_STATE_WAIT:
		return (HTTP_STATE_RETRY);
	case KORE_PGSQL_STATE_COMPLETE:
		req->fsm_state = PGSQL_SIMPLE_STATE_DONE;
		break;
	case KORE_PGSQL_STATE_ERROR:
		req->status = HTTP_STATUS_INTERNAL_ERROR;
		req->fsm_state = PGSQL_SIMPLE_STATE_DONE;
		kore_pgsql_logerror(&simple->sql);
		break;
	case KORE_PGSQL_STATE_RESULT:
		req->fsm_state = PGSQL_SIMPLE_STATE_RESULT;
		break;
	default:
		kore_pgsql_continue(req, &simple->sql);
		break;
	}

	return (HTTP_STATE_CONTINUE);
}

static int
pgsql_simple_state_result(struct http_request *req)
{
	struct kore_pgsql_simple	*simple = req->hdlr_extra;

	if (simple->result)
		simple->result(req, simple);

	req->fsm_state = PGSQL_SIMPLE_STATE_DONE;
	return (HTTP_STATE_CONTINUE);
}

static int
pgsql_simple_state_done(struct http_request *req)
{
	struct kore_pgsql_simple	*simple = req->hdlr_extra;

	req->hdlr_extra = NULL;
	simple->done(req, simple);

	if (simple->sql.state != 0)
		kore_pgsql_cleanup(&simple->sql);

	return (HTTP_STATE_COMPLETE);
}
