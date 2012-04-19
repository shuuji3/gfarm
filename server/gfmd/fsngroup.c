/*
 * $Id$
 */

#include <assert.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <time.h>

/* for host_addr_lookup() */
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>

#include <pthread.h>

#include <gfarm/gfarm.h>

#include "internal_host_info.h"

#include "gfutil.h"
#include "hash.h"
#include "thrsubr.h"

#include "metadb_common.h"	/* gfarm_host_info_free_except_hostname() */
#include "gfp_xdr.h"
#include "gfm_proto.h" /* GFM_PROTO_SCHED_FLAG_* */
#include "gfs_proto.h" /* GFS_PROTOCOL_VERSION */
#include "auth.h"
#include "config.h"

#include "callout.h"
#include "subr.h"
#include "rpcsubr.h"
#include "db_access.h"
#include "host.h"
#include "mdhost.h"
#include "user.h"
#include "peer.h"
#include "inode.h"
#include "abstract_host.h"
#include "abstract_host_impl.h"
#include "dead_file_copy.h"
#include "back_channel.h"
#include "relay.h"
#include "fsngroup.h"

#define FOR_ALL_HOSTS(it) \
	for (gfarm_hash_iterator_begin(host_hashtab, (it)); \
	    !gfarm_hash_iterator_is_end(it); \
	     gfarm_hash_iterator_next(it))

#define macro_stringify(X)	stringify(X)
#define stringify(X)	#X

#define dup_or_null(X)	\
	((X) == NULL) ? NULL : strdup_ck((X), "dup_or_null")

/*****************************************************************************/
/*
 * Symbols: mainly from host.c
 */
struct gfarm_hash_table *host_hashtab;
struct host *host_iterator_access(struct gfarm_hash_iterator *);

/*****************************************************************************/
/*
 * Type definitions:
 */

typedef void * (*iteration_filter_func)(struct host *, void *, int *);

typedef struct {
	char *hostname;
	char *fsngroupname;
} a_tuple_t;

struct gfarm_fsngroup_tuples_record {
	size_t n;
	a_tuple_t **tuples;
};

struct gfarm_fsngroup_text_record {
	size_t n;
	char **lines;
};

typedef struct {
	size_t nnames;
	char **names;
	gfarm_fsngroup_text_t exclusions;
	int flags;

#define CHECK_VALID(f)	((f) & FILTER_CHECK_VALID)
#define CHECK_UP(f)	((f) & FILTER_CHECK_UP)

#define MATCH_VALID(f, h) \
	((CHECK_VALID(f) && host_is_valid((h))) || !CHECK_VALID(f))
#define MATCH_UP(f, h) \
	((CHECK_UP(f) && host_is_up((h))) || !CHECK_UP(f))

#define MATCH_COND(f, h) \
	((MATCH_VALID((f), (h))) && (MATCH_UP((f), (h))))
} filter_arg_t;

/*****************************************************************************/
/*
 * Internal functions:
 */

/*
 * Basic objects constructors/destructors/methods:
 */

/*
 * Tuple/Tuples:
 */
static a_tuple_t *
allocate_tuple(const char *hostname, const char *fsngroupname)
{
	a_tuple_t *ret =
		(a_tuple_t *)malloc(sizeof(*ret));
	if (ret != NULL) {
		ret->hostname = dup_or_null(hostname);
		ret->fsngroupname = dup_or_null(fsngroupname);
	}

	return (ret);
}

static void
destroy_tuple(a_tuple_t *t)
{
	if (t != NULL) {
		free(t->hostname);
		free(t->fsngroupname);
		free(t);
	}
}

static gfarm_fsngroup_tuples_t
allocate_tuples(size_t n, a_tuple_t **tuples)
{
	struct gfarm_fsngroup_tuples_record *ret =
		(struct gfarm_fsngroup_tuples_record *)malloc(sizeof(*ret));
	if (ret != NULL) {
		ret->n = n;
		ret->tuples = tuples;
	}

	return ((gfarm_fsngroup_tuples_t)ret);
}

static void
destroy_tuples(gfarm_fsngroup_tuples_t t)
{
	struct gfarm_fsngroup_tuples_record *tr =
		(struct gfarm_fsngroup_tuples_record *)t;

	if (tr != NULL) {
		if (tr->tuples != NULL) {
			int i;
			for (i = 0; i < tr->n; i++) {
				destroy_tuple(tr->tuples[i]);
			}
			free(tr->tuples);
		}
		free(tr);
	}
}

static size_t
tuples_size(gfarm_fsngroup_tuples_t t)
{
	return (((struct gfarm_fsngroup_tuples_record *)t)->n);
}

static const char *
tuples_hostname(gfarm_fsngroup_tuples_t t, size_t i)
{
	struct gfarm_fsngroup_tuples_record *tr =
		(struct gfarm_fsngroup_tuples_record *)t;

	if (i < tr->n && tr->tuples[i] != NULL)
		return (const char *)(tr->tuples[i]->hostname);
	else
		return (NULL);
}

static const char *
tuples_fsngroup(gfarm_fsngroup_tuples_t t, size_t i)
{
	struct gfarm_fsngroup_tuples_record *tr =
		(struct gfarm_fsngroup_tuples_record *)t;

	if (i < tr->n && tr->tuples[i] != NULL)
		return (const char *)(tr->tuples[i]->fsngroupname);
	else
		return (NULL);
}

/*
 * Text:
 *	Actually, an array char * and # of its element.
 */
static gfarm_fsngroup_text_t
allocate_text(size_t n, char **lines)
{
	struct gfarm_fsngroup_text_record *ret =
		(struct gfarm_fsngroup_text_record *)malloc(sizeof(*ret));
	if (ret != NULL) {
		ret->n = n;
		ret->lines = lines;
	}

	return ((gfarm_fsngroup_text_t)ret);
}

static void
destroy_text(gfarm_fsngroup_text_t t)
{
	struct gfarm_fsngroup_text_record *tr =
		(struct gfarm_fsngroup_text_record *)t;

	if (tr != NULL) {
		if (tr->lines != NULL) {
			int i;
			for (i = 0; i < tr->n; i++) {
				free(tr->lines[i]);
			}
			free(tr->lines);
		}
		free(tr);
	}
}

static size_t
text_size(gfarm_fsngroup_text_t t)
{
	return (((struct gfarm_fsngroup_text_record *)t)->n);
}

static const char *
text_line(gfarm_fsngroup_text_t t, size_t i)
{
	struct gfarm_fsngroup_text_record *tr =
		(struct gfarm_fsngroup_text_record *)t;

	if (i < tr->n)
		return (const char *)(tr->lines[i]);
	else
		return (NULL);
}

static const char * const *
text_lines(gfarm_fsngroup_text_t t)
{
	struct gfarm_fsngroup_text_record *tr =
		(struct gfarm_fsngroup_text_record *)t;

	return (const char * const *)(tr->lines);
}

/*****************************************************************************/
/*
 * Host table iterations:
 */

/*
 * Simple fsngroup searcher.
 *
 * REQUISITE: giant_lock
 *
 *	returned pointer needs to be free'd.
 */
static char *
find_fsngroup_by_hostname(const char *hostname, int check_valid)
{
	char *ret = NULL;
	struct host *h;
	struct gfarm_hash_iterator it;

	FOR_ALL_HOSTS(&it) {
		h = host_iterator_access(&it);
		if (check_valid && !host_is_valid(h))
			continue;
		if (strcmp(host_name(h), hostname) == 0) {
			ret = strdup(host_fsngroup(h));
			if (ret == NULL)
				gflog_error(GFARM_MSG_UNFIXED,
					"%s: insufficient memory to "
					"allocate for a host search results.",
					"find_fsngroup_by_hostname");
			break;
		}
	}

	return (ret);
}

/*
 * A scanning workhorse.
 *
 * REQUISITE: giant_lock
 */
static void *
scan_host_cache(iteration_filter_func f, void *arg,
	size_t esize,		/* A size of one element to be allocated */
	size_t alloc_limit,	/* A limit # of elements to be allocated */
	size_t iter_limit, 	/* A limit # of iteration */
	size_t *nelemp		/* Returns # of allocated elements */)
{
	/*
	 * I really hate having the giant lock kinda long period of
	 * time. In order to avoid it, just copy needed members from
	 * the host hash table.
	 */

	struct gfarm_hash_iterator it;
	size_t nhosts = 0;
	int nmatches = 0;
	char *ret = NULL;
	char *diag = "scan_host_cache";

	FOR_ALL_HOSTS(&it) {
		(void)host_iterator_access(&it);
		nhosts++;
	}

	if (nhosts > 0) {
		size_t i = 0;
		size_t max_alloc;
		size_t max_iter;
		struct host *h;
		void *a_elem;
		char *dst;
		int stop_iter;
		int of;
		size_t sz;

		if (alloc_limit == 0)
			max_alloc = nhosts;
		else
			max_alloc =
				(alloc_limit < nhosts) ? alloc_limit : nhosts;

		of = 0;
		sz = gfarm_size_mul(&of, esize, max_alloc);
		if (of == 0 && sz > 0)
			ret = (char *)malloc(sz);
		if (ret == NULL) {
			gflog_error(GFARM_MSG_UNFIXED,
				"%s: insufficient memory to "
				"allocate for %zu host(s) search results.",
				diag, alloc_limit);
			return (NULL);
		}
		dst = ret;

		if (iter_limit == 0)
			max_iter = nhosts;
		else
			max_iter =
				(alloc_limit < nhosts) ? alloc_limit : nhosts;

		FOR_ALL_HOSTS(&it) {
			if (i >= max_alloc || i >= max_iter)
				break;
			h = host_iterator_access(&it);
			stop_iter = 0;
			if ((a_elem = (f)(h, arg, &stop_iter)) != NULL) {
				(void)memcpy((void *)dst,
					     (void *)&a_elem, esize);
				dst += esize;
				i++;
			}
			if (stop_iter)
				break;
		}
		nmatches = i;
	}

	if (nelemp != NULL)
		*nelemp = nmatches;

	return ((void *)ret);
}

/*
 * Matcher functions for the scanner:
 */
static void *
match_tuple_all(struct host *h, void *a, int *stopp)
{
	filter_arg_t *fa = (filter_arg_t *)a;
	int flags = fa->flags;
	gfarm_fsngroup_text_t exs = fa->exclusions;
	char *host = host_name(h);

	if (stopp != NULL)
		*stopp = 0;

	if (MATCH_COND(flags, h)) {
		if (exs == NULL) {
			return (allocate_tuple(host, host_fsngroup(h)));
		} else {
			size_t nexs = gfm_fsngroup_text_size(exs);
			size_t i;
			for (i = 0; i < nexs; i++) {
				if (strcmp(host,
					gfm_fsngroup_text_line(exs, i)) == 0)
					return (NULL);
			}
			return (allocate_tuple(host, host_fsngroup(h)));
		}
	}

	return (NULL);
}

static void *
match_tuple_by_hostnames(struct host *h, void *a, int *stopp)
{
	filter_arg_t *fa = (filter_arg_t *)a;
	int flags = fa->flags;
	gfarm_fsngroup_text_t exs = fa->exclusions;
	char *host = host_name(h);
	size_t i;

	if (stopp != NULL)
		*stopp = 0;

	if (MATCH_COND(flags, h)) {
		if (exs == NULL) {
			for (i = 0; i < fa->nnames; i++) {
				if (strcmp(host, fa->names[i]) == 0)
					return (allocate_tuple(fa->names[i],
							host_fsngroup(h)));
			}
		} else {
			size_t nexs = gfm_fsngroup_text_size(exs);
			for (i = 0; i < nexs; i++) {
				if (strcmp(host,
					gfm_fsngroup_text_line(exs, i)) == 0)
					return (NULL);
			}

			for (i = 0; i < fa->nnames; i++) {
				if (strcmp(host, fa->names[i]) == 0)
					return (allocate_tuple(fa->names[i],
							host_fsngroup(h)));
			}
		}
	}

	return (NULL);
}

static void *
match_tuple_by_fsngroups(struct host *h, void *a, int *stopp)
{
	filter_arg_t *fa = (filter_arg_t *)a;
	int flags = fa->flags;
	gfarm_fsngroup_text_t exs = fa->exclusions;
	char *fsngroup = host_fsngroup(h);
	size_t i;

	if (stopp != NULL)
		*stopp = 0;

	if (MATCH_COND(flags, h)) {
		if (exs == NULL) {
			for (i = 0; i < fa->nnames; i++) {
				if (strcmp(fsngroup, fa->names[i]) == 0)
					return (allocate_tuple(host_name(h),
							fa->names[i]));
			}
		} else {
			char *host = host_name(h);
			size_t nexs = gfm_fsngroup_text_size(exs);
			for (i = 0; i < nexs; i++) {
				if (strcmp(host,
					gfm_fsngroup_text_line(exs, i)) == 0)
					return (NULL);
			}

			for (i = 0; i < fa->nnames; i++) {
				if (strcmp(fsngroup, fa->names[i]) == 0)
					return (allocate_tuple(host,
							fa->names[i]));
			}
		}
	}

	return (NULL);
}

static void *
match_hostname_by_fsngroup(struct host *h, void *a, int *stopp)
{
	filter_arg_t *fa = (filter_arg_t *)a;
	const char *fsngroupname = (const char *)(fa->names[0]);
	int flags = fa->flags;
	gfarm_fsngroup_text_t exs = fa->exclusions;

	if (stopp != NULL)
		*stopp = 0;

	if (MATCH_COND(flags, h)) {
		if (exs == NULL) {
			if (strcmp(host_fsngroup(h), fsngroupname) == 0)
				return (strdup_ck(host_name(h),
					"get_hostname_by_fsngroup"));
		} else {
			char *host = host_name(h);
			size_t nexs = gfm_fsngroup_text_size(exs);
			size_t i;
			for (i = 0; i < nexs; i++) {
				if (strcmp(host,
					gfm_fsngroup_text_line(exs, i)) == 0)
					return (NULL);
			}

			if (strcmp(host_fsngroup(h), fsngroupname) == 0)
				return (strdup_ck(host,
					"get_hostname_by_fsngroup"));
		}
	}

	return (NULL);
}

/*
 * Scanners:
 */

static gfarm_fsngroup_tuples_t
get_tuples_all(gfarm_fsngroup_text_t exs, int flags)
{
	size_t n = 0;
	filter_arg_t arg = {
		0, NULL, exs, flags
	};

	a_tuple_t **tuples =
		(a_tuple_t **)scan_host_cache(
			match_tuple_all, (void *)&arg,
			sizeof(a_tuple_t *), 0, 0, &n);

	return (allocate_tuples(n, tuples));
}

static gfarm_fsngroup_tuples_t
get_tuples_by_hostnames(const char **hostnames, size_t nhostnames,
	gfarm_fsngroup_text_t exs, int flags)
{
	size_t n = 0;
	filter_arg_t arg = {
		nhostnames, (char **)hostnames, exs, flags
	};
	a_tuple_t **tuples =
		(a_tuple_t **)scan_host_cache(
			match_tuple_by_hostnames, (void *)&arg,
			sizeof(a_tuple_t *), 0, 0, &n);

	return (allocate_tuples(n, tuples));
}

static gfarm_fsngroup_tuples_t
get_tuples_by_fsngroups(const char **fsngroups, size_t nfsngroups,
	gfarm_fsngroup_text_t exs, int flags)
{
	size_t n = 0;
	filter_arg_t arg = {
		nfsngroups, (char **)fsngroups, exs, flags
	};
	a_tuple_t **tuples =
		(a_tuple_t **)scan_host_cache(
			match_tuple_by_fsngroups, (void *)&arg,
			sizeof(a_tuple_t *), 0, 0, &n);

	return (allocate_tuples(n, tuples));
}

static gfarm_fsngroup_text_t
get_hostnames_by_fsngroup(const char *fsngroup,
	gfarm_fsngroup_text_t exs, int flags)
{
	size_t n = 0;
	const char * const names[] = { fsngroup, NULL };
	filter_arg_t arg = {
		1, (char **)names, exs, flags
	};
	char **hostnames =
		(char **)scan_host_cache(
			match_hostname_by_fsngroup, (void *)&arg,
			sizeof(char *), 0, 0, &n);

	return (allocate_text(n, hostnames));
}

/*****************************************************************************/
/*
 * Exported functions:
 */

/*
 * Basic objects destructors/methods:
 */

size_t
gfm_fsngroup_tuples_size(gfarm_fsngroup_tuples_t t)
{
	return (tuples_size(t));
}

const char *
gfm_fsngroup_tuples_hostname(gfarm_fsngroup_tuples_t t, size_t i)
{
	return (tuples_hostname(t, i));
}

const char *
gfm_fsngroup_tuples_fsngroup(gfarm_fsngroup_tuples_t t, size_t i)
{
	return (tuples_fsngroup(t, i));
}

void
gfm_fsngroup_tuples_destroy(gfarm_fsngroup_tuples_t t)
{
	destroy_tuples(t);
}

size_t
gfm_fsngroup_text_size(gfarm_fsngroup_text_t t)
{
	return (text_size(t));
}

const char *
gfm_fsngroup_text_line(gfarm_fsngroup_text_t t, size_t i)
{
	return (text_line(t, i));
}

const char * const *
gfm_fsngroup_text_lines(gfarm_fsngroup_text_t t)
{
	return (text_lines(t));
}

void
gfm_fsngroup_text_destroy(gfarm_fsngroup_text_t t)
{
	destroy_text(t);
}

gfarm_fsngroup_text_t
gfm_fsngroup_text_allocate(size_t n, char **lines)
{
	return allocate_text(n, lines);
}

/*****************************************************************************/
/*
 * Scanners:
 */

gfarm_fsngroup_tuples_t
gfm_fsngroup_get_tuples_all_unlock(gfarm_fsngroup_text_t exs, int flags)
{
	return (get_tuples_all(exs, flags));
}

gfarm_fsngroup_tuples_t
gfm_fsngroup_get_tuples_all(gfarm_fsngroup_text_t exs, int flags)
{
	gfarm_fsngroup_tuples_t ret;

	giant_lock();
	ret = get_tuples_all(exs, flags);
	giant_unlock();

	return (ret);
}

gfarm_fsngroup_tuples_t
gfm_fsngroup_get_tuples_by_hostnames_unlock(
	const char **hostnames, size_t nhostnames,
	gfarm_fsngroup_text_t exs, int flags)
{
	return (get_tuples_by_hostnames(hostnames, nhostnames,
			exs, flags));
}

gfarm_fsngroup_tuples_t
gfm_fsngroup_get_tuples_by_hostnames(
	const char **hostnames, size_t nhostnames,
	gfarm_fsngroup_text_t exs, int flags)
{
	gfarm_fsngroup_tuples_t ret;

	giant_lock();
	ret = get_tuples_by_hostnames(hostnames, nhostnames, exs, flags);
	giant_unlock();

	return (ret);
}

gfarm_fsngroup_tuples_t
gfm_fsngroup_get_tuples_by_fsngroups_unlock(
	const char **hostnames, size_t nhostnames,
	gfarm_fsngroup_text_t exs, int flags)
{
	return (get_tuples_by_fsngroups(hostnames, nhostnames,
			exs, flags));
}

gfarm_fsngroup_tuples_t
gfm_fsngroup_get_tuples_by_fsngroups(
	const char **hostnames, size_t nhostnames,
	gfarm_fsngroup_text_t exs, int flags)
{
	gfarm_fsngroup_tuples_t ret;

	giant_lock();
	ret = get_tuples_by_fsngroups(hostnames, nhostnames, exs, flags);
	giant_unlock();

	return (ret);
}

gfarm_fsngroup_text_t
gfm_fsngroup_get_hostnames_by_fsngroup_unlock(
	const char *fsngroup, gfarm_fsngroup_text_t exs, int flags)
{
	return (get_hostnames_by_fsngroup(fsngroup, exs, flags));
}

gfarm_fsngroup_text_t
gfm_fsngroup_get_hostnames_by_fsngroup(
	const char *fsngroup, gfarm_fsngroup_text_t exs, int flags)
{
	gfarm_fsngroup_text_t ret;

	giant_lock();
	ret = get_hostnames_by_fsngroup(fsngroup, exs, flags);
	giant_unlock();

	return (ret);
}

/*****************************************************************************/
/*
 * Server side RPC stubs:
 */

gfarm_error_t
gfm_server_fsngroup_get_all(
	struct peer *peer, gfp_xdr_xid_t xid, size_t *sizep,
	int from_client, int skip)
{
	/*
	 * IN:
	 *	None
	 *
	 * OUT:
	 *	resultcode::integer
	 *	if resultcode == GFM_ERROR_NO_ERROR
	 *		n::integer
	 *		tuple{hostname::string, fsngroupname::string}[n]
	 */

	gfarm_error_t e = GFARM_ERR_UNKNOWN;

	(void)from_client;

	if (!skip) {
		int i;
		int n = 0;
		gfarm_fsngroup_tuples_t t = NULL;
		gfarm_error_t e2;
		const char diag[] =
			macro_stringify(GFM_PROTO_FSNGROUP_GET_ALL);

		e = wait_db_update_info(peer, DBUPDATE_HOST, diag);
		if (e != GFARM_ERR_NO_ERROR) {
			gflog_error(GFARM_MSG_UNFIXED,
				"failed to wait for the backend DB to "
				"be updated: %s",
				gfarm_error_string(e));
			goto bailout;
		}

		giant_lock();
		t = get_tuples_all(NULL, FILTER_CHECK_VALID);
		giant_unlock();

		if (t != NULL) {
			n = tuples_size(t);
			e = GFARM_ERR_NO_ERROR;
		} else {
			gflog_debug(
				GFARM_MSG_UNFIXED,
				"get_tuples_all() returns NULL");
			e = GFARM_ERR_NO_MEMORY;
		}

		e2 = gfm_server_put_reply(peer, xid, sizep, diag, e, "i", n);
		if (e2 != GFARM_ERR_NO_ERROR) {
			gflog_debug(GFARM_MSG_UNFIXED,
				"gfm_server_put_reply(%s) failed: %s",
				diag, gfarm_error_string(e));
		}

		if (e != GFARM_ERR_NO_ERROR || e2 != GFARM_ERR_NO_ERROR)
			goto done;

		if (n > 0) {
			struct gfp_xdr *c = peer_get_conn(peer);

			for (i = 0; i < n; i++) {
				e = gfp_xdr_send(c, "ss",
					tuples_hostname(t, i),
					tuples_fsngroup(t, i));
				if (e != GFARM_ERR_NO_ERROR) {
					gflog_debug(GFARM_MSG_UNFIXED,
						"gfp_xdr_send(%s) failed: %s",
						diag, gfarm_error_string(e));
					goto done;
				}
			}
		}

done:
		if (t != NULL)
			destroy_tuples(t);
	} else {
		e = GFARM_ERR_NO_ERROR;
	}

bailout:
	return (e);
}

gfarm_error_t
gfm_server_fsngroup_get_by_hostname(
	struct peer *peer, gfp_xdr_xid_t xid, size_t *sizep,
	int from_client, int skip)
{
	/*
	 * IN:
	 *	hostname::string
	 *
	 * OUT:
	 *	resultcode::integer
	 *	if resultcode == GFM_ERROR_NO_ERROR
	 *		fsngroupname::string
	 */

	gfarm_error_t e = GFARM_ERR_UNKNOWN;
	char *hostname = NULL;		/* Always need to be free'd */
	char *fsngroupname = NULL;	/* Always need to be free'd */
	static const char diag[] =
		macro_stringify(GFM_PROTO_FSNGROUP_GET_BY_HOSTNAME);
	struct relayed_request *relay = NULL;

	(void)from_client;

	e = gfm_server_get_request_with_relay(
		peer, sizep, skip, &relay, diag,
		GFM_PROTO_FSNGROUP_GET_BY_HOSTNAME,
		"s", &hostname);
	if (e != GFARM_ERR_NO_ERROR)
		goto bailout;
	if (skip) {
		e = GFARM_ERR_NO_ERROR;
		goto bailout;
	}

	if (relay == NULL) {
		/* do not relay RPC to master gfmd */

		giant_lock();
		fsngroupname = find_fsngroup_by_hostname(hostname, 1);
		giant_unlock();

		if (fsngroupname == NULL) {
			gflog_debug(GFARM_MSG_UNFIXED,
				"host does not exists");
			e = GFARM_ERR_NO_SUCH_OBJECT;
		}
	}

	if (fsngroupname != NULL)
		e = gfm_server_put_reply_with_relay(
			peer, xid, sizep, relay, diag, &e, "s", &fsngroupname);
	else
		e = gfm_server_put_reply_with_relay(
			peer, xid, sizep, relay, diag, &e, "");

bailout:
	free(hostname);
	free(fsngroupname);

	return (e);
}

gfarm_error_t
gfm_server_fsngroup_modify(
	struct peer *peer, gfp_xdr_xid_t xid, size_t *sizep,
	int from_client, int skip)
{
	/*
	 * IN:
	 *	hostname::string
	 *	fsngroupname::string
	 *
	 * OUT:
	 *	resultcode::integer
	 */

	static const char diag[] =
		macro_stringify(GFM_PROTO_FSNGROUP_MODIFY);
	gfarm_error_t e = GFARM_ERR_UNKNOWN;
	struct relayed_request *relay = NULL;
	char *hostname = NULL;		/* need to be free'd always */
	char *fsngroupname = NULL;	/* need to be free'd always */

	e = gfm_server_get_request_with_relay(
		peer, sizep, skip, &relay, diag,
		GFM_PROTO_FSNGROUP_MODIFY,
		"ss", &hostname, &fsngroupname);
	if (e != GFARM_ERR_NO_ERROR)
		goto bailout;
	if (skip) {
		e = GFARM_ERR_NO_ERROR;
		goto bailout;
	}

	if (relay == NULL) {
		/* do not relay RPC to master gfmd */
		struct host *h = NULL;
		struct user *user = peer_get_user(peer);

		if (!from_client || user == NULL) {
			gflog_debug(GFARM_MSG_UNFIXED,
				"operation is not permitted");
			e = GFARM_ERR_OPERATION_NOT_PERMITTED;
			goto reply;
		}

		giant_lock();

		if ((h = host_lookup(hostname)) == NULL) {
			gflog_debug(GFARM_MSG_UNFIXED,
				"host does not exists");
			e = GFARM_ERR_NO_SUCH_OBJECT;
			goto unlock;
		}
		if (!user_is_admin(user)) {
			gflog_debug(GFARM_MSG_UNFIXED,
				"operation is not permitted");
			e = GFARM_ERR_OPERATION_NOT_PERMITTED;
			goto unlock;
		}
		if ((e = db_fsngroup_modify(hostname, fsngroupname)) !=
			GFARM_ERR_NO_ERROR) {
			gflog_debug(GFARM_MSG_UNFIXED,
				"db_fsngroup_modify failed: %s",
				gfarm_error_string(e));
			goto unlock;
		}
		host_fsngroup_modify(h, fsngroupname);

unlock:
		giant_unlock();
	}
reply:
	e = gfm_server_put_reply_with_relay(
		peer, xid, sizep, relay, diag, &e, "");

bailout:
	free(hostname);
	free(fsngroupname);

	return (e);
}
