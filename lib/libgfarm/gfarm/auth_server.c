#include <stdio.h>
#include <assert.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <errno.h>
#include <grp.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <time.h>
#include <pwd.h>
#include <gfarm/gfarm_config.h>
#include <gfarm/error.h>
#include <gfarm/gfarm_misc.h>
#include "liberror.h"
#include "gfutil.h"
#include "gfp_xdr.h"
#include "hostspec.h"
#include "auth.h"

static gfarm_error_t gfarm_authorize_panic(struct gfp_xdr *, int,
	char *, char *,	enum gfarm_auth_id_type *, char **);

gfarm_error_t (*gfarm_authorization_table[])(struct gfp_xdr *, int,
	char *, char *, enum gfarm_auth_id_type *, char **) = {
	/*
	 * This table entry should be ordered by enum gfarm_auth_method.
	 */
	gfarm_authorize_panic,		/* GFARM_AUTH_METHOD_NONE */
	gfarm_authorize_sharedsecret,	/* GFARM_AUTH_METHOD_SHAREDSECRET */
	gfarm_authorize_panic,		/* GFARM_AUTH_METHOD_GSI_OLD */
#ifdef HAVE_GSI
	gfarm_authorize_gsi,		/* GFARM_AUTH_METHOD_GSI */
	gfarm_authorize_gsi_auth,	/* GFARM_AUTH_METHOD_GSI_AUTH */
#else
	gfarm_authorize_panic,		/* GFARM_AUTH_METHOD_GSI */
	gfarm_authorize_panic,		/* GFARM_AUTH_METHOD_GSI_AUTH */
#endif
};

static gfarm_error_t
gfarm_authorize_panic(struct gfp_xdr *conn, int switch_to,
	char *service_tag, char *hostname,
	enum gfarm_auth_id_type *peer_typep, char **global_usernamep)
{
	gflog_fatal("gfarm_authorize: authorization assertion failed");
	return (GFARM_ERR_PROTOCOL);
}

static gfarm_error_t
gfarm_auth_sharedsecret_response(struct gfp_xdr *conn, char *homedir)
{
	gfarm_error_t e;
	gfarm_uint32_t request, expire, expire_expected;
	size_t len;
	gfarm_int32_t error = GFARM_AUTH_ERROR_EXPIRED; /* gfarm_auth_error */
	int eof, try = 0;
	char challenge[GFARM_AUTH_CHALLENGE_LEN];
	char response[GFARM_AUTH_RESPONSE_LEN];
	char shared_key_expected[GFARM_AUTH_SHARED_KEY_LEN];
	char response_expected[GFARM_AUTH_RESPONSE_LEN];

	/* NOTE: `homedir' may be NULL, if invalid username is requested. */

	for (;;) {
		++try;
		e = gfp_xdr_recv(conn, 0, &eof, "i", &request);
		if (e != GFARM_ERR_NO_ERROR) {
			gflog_error("auth_sharedsecret_response: %s",
			    gfarm_error_string(e));
			return (e);
		}
		if (eof) {
			gflog_error("auth_sharedsecret_response: "
			    "unexpected EOF");
			return (GFARM_ERR_PROTOCOL);
		}
		if (request != GFARM_AUTH_SHAREDSECRET_GIVEUP &&
		    request != GFARM_AUTH_SHAREDSECRET_MD5) {
			error = GFARM_AUTH_ERROR_NOT_SUPPORTED;
			e = gfp_xdr_send(conn, "i", error);
			if (e != GFARM_ERR_NO_ERROR) {
				gflog_error("auth_sharedsecret: key type: %s",
				    gfarm_error_string(e));
				return (e);
			}
			continue;
		}
		e = gfp_xdr_send(conn, "i", GFARM_AUTH_ERROR_NO_ERROR);
		if (e != GFARM_ERR_NO_ERROR) {
			gflog_error("auth_sharedsecret: key query: %s",
			    gfarm_error_string(e));
			return (e);
		}
		if (request == GFARM_AUTH_SHAREDSECRET_GIVEUP) {
			e = gfp_xdr_flush(conn);
			if (e != GFARM_ERR_NO_ERROR) {
				gflog_error("auth_sharedsecret: cut: %s",
				    gfarm_error_string(e));
			} else if (try <= 1) {
				e = GFARM_ERR_AUTHENTICATION;
				gflog_error("auth_sharedsecret: scaned: %s",
				    gfarm_error_string(e));
			} else {
				switch (error) {
				case GFARM_AUTH_ERROR_EXPIRED:
					e = GFARM_ERR_EXPIRED;
					break;
				case GFARM_AUTH_ERROR_NOT_SUPPORTED:
					e = GFARM_ERR_PROTOCOL_NOT_SUPPORTED;
					break;
				default:
					e = GFARM_ERR_AUTHENTICATION;
					break;
				}
				gflog_error("auth_sharedsecret: gives up: %s",
				    gfarm_error_string(e));
			}
			return (e);
		}

		gfarm_auth_random(challenge, sizeof(challenge));
		e = gfp_xdr_send(conn, "b", sizeof(challenge), challenge);
		if (e != GFARM_ERR_NO_ERROR) {
			gflog_error("auth_sharedsecret: challenge: %s",
			    gfarm_error_string(e));
			return (e);
		}
		e = gfp_xdr_recv(conn, 0, &eof, "ib",
		    &expire, sizeof(response), &len, response);
		if (e != GFARM_ERR_NO_ERROR) {
			gflog_error("auth_sharedsecret: response: %s",
			    gfarm_error_string(e));
			return (e);
		}
		if (eof) {
			gflog_error("auth_sharedsecret: "
			    "unexpected EOF in response");
			return (GFARM_ERR_PROTOCOL);
		}
		/*
		 * Note that gfarm_auth_shared_key_get() should be called
		 * after the above gfp_xdr_recv(), otherwise
		 * client (re)generated shared key may not be accessible.
		 */
		if (homedir == NULL) {
			error = GFARM_AUTH_ERROR_INVALID_CREDENTIAL;
			/* already logged at gfarm_authorize_sharedsecret() */
		} else if ((e = gfarm_auth_shared_key_get(&expire_expected,
		    shared_key_expected, homedir, GFARM_AUTH_SHARED_KEY_GET,0))
		    != GFARM_ERR_NO_ERROR && e != GFARM_ERR_EXPIRED) {
			error = GFARM_AUTH_ERROR_INVALID_CREDENTIAL;
			gflog_error("auth_sharedsecret: .gfarm_shared_key: %s",
			    gfarm_error_string(e));
		} else if (time(0) >= expire) {
			/* may reach here if (e == GFARM_ERR_EXPIRED) */
			error = GFARM_AUTH_ERROR_EXPIRED;
			gflog_warning("auth_sharedsecret: key expired");
		} else {
			/* may also reach here if (e == GFARM_ERR_EXPIRED) */
			gfarm_auth_sharedsecret_response_data(
			    shared_key_expected, challenge, response_expected);
			if (expire != expire_expected) {
				error = GFARM_AUTH_ERROR_INVALID_CREDENTIAL;
				gflog_error("auth_sharedsecret: "
				    "expire time mismatch");
			} else if (memcmp(response, response_expected,
			    sizeof(response)) != 0) {
				error = GFARM_AUTH_ERROR_INVALID_CREDENTIAL;
				gflog_error("auth_sharedsecret: "
				    "key mismatch");
			} else { /* success */
				error = GFARM_AUTH_ERROR_NO_ERROR;
			}
		}
		e = gfp_xdr_send(conn, "i", error);
		if (e != GFARM_ERR_NO_ERROR) {
			gflog_error("auth_sharedsecret: send result: %s",
			    gfarm_error_string(e));
			return (e);
		}
		if (error == GFARM_AUTH_ERROR_NO_ERROR) {
			e = gfp_xdr_flush(conn);
			if (e != GFARM_ERR_NO_ERROR) {
				gflog_error(
				    "auth_sharedsecret: completion: %s",
				    gfarm_error_string(e));
				return (e);
			}
			return (GFARM_ERR_NO_ERROR); /* success */
		}
	}
}

gfarm_error_t
gfarm_authorize_sharedsecret(struct gfp_xdr *conn, int switch_to,
	char *service_tag, char *hostname,
	enum gfarm_auth_id_type *peer_typep, char **global_usernamep)
{
	gfarm_error_t e;
	char *global_username, *local_username, *aux, *msg;
	int eof;
	struct passwd *pwd;
	uid_t o_uid;
	gid_t o_gid;
	static char method[] = "auth=sharedsecret local_user=";

#ifdef __GNUC__ /* workaround gcc warning: unused variable */
	o_uid = o_gid = 0;
#endif
	e = gfp_xdr_recv(conn, 0, &eof, "s", &global_username);
	if (e != GFARM_ERR_NO_ERROR) {
		gflog_error("authorize_sharedsecret: reading username");
		return (e);
	}
	if (eof) {
		gflog_error("authorize_sharedsecret: unexpected EOF");
		return (GFARM_ERR_PROTOCOL);
	}

	aux = malloc(strlen(global_username) + 1 + strlen(hostname) + 1);
	if (aux == NULL) {
		gflog_error("authorize_sharedsecret: %s",
		    gfarm_error_string(GFARM_ERR_NO_MEMORY));
		return (GFARM_ERR_NO_MEMORY);
	}
	sprintf(aux, "%s@%s", global_username, hostname);
	gflog_set_auxiliary_info(aux);

	/* XXX NOTYET determine *peer_typep == GFARM_AUTH_ID_TYPE_SPOOL_HOST */
	e = gfarm_global_to_local_username(global_username, &local_username);
	if (e != GFARM_ERR_NO_ERROR) {
		pwd = NULL;
		gflog_error("authorize_sharedsecret: "
		    "cannot map global username into local username");
	} else {
		pwd = getpwnam(local_username);
		if (pwd == NULL)
			gflog_error("%s: authorize_sharedsecret: "
			    "local account doesn't exist", local_username);
	}

	if (pwd != NULL) {
		/*
		 * first, switch to the user's privilege
		 * to read ~/.gfarm_shared_key.
		 *
		 * NOTE: reading this file with root privilege may not work,
		 *	if home directory is NFS mounted and root access for
		 *	the home directory partition is not permitted.
		 *
		 * Do not switch the user ID of the current process here
		 * even in the switch_to case, because it is necessary to
		 * switch back to the original user ID when
		 * gfarm_auth_sharedsecret fails.
		 */
		o_gid = getegid();
		o_uid = geteuid();
		initgroups(pwd->pw_name, pwd->pw_gid);
		setegid(pwd->pw_gid);
		seteuid(pwd->pw_uid);
	}
	e = gfarm_auth_sharedsecret_response(conn,
	    pwd == NULL ? NULL : pwd->pw_dir);
	if (pwd != NULL) {
		setegid(o_gid);
		seteuid(o_uid);
	}

	if (e == GFARM_ERR_NO_ERROR) {
		/* succeed, do logging */

		msg = malloc(sizeof(method) + strlen(local_username));
		if (msg == NULL) {
			e = GFARM_ERR_NO_MEMORY;
			gflog_error("authorize_sharedsecret: %s",
			    gfarm_error_string(e));
		} else {
			sprintf(msg, "%s%s", method, local_username);
			gflog_notice("authenticated: %s", msg);
			free(msg);
		}
	}

	/* if (pwd == NULL), (e != GFARM_ERR_NO_ERROR) must be true here */
	if (e != GFARM_ERR_NO_ERROR) {
		free(gflog_get_auxiliary_info());
		gflog_set_auxiliary_info(NULL);
		free(global_username);
		if (local_username != NULL)
			free(local_username);
		return (e);
	}

	if (switch_to) {
		/*
		 * because the name returned by getlogin() is
		 * an attribute of a session on 4.4BSD derived OSs,
		 * we should create new session before calling
		 * setlogin().
		 */
		setsid();
#ifdef HAVE_SETLOGIN
		setlogin(pwd->pw_name);
#endif
		setgid(pwd->pw_gid);
		setuid(pwd->pw_uid);

		/* assert(local_username != NULL); */
		gfarm_set_global_username(global_username);
		gfarm_set_local_username(local_username);
		gfarm_set_local_homedir(pwd->pw_dir);
	} else {
		gflog_set_auxiliary_info(NULL);
		free(aux);
	}
	free(local_username);
	/* XXX NOTYET determine *peer_typep == GFARM_AUTH_ID_TYPE_SPOOL_HOST */
	if (peer_typep != NULL)
		*peer_typep = GFARM_AUTH_ID_TYPE_USER;
	if (global_usernamep != NULL)
		*global_usernamep = global_username;
	else
		free(global_username);
	return (GFARM_ERR_NO_ERROR);
}

/*
 * the `switch_to' flag has the following side effects:
 *      - the privilege of this program will switch to the authenticated user.
 *      - gflog_set_auxiliary_info("user@hostname") will be called.
 *        thus, the caller of gfarm_authorize() must call the following later:
 *              char *aux = gflog_get_auxiliary_info();
 *              gflog_get_auxiliary_info(NULL);
 *              free(aux);
 *      - gfarm_get_local_username(), gfarm_get_local_homedir() and
 *        gfarm_get_global_username() become available.
 *
 * note that the user's account is not always necessary on this host,
 * if the `switch_to' flag isn't set. but also note that some
 * authentication methods (e.g. "sharedsecret") require the user's
 * local account anyway even if the `switch_to' isn't set.
 */
gfarm_error_t
gfarm_authorize(struct gfp_xdr *conn,
	int switch_to, char *service_tag,
	enum gfarm_auth_id_type *peer_typep, char **global_usernamep,
	char **hostnamep, enum gfarm_auth_method *auth_methodp)
{
	gfarm_error_t e;
	char *hostname;
	gfarm_int32_t methods; /* bitset of enum gfarm_auth_method */
	gfarm_int32_t method; /* enum gfarm_auth_method */
	gfarm_int32_t error; /* enum gfarm_auth_error */
	struct sockaddr addr;
	socklen_t addrlen = sizeof(addr);
	int rv = getpeername(gfp_xdr_fd(conn), &addr, &addrlen);
	int i, eof, try = 0;
	size_t nmethods;
	unsigned char methods_buffer[CHAR_BIT * sizeof(gfarm_int32_t)];
	char addr_string[GFARM_SOCKADDR_STRLEN];

	assert(GFARM_ARRAY_LENGTH(gfarm_authorization_table) ==
	    GFARM_AUTH_METHOD_NUMBER);

	if (rv == -1) {
		e = gfarm_errno_to_error(errno);
		gflog_error("authorize: getpeername: %s",
		    gfarm_error_string(e));
		return (e);
	}
	e = gfarm_sockaddr_to_name(&addr, &hostname);
	if (e != GFARM_ERR_NO_ERROR) {
		gfarm_sockaddr_to_string(&addr,
		    addr_string, GFARM_SOCKADDR_STRLEN);
		gflog_warning("%s: %s", addr_string, gfarm_error_string(e));
		hostname = strdup(addr_string);
		if (hostname == NULL) {
			gflog_warning("%s: %s", addr_string,
			    gfarm_error_string(GFARM_ERR_NO_MEMORY));
			return (GFARM_ERR_NO_MEMORY);
		}
	}

	methods = gfarm_auth_method_get_enabled_by_name_addr(hostname, &addr);
	if (methods == 0) {
		gflog_error("%s: refusing access", hostname);
	} else {
		methods &= gfarm_auth_method_get_available();
		if (methods == 0)
			gflog_error("%s: auth-method not configured",
			    hostname);
	}

	nmethods = 0;
	for (i = GFARM_AUTH_METHOD_NONE + 1; i < GFARM_AUTH_METHOD_NUMBER &&
	    i < CHAR_BIT * sizeof(gfarm_int32_t); i++) {
		if ((methods & (1 << i)) != 0)
			methods_buffer[nmethods++] = i;
	}
	e = gfp_xdr_send(conn, "b", nmethods, methods_buffer);
	if (e != GFARM_ERR_NO_ERROR) {
		gflog_error("%s: %s", hostname, gfarm_error_string(e));
		free(hostname);
		return (e);
	}
	for (;;) {
		++try;
		e = gfp_xdr_recv(conn, 0, &eof, "i", &method);
		if (e != GFARM_ERR_NO_ERROR) {
			gflog_error("%s: %s", hostname, gfarm_error_string(e));
			free(hostname);
			return (e);
		}
		if (eof) {
			if (try <= 1)
				gflog_warning("%s: port scan", hostname);
			else
				gflog_warning("%s: client disappeared",
				    hostname);
			free(hostname);
			return (GFARM_ERR_PROTOCOL);
		}
		if (method == GFARM_AUTH_METHOD_NONE)
			error = GFARM_AUTH_ERROR_NO_ERROR;
		else if (method >= GFARM_AUTH_METHOD_NUMBER)
			error = GFARM_AUTH_ERROR_NOT_SUPPORTED;
		else if (method <= GFARM_AUTH_METHOD_NONE ||
		    ((1 << method) & methods) == 0)
			error = GFARM_AUTH_ERROR_DENIED;
		else
			error = GFARM_AUTH_ERROR_NO_ERROR;
		e = gfp_xdr_send(conn, "i", error);
		if (e == GFARM_ERR_NO_ERROR)
			e = gfp_xdr_flush(conn);
		if (e != GFARM_ERR_NO_ERROR) {
			gflog_error("%s: %s", hostname, gfarm_error_string(e));
			free(hostname);
			return (e);
		}
		if (error != GFARM_AUTH_ERROR_NO_ERROR) {
			gflog_error("%s: incorrect auth-method request",
			    hostname);
			free(hostname);
			return (GFARM_ERR_PROTOCOL);
		}
		if (method == GFARM_AUTH_METHOD_NONE) {
			/* client gave up */
			if (methods == 0) {
				e = GFARM_ERR_PERMISSION_DENIED;
			} else if (try <= 1) {
				/*
				 * there is no usable auth-method
				 * between client and server.
				 */
				gflog_notice("%s: authentication method "
				    "doesn't match", hostname);
				e = GFARM_ERR_PROTOCOL_NOT_SUPPORTED;
			} else {
				e = GFARM_ERR_AUTHENTICATION;
			}
			free(hostname);
			return (e);
		}

		e = (*gfarm_authorization_table[method])(conn, switch_to,
			service_tag, hostname, peer_typep, global_usernamep);
		if (e != GFARM_ERR_PROTOCOL_NOT_SUPPORTED &&
		    e != GFARM_ERR_EXPIRED &&
		    e != GFARM_ERR_AUTHENTICATION) {
			/* protocol error, or success */
			if (e != GFARM_ERR_NO_ERROR || hostnamep == NULL)
				free(hostname);
			else
				*hostnamep = hostname;
			if (e == GFARM_ERR_NO_ERROR) {
				if (auth_methodp != NULL)
					*auth_methodp = method;
			}
			return (e);
		}
	}
}
