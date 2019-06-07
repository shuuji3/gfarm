/*
 * $Id$
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <libgen.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/time.h>
#include <errno.h>
#include <assert.h>
#include <ctype.h>

#include <gfarm/gfarm.h>

#include "gfutil.h"
#include "hash.h"

#include "gfarm_foreach.h"
#include "gfarm_path.h"
#include "fsngroup_info.h"
#include "gfm_proto.h"
#include "gfm_client.h"
#include "lookup.h"
#include "metadb_server.h"
#include "gfs_replica_fix.h"

#define ISBLANK(c)	((c) == ' ' || (c) == '\t')

#define SLEEP_TIME	1

static char *program_name = "gfncopy";

static void
usage(void)
{
	fprintf(stderr,
"Usage: %s [-vh] PATH               - display NCOPY of PATH [or parent dir]\n"
"       %s [-vh][-C|-M] -s NCOPY PATH    - set NCOPY of PATH\n"
"       %s [-vh][-C|-M] -S REPATTR PATH  - set REPATTR of PATH\n"
"       %s [-vh] -r PATH...         - remove NCOPY of PATHs\n"
"       %s [-vh] -c PATH            - display number of replicas of PATH\n"
"       %s [-v] -w [-t SEC] PATH... - wait for automatic replication\n",
	    program_name, program_name, program_name,
	    program_name, program_name, program_name);

	fprintf(stderr,
"REPATTR BNF:\n"
"\t<repattr> ::= <an_attr> | <an_attr> ',' <repattr>\n"
"\t<an_attr> ::= <string> ':' <integer>\n"
"REPATTR Examples:\n"
"\t'group0:2'\n"
"\t'group0:1,group1:2,group2:3'\n"
"SEE ALSO:\n"
"\tgfhostgroup(1)\n");

	exit(1);
}

static int opt_verb;

static void
ERR(const char *format, ...)
{
	va_list ap;

	fprintf(stderr, "%s: ", program_name);
	va_start(ap, format);
	vfprintf(stderr, format, ap);
	va_end(ap);
	fputs("\n", stderr);
}

static void
VERB(const char *format, ...)
{
	va_list ap;

	if (!opt_verb)
		return;
	fprintf(stderr, "%s: ", program_name);
	va_start(ap, format);
	vfprintf(stderr, format, ap);
	va_end(ap);
	fputs("\n", stderr);
}

static int
port_size(int port)
{
	int s;

	for (s = 0; port > 0; ++s, port /= 10)
		;
	return (s);
}

static gfarm_error_t
gfarm2fs_realurl(const char *path, char **url_p, char **root_url_p)
{
	gfarm_error_t e;
	char *url, *url2, *root_url;
	const char *metadb;
	int port;
	size_t metadb_len, port_len, path_len, root_url_len;
	struct gfm_connection *gfm_server;
	struct gfarm_metadb_server *ms;

	if (gfarm_is_url(path)) {
		url = strdup(path);
		if (url == NULL) {
			ERR("no memory");
			return (GFARM_ERR_NO_MEMORY);
		}
	} else {
		url = NULL;
		e = gfarm_realpath_by_gfarm2fs(path, &url);
		if (e == GFARM_ERR_NO_SUCH_OBJECT) {
			if (path[0] == '/') {
				path_len = strlen(path) + 9; /* + gfarm:// */
				GFARM_MALLOC_ARRAY(url, path_len);
				if (url == NULL) {
					ERR("no memory");
					return (GFARM_ERR_NO_MEMORY);
				}
				snprintf(url, path_len, "gfarm://%s", path);
			} else {
				path_len = strlen(path) + 10; /* + gfarm:/// */
				GFARM_MALLOC_ARRAY(url, path_len);
				if (url == NULL) {
					ERR("no memory");
					return (GFARM_ERR_NO_MEMORY);
				}
				snprintf(url, path_len, "gfarm:///%s", path);
			}
		} else if (e != GFARM_ERR_NO_ERROR) {
			ERR("%s: %s", path, gfarm_error_string(e));
			return (e);
		}
	}

	assert(url != NULL);
	assert(gfarm_is_url(url));
	if (strncmp(url, "gfarm:///", 9) != 0 && strlen(url) >= 9) {
		/* gfarm://host:601 or gfarm://host:601/dir/file */
		int i = 8; /* skip gfarm:// */
		char *p = url + i;

		/* skip hostname:port */
		while (*p != '/' && *p != '\0') {
			p++;
			i++;
		}
		/* ex.: gfarm://host:601/abc -> i = 16 */
		GFARM_MALLOC_ARRAY(root_url, i + 1);
		memcpy(root_url, url, i);
		root_url[i] = '\0';
		goto end;
	}

	/* hostname:port is not set */
	/* [convert] gfarm:///dir/file -> gfarm://host:port/dir/file */
	/* [permit] "gfarm:" "gfarm:/" "gfarm://" */
	if ((e = gfm_client_connection_and_process_acquire_by_path(
	    url, &gfm_server)) != GFARM_ERR_NO_ERROR) {
		free(url);
		ERR("gfm_client_connection_and_process_acquire_by_path: %s",
		    gfarm_error_string(e));
		return (e);
	}
	ms = gfm_client_connection_get_real_server(gfm_server);
	metadb = gfarm_metadb_server_get_name(ms);
	port = gfarm_metadb_server_get_port(ms);
	gfm_client_connection_free(gfm_server);

	metadb_len = strlen(metadb);
	port_len = port_size(port);
	/* gfarm://host:port/ */
	root_url_len = 8 + metadb_len + 1 + port_len + 1;
	GFARM_MALLOC_ARRAY(root_url, root_url_len);
	if (root_url == NULL) {
		free(url);
		ERR("no memory");
		return (GFARM_ERR_NO_MEMORY);
	}
	snprintf(root_url, root_url_len, "gfarm://%s:%d", metadb, port);

	if (strcmp(url, "gfarm:") == 0 ||
	    strcmp(url, "gfarm:/") == 0 ||
	    strcmp(url, "gfarm://") == 0) {
		free(url);
		url = strdup(root_url);
		if (url == NULL) {
			free(root_url);
			ERR("no memory");
			return (GFARM_ERR_NO_MEMORY);
		}
	} else {
		/* gfarm:///dir/file */
		path = url + 8; /* skip gfarm:// */
		path_len = strlen(path);
		GFARM_MALLOC_ARRAY(url2, root_url_len + path_len + 1);
		if (url2 == NULL) {
			free(url);
			free(root_url);
			ERR("no memory");
			return (GFARM_ERR_NO_MEMORY);
		}
		url2[0] = '\0';
		strcat(url2, root_url);
		strcat(url2, path);
		free(url);
		url = url2;
	}
end:
	*url_p = url;
	*root_url_p = root_url;
	return (GFARM_ERR_NO_ERROR);
}

static gfarm_error_t (*gfncopy_stat)(const char *, struct gfs_stat *);
static gfarm_error_t (*gfncopy_getxattr)(const char *, const char *,
	void *, size_t *);
static gfarm_error_t (*gfncopy_setxattr)(const char *, const char *,
	const void *, size_t, int);
static gfarm_error_t (*gfncopy_removexattr)(const char *, const char *);

static gfarm_error_t
get_xattr(const char *url, char *key, char **value_p)
{
	gfarm_error_t e;
	char *value;
	size_t size = 128;
	int retry = 0;

	for (;;) {
		GFARM_MALLOC_ARRAY(value, size + 1); /* '\0' */
		if (value == NULL)
			return (GFARM_ERR_NO_MEMORY);
		e = gfncopy_getxattr(url, key, value, &size);
		if (e == GFARM_ERR_NO_ERROR) {
			value[size] = '\0';
			*value_p = value;
			break; /* OK */
		} else {
			free(value);
			if (retry == 0)
				retry = 1;
			else
				break; /* NG */
		}
	}
	return (e);
}

static gfarm_error_t
get_replica_spec(
	const char *url, const char *root_url, int *ncopy_p, char **repattr_p)
{
	gfarm_error_t e_ncopy, e_repattr;
	char *repattr = NULL, *ncopy_str, *tmpurl;
	int found, ncopy = 0, support_repattr;
	static char *cached_root_url = NULL;
	static int cached_support_repattr = 0;

	if (cached_root_url != NULL &&
	    strcmp(cached_root_url, root_url) == 0) {
		support_repattr = cached_support_repattr;
		if (support_repattr)
			e_repattr = GFARM_ERR_NO_ERROR;
		else
			e_repattr = GFARM_ERR_NO_SUCH_OBJECT;
	} else {
		e_repattr = get_xattr(root_url, GFARM_EA_REPATTR, &repattr);
		if (e_repattr == GFARM_ERR_NO_ERROR ||
		    e_repattr == GFARM_ERR_NO_SUCH_OBJECT) {
			support_repattr = 1;
			if (e_repattr == GFARM_ERR_NO_ERROR)
				free(repattr);
		} else {
			support_repattr = 0;
			e_repattr = GFARM_ERR_NO_SUCH_OBJECT;
		}
		free(cached_root_url);
		cached_root_url = strdup(root_url);
		/* ignore: cached_root_url == NULL */
		cached_support_repattr = support_repattr;
	}

	tmpurl = strdup(url);
	if (tmpurl == NULL) {
		free(repattr);
		return (GFARM_ERR_NO_MEMORY);
	}

	found = 0;
	do {
		int is_root = 0;
		char *parent;

		if (opt_verb)
			printf("%s:\n", tmpurl);

		if (strcmp(tmpurl, root_url) == 0)
			is_root = 1;
		e_ncopy = get_xattr(tmpurl, GFARM_EA_NCOPY, &ncopy_str);
		if (e_ncopy == GFARM_ERR_NO_ERROR) {
			ncopy = atoi(ncopy_str);
			free(ncopy_str);
			found = 1;
		} else if (e_ncopy != GFARM_ERR_NO_SUCH_OBJECT) {
			free(tmpurl);
			free(repattr);
			return (e_ncopy);
		}
		if (support_repattr) {
			e_repattr =
			    get_xattr(tmpurl, GFARM_EA_REPATTR, &repattr);
			if (e_repattr == GFARM_ERR_NO_ERROR) {
				found = 1;
			} else if (e_repattr != GFARM_ERR_NO_SUCH_OBJECT) {
				free(tmpurl);
				free(repattr);
				return (e_repattr);
			}
		}

		if (found || is_root)
			break;

		parent = gfarm_url_dir(tmpurl);
		if (parent == NULL) {
			free(tmpurl);
			free(repattr);
			return (GFARM_ERR_NO_MEMORY);
		}
		free(tmpurl);
		tmpurl = parent;
	} while (1);

	free(tmpurl);

	if (e_ncopy != GFARM_ERR_NO_ERROR)
		ncopy = -1;
	if (e_repattr != GFARM_ERR_NO_ERROR)
		repattr = NULL;

	*ncopy_p = ncopy;
	*repattr_p = repattr;
	return (found ? GFARM_ERR_NO_ERROR : GFARM_ERR_NO_SUCH_OBJECT);
}

static gfarm_error_t
count_replica(const char *url, int flags, int *np)
{
	gfarm_error_t e;
	struct gfs_replica_info *ri;

	e = gfs_replica_info_by_name(url, flags, &ri);
	if (e != GFARM_ERR_NO_ERROR) {
		ERR("%s: gfs_replica_info_by_name: %s",
		    url, gfarm_error_string(e));
		return (e);
	}
	*np = gfs_replica_info_number(ri);
	gfs_replica_info_free(ri);
	return (GFARM_ERR_NO_ERROR);
}

static gfarm_error_t
count_valid(const char *url, int *np)
{
	return (count_replica(url, 0, np));
}

static void
print_replica_spec_main(int ncopy, const char *repattr)
{
	if (ncopy >= 0) {
		if (opt_verb)
			printf("%s=", GFARM_EA_NCOPY);
		printf("%d\n", ncopy);
	}

	if (repattr != NULL) {
		if (opt_verb)
			printf("%s=", GFARM_EA_REPATTR);
		printf("%s\n", repattr);
	}
}


static time_t opt_timeout = 30; /* sec. */

static gfarm_error_t
wait_for_replication(
	const char *url, const char *root_url, struct gfs_stat *st, void *val)
{
	gfarm_error_t e;
	char *repattr = NULL;
	int ncopy;
	gfarm_uint64_t *iflagsp = val, oflags;
	struct timeval limit, now, t;
	time_t timeout;

	if (!GFARM_S_ISREG(st->st_mode)) {
		VERB("%s: not a file (ignored)", url);
		return (GFARM_ERR_NO_ERROR); /* ignore */
	}
	if (st->st_size == 0) {
		VERB("%s: empty file (ignored)", url);
		return (GFARM_ERR_NO_ERROR);
	}

	if (opt_verb) {
		e = get_replica_spec(url, root_url, &ncopy, &repattr);
		if (e == GFARM_ERR_NO_SUCH_OBJECT) {
			/* ignore: xattr is not set */
			return (GFARM_ERR_NO_ERROR);
		} else if (e != GFARM_ERR_NO_ERROR)
			return (e);

		print_replica_spec_main(ncopy, repattr);
		free(repattr);
	}

	gettimeofday(&now, NULL);
	limit = now;
	limit.tv_sec += opt_timeout;
	for (;;) {
		t = limit;
		gfarm_timeval_sub(&t, &now);
		timeout = t.tv_sec;
		if (t.tv_usec != 0)
			timeout++;

		e = gfs_replica_fix(url, *iflagsp, timeout, &oflags);
		if (e == GFARM_ERR_RESOURCE_TEMPORARILY_UNAVAILABLE) {
			gettimeofday(&now, NULL);
			now.tv_sec += SLEEP_TIME;
			if (gfarm_timeval_cmp(&limit, &now) > 0) {
				gfarm_sleep(SLEEP_TIME);
				continue;
			}
		}
		if (e != GFARM_ERR_NO_ERROR) {
			fprintf(stderr, "%s: %s\n", url,
			    gfarm_error_string(e));
		}
		if (oflags &
		    GFM_PROTO_REPLICA_FIX_OFLAG_REPLICATION_SHORTAGE) {
			fprintf(stderr, "%s: %s\n", url,
			    "available filesystem node is not enough");
			e = GFARM_ERR_NO_FILESYSTEM_NODE;
		}
		break;
	}
	return (e);
}

static gfarm_error_t
print_replica_spec(
	const char *url, const char *root_url, struct gfs_stat *st, void *val)
{
	gfarm_error_t e;
	int ncopy;
	char *repattr = NULL;

	if (GFARM_S_ISLNK(st->st_mode)) {
		e = GFARM_ERR_IS_A_SYMBOLIC_LINK;
		ERR("%s: %s", url, gfarm_error_string(e));
		return (e);
	}
	if (!GFARM_S_ISREG(st->st_mode) && !GFARM_S_ISDIR(st->st_mode)) {
		ERR("%s: not a file or directory", url);
		return (GFARM_ERR_INVALID_ARGUMENT);
	}

	e = get_replica_spec(url, root_url, &ncopy, &repattr);
	if (e != GFARM_ERR_NO_ERROR)
		return (e);

	print_replica_spec_main(ncopy, repattr);
	free(repattr);

	return (GFARM_ERR_NO_ERROR);
}

static gfarm_error_t
count_replicas(
	const char *url, const char *root_url, struct gfs_stat *st, void *val)
{
	gfarm_error_t e;
	int n;

	if (GFARM_S_ISDIR(st->st_mode)) {
		ERR("%s: is a directory");
		return (GFARM_ERR_IS_A_DIRECTORY);
	}
	e = count_valid(url, &n);
	if (e != GFARM_ERR_NO_ERROR)
		return (e);
	printf("%d\n", n);
	return (e);
}

static int
is_number(const char *str)
{
	const unsigned char *p = (unsigned char *)str;

	if (*p == '-')
		p++;
	while (isdigit(*p))
		p++;
	return (*p == '\0' ? 1 : 0);
}

static void
translate_digit(char *out, int size, const char *in)
{
	const unsigned char *p = (unsigned char *)in;

	while (ISBLANK(*p))
		p++;
#if 0
	if (*p == '-') {
		if (size <= 1)
			goto end;
		*out = *p;
		p++;
		out++;
		size--;
	}
#endif
	while (isdigit(*p)) {
		if (size <= 1)
			goto end;
		*out = *p;
		p++;
		out++;
		size--;
	}
	while (ISBLANK(*p))
		p++;
	while (*p != '\0') {
		if (size <= 1)
			goto end;
		*out = *p;
		p++;
		out++;
		size--;
	}
end:
	*out = '\0';
}

static int set_flags; /* = 0 */

static gfarm_error_t
set_ncopy(
	const char *url, const char *root_url, struct gfs_stat *st, void *val)
{
	gfarm_error_t e;
	char *ncopy_str = val;
	char str[32];

	if (GFARM_S_ISLNK(st->st_mode)) {
		e = GFARM_ERR_IS_A_SYMBOLIC_LINK;
		ERR("%s: %s", url, gfarm_error_string(e));
		return (e);
	}
	if (!GFARM_S_ISREG(st->st_mode) && !GFARM_S_ISDIR(st->st_mode)) {
		ERR("%s: not a file or directory", url);
		return (GFARM_ERR_INVALID_ARGUMENT);
	}

	translate_digit(str, sizeof(str), ncopy_str);
	if (!is_number(str)) {
		ERR("%s: %s: not a number", url, ncopy_str);
		return (GFARM_ERR_INVALID_ARGUMENT);
	}

	e = gfncopy_setxattr(url, GFARM_EA_NCOPY, str, strlen(str), set_flags);
	if (e != GFARM_ERR_NO_ERROR) {
		if (opt_verb)
			ERR("%s: setxattr(%s): %s",
			    url, GFARM_EA_NCOPY, gfarm_error_string(e));
		else
			fprintf(stderr, "%s\n", gfarm_error_string(e));
	}
	return (e);
}

static gfarm_error_t
set_repattr(
	const char *url, const char *root_url, struct gfs_stat *st, void *val)
{
	gfarm_error_t e;
	char *repattr = val;

	if (GFARM_S_ISLNK(st->st_mode)) {
		e = GFARM_ERR_IS_A_SYMBOLIC_LINK;
		ERR("%s: %s", url, gfarm_error_string(e));
		return (e);
	}
	if (!GFARM_S_ISREG(st->st_mode) && !GFARM_S_ISDIR(st->st_mode)) {
		ERR("%s: not a file or directory", url);
		return (GFARM_ERR_INVALID_ARGUMENT);
	}

	e = gfncopy_setxattr(url, GFARM_EA_REPATTR, repattr,
	    strlen(repattr) + 1, set_flags);
	if (e != GFARM_ERR_NO_ERROR) {
		if (opt_verb)
			ERR("%s: setxattr(%s): %s",
			    url, GFARM_EA_REPATTR, gfarm_error_string(e));
		else
			fprintf(stderr, "%s\n", gfarm_error_string(e));
	}
	return (e);
}

static gfarm_error_t
remove_replica_spec(
	const char *url, const char *root_url, struct gfs_stat *st, void *val)
{
	gfarm_error_t e, e2;

	if (GFARM_S_ISLNK(st->st_mode)) {
		e = GFARM_ERR_IS_A_SYMBOLIC_LINK;
		ERR("%s: %s", url, gfarm_error_string(e));
		return (e);
	}
	if (!GFARM_S_ISREG(st->st_mode) && !GFARM_S_ISDIR(st->st_mode)) {
		ERR("%s: not a file or directory", url);
		return (GFARM_ERR_INVALID_ARGUMENT);
	}

	e = gfncopy_removexattr(url, GFARM_EA_NCOPY);
	if (e == GFARM_ERR_NO_SUCH_OBJECT) {
		VERB("%s: removexattr %s: %s",
		    url, GFARM_EA_NCOPY, gfarm_error_string(e));
		e = GFARM_ERR_NO_ERROR;
	} else if (e != GFARM_ERR_NO_ERROR)
		ERR("%s: removexattr(%s): %s",
		    url, GFARM_EA_NCOPY, gfarm_error_string(e));

	e2 = gfncopy_removexattr(url, GFARM_EA_REPATTR);
	if (e2 == GFARM_ERR_NO_SUCH_OBJECT) {
		VERB("%s: removexattr %s: %s",
		    url, GFARM_EA_REPATTR, gfarm_error_string(e2));
		e2 = GFARM_ERR_NO_ERROR;
	} else if (e2 != GFARM_ERR_NO_ERROR)
		ERR("%s: removexattr(%s): %s",
		    url, GFARM_EA_REPATTR, gfarm_error_string(e2));

	return (e == GFARM_ERR_NO_ERROR ? e2 : e);
}

typedef gfarm_error_t (*gfncopy_func)(
	const char *, const char *, struct gfs_stat *, void *);

struct foreach_arg {
	gfarm_int64_t n_error;
	gfncopy_func func;
	const char *root_url;
	void *val;
};

static gfarm_error_t
do_func_recursive(char *url, struct gfs_stat *st, void *a)
{
	gfarm_error_t e;
	struct foreach_arg *argp = a;

	e = argp->func(url, argp->root_url, st, argp->val);
	if (e != GFARM_ERR_NO_ERROR)
		argp->n_error++;
	return (e);
}

static gfarm_int64_t
do_func(gfncopy_func func,
	const char *path, int enable_recursive, int file_only, void *val)
{
	gfarm_error_t e;
	char *url, *root_url;
	struct gfs_stat st;

	e = gfarm2fs_realurl(path, &url, &root_url);
	if (e != GFARM_ERR_NO_ERROR)
		return (1);

	e = gfncopy_stat(url, &st);
	if (e != GFARM_ERR_NO_ERROR) {
		ERR("%s: %s", path, gfarm_error_string(e));
		free(url);
		free(root_url);
		return (1);
	}
	if (enable_recursive && GFARM_S_ISDIR(st.st_mode)) {
		struct foreach_arg arg;

		arg.n_error = 0;
		arg.func = func;
		arg.root_url = root_url;
		arg.val = val;
		e = gfarm_foreach_directory_hierarchy(
		    do_func_recursive, NULL, NULL, url, &arg);
		gfs_stat_free(&st);
		free(url);
		free(root_url);
		return (arg.n_error);
	}
	if (file_only && !GFARM_S_ISREG(st.st_mode)) {
		ERR("%s: not a file", url);
		gfs_stat_free(&st);
		free(url);
		free(root_url);
		return (1); /* ignore */
	}

	e = func(url, root_url, &st, val);
	gfs_stat_free(&st);
	free(url);
	free(root_url);
	return (e != GFARM_ERR_NO_ERROR ? 1 : 0);
}

static gfarm_int64_t
handle_arg1(
	gfncopy_func func,
	int argc, char **argv, int enable_recursive, int file_only, void *val)
{
	if (argc != 1)
		usage();

	return (do_func(func, argv[0], enable_recursive, file_only, val));
}

static gfarm_int64_t
handle_args(
	gfncopy_func func,
	int argc, char **argv, int enable_recursive, int file_only, void *val)
{
	gfarm_int64_t n_error = 0;

	if (argc <= 0)
		usage();

	while (argc > 0) {
		n_error += do_func(
		    func, argv[0], enable_recursive, file_only, val);
		argc--;
		argv++;
	}

	if (opt_verb && n_error > 0)
		ERR("%d files failed", n_error);
	return (n_error);
}

static gfarm_uint64_t
parse_fix_flags(const char *option)
{
	if (strcmp(option, "replication_request") == 0)
		return (GFM_PROTO_REPLICA_FIX_IFLAG_REPLICATION_REQUEST);
	else if (strcmp(option, "replication_wait") == 0)
		return (GFM_PROTO_REPLICA_FIX_IFLAG_REPLICATION_WAIT);
	else if (strcmp(option, "retry_when_afford") == 0)
		return (GFM_PROTO_REPLICA_FIX_IFLAG_RETRY_WHEN_AFFORD);
	else if (strcmp(option, "retry_when_closed") == 0)
		return (GFM_PROTO_REPLICA_FIX_IFLAG_RETRY_WHEN_CLOSED);
	else {
		fprintf(stderr, "%s: unknown -F flag \"%s\"\n",
		    program_name, option);
		usage();
		/*NOTREACHED*/
		return (0);
	}
}

int
main(int argc, char **argv)
{
	enum {
		MODE_GET, MODE_NCOPY, MODE_REPLICAINFO, MODE_REMOVE,
		MODE_COUNT, MODE_WAIT, MODE_NONE,
	} opt_mode = MODE_NONE;
	gfarm_error_t e;
	int c, opt_nofollow = 0;
	gfarm_int64_t n_error = 0;
	gfarm_uint64_t fix_flags = 0;
	char *repattr = NULL, *ncopy_str = NULL;

	if (argc > 0)
		program_name = basename(argv[0]);

	e = gfarm_initialize(&argc, &argv);
	if (e != GFARM_ERR_NO_ERROR) {
		ERR("gfarm_initialize: %s", gfarm_error_string(e));
		exit(1);
	}

	while ((c = getopt(argc, argv, "s:S:CMrcF:wt:vh?")) != -1) {
		switch (c) {
		case 's':
			if (opt_mode != MODE_NONE)
				usage();
			opt_mode = MODE_NCOPY;
			ncopy_str = optarg;
			break;
		case 'S':
			if (opt_mode != MODE_NONE)
				usage();
			opt_mode = MODE_REPLICAINFO;
			repattr = optarg;
			break;
		case 'C':
			if (set_flags != 0)
				usage();
			set_flags = GFS_XATTR_CREATE;
			break;
		case 'M':
			if (set_flags != 0)
				usage();
			set_flags = GFS_XATTR_REPLACE;
			break;
		case 'r':
			if (opt_mode != MODE_NONE)
				usage();
			opt_mode = MODE_REMOVE;
			break;
		case 'c':
			if (opt_mode != MODE_NONE)
				usage();
			opt_mode = MODE_COUNT;
			break;
		case 'F':
			if (opt_mode != MODE_NONE && opt_mode != MODE_WAIT)
				usage();
			opt_mode = MODE_WAIT;
			fix_flags |= parse_fix_flags(optarg);
			break;
		case 'w':
			if (opt_mode != MODE_NONE && opt_mode != MODE_WAIT)
				usage();
			opt_mode = MODE_WAIT;
			fix_flags |=
			    GFM_PROTO_REPLICA_FIX_IFLAG_REPLICATION_REQUEST|
			    GFM_PROTO_REPLICA_FIX_IFLAG_REPLICATION_WAIT|
			    GFM_PROTO_REPLICA_FIX_IFLAG_RETRY_WHEN_AFFORD;
			break;
		case 't':
			opt_timeout = atoi(optarg);
			break;
		case 'v':
			opt_verb = 1;
			break;
		case 'h':
			opt_nofollow = 1;
			break;
		case '?':
		default:
			usage();
		}
	}
	argc -= optind;
	argv += optind;

	if (opt_mode == MODE_NONE)
		opt_mode = MODE_GET;

	gfs_stat_cache_enable(1);
	gfarm_xattr_caching_pattern_add(GFARM_EA_NCOPY);
	gfarm_xattr_caching_pattern_add(GFARM_EA_REPATTR);

	if (opt_nofollow || opt_mode == MODE_WAIT) {
		gfncopy_stat = gfs_lstat_cached;
		gfncopy_getxattr = gfs_lgetxattr_cached;
		gfncopy_setxattr = gfs_lsetxattr;
		gfncopy_removexattr = gfs_lremovexattr;
	} else {
		gfncopy_stat = gfs_stat_cached;
		gfncopy_getxattr = gfs_getxattr_cached;
		gfncopy_setxattr = gfs_setxattr;
		gfncopy_removexattr = gfs_removexattr;
	}

	switch (opt_mode) {
	case MODE_GET:
		n_error += handle_arg1(
		    print_replica_spec, argc, argv, 0, 0, NULL);
		break;
	case MODE_NCOPY:
		n_error += handle_arg1(set_ncopy, argc, argv, 0, 0, ncopy_str);
		break;
	case MODE_REPLICAINFO:
		n_error += handle_arg1(
		    set_repattr, argc, argv, 0, 0, repattr);
		break;
	case MODE_REMOVE:
		n_error += handle_args(
		    remove_replica_spec, argc, argv, 0, 0, NULL);
		break;
	case MODE_COUNT:
		n_error += handle_arg1(count_replicas, argc, argv, 0, 0, NULL);
		break;
	case MODE_WAIT:
		n_error += handle_args(
		    wait_for_replication, argc, argv, 1, 1, &fix_flags);
		break;
	default:
		usage();
	}

	e = gfarm_terminate();
	if (e != GFARM_ERR_NO_ERROR) {
		ERR("gfarm_terminate: %s", gfarm_error_string(e));
		exit(1);
	}

	return (n_error > 0 ? 1 : 0);
}
