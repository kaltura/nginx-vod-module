// TODO: add caching of file handles

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_event.h>

#if (NGX_THREADS)

#include "ngx_async_open_file.h"

///////////////////////////////////////////////////////////////////////
//	start code copied from ngx_open_file_cache (in order to expose ngx_open_and_stat_file)

#if (NGX_HAVE_OPENAT)
static ngx_fd_t ngx_openat_file_owner(ngx_fd_t at_fd, const u_char *name,
	ngx_int_t mode, ngx_int_t create, ngx_int_t access, ngx_log_t *log);
#if (NGX_HAVE_O_PATH)
static ngx_int_t ngx_file_o_path_info(ngx_fd_t fd, ngx_file_info_t *fi,
	ngx_log_t *log);
#endif
#endif
static ngx_fd_t ngx_open_file_wrapper(ngx_str_t *name,
	ngx_open_file_info_t *of, ngx_int_t mode, ngx_int_t create,
	ngx_int_t access, ngx_log_t *log);
static ngx_int_t ngx_file_info_wrapper(ngx_str_t *name,
	ngx_open_file_info_t *of, ngx_file_info_t *fi, ngx_log_t *log);
static ngx_int_t ngx_open_and_stat_file(ngx_str_t *name,
	ngx_open_file_info_t *of, ngx_log_t *log);


#define NGX_MIN_READ_AHEAD  (128 * 1024)

#if (NGX_HAVE_OPENAT)

static ngx_fd_t
ngx_openat_file_owner(ngx_fd_t at_fd, const u_char *name,
    ngx_int_t mode, ngx_int_t create, ngx_int_t access, ngx_log_t *log)
{
    ngx_fd_t         fd;
    ngx_err_t        err;
    ngx_file_info_t  fi, atfi;

    /*
     * To allow symlinks with the same owner, use openat() (followed
     * by fstat()) and fstatat(AT_SYMLINK_NOFOLLOW), and then compare
     * uids between fstat() and fstatat().
     *
     * As there is a race between openat() and fstatat() we don't
     * know if openat() in fact opened symlink or not.  Therefore,
     * we have to compare uids even if fstatat() reports the opened
     * component isn't a symlink (as we don't know whether it was
     * symlink during openat() or not).
     */

    fd = ngx_openat_file(at_fd, name, mode, create, access);

    if (fd == NGX_INVALID_FILE) {
        return NGX_INVALID_FILE;
    }

    if (ngx_file_at_info(at_fd, name, &atfi, AT_SYMLINK_NOFOLLOW)
        == NGX_FILE_ERROR)
    {
        err = ngx_errno;
        goto failed;
    }

#if (NGX_HAVE_O_PATH)
    if (ngx_file_o_path_info(fd, &fi, log) == NGX_ERROR) {
        err = ngx_errno;
        goto failed;
    }
#else
    if (ngx_fd_info(fd, &fi) == NGX_FILE_ERROR) {
        err = ngx_errno;
        goto failed;
    }
#endif

    if (fi.st_uid != atfi.st_uid) {
        err = NGX_ELOOP;
        goto failed;
    }

    return fd;

failed:

    if (ngx_close_file(fd) == NGX_FILE_ERROR) {
        ngx_log_error(NGX_LOG_ALERT, log, ngx_errno,
                      ngx_close_file_n " \"%V\" failed", name);
    }

    ngx_set_errno(err);

    return NGX_INVALID_FILE;
}


#if (NGX_HAVE_O_PATH)

static ngx_int_t
ngx_file_o_path_info(ngx_fd_t fd, ngx_file_info_t *fi, ngx_log_t *log)
{
    static ngx_uint_t  use_fstat = 1;

    /*
     * In Linux 2.6.39 the O_PATH flag was introduced that allows to obtain
     * a descriptor without actually opening file or directory.  It requires
     * less permissions for path components, but till Linux 3.6 fstat() returns
     * EBADF on such descriptors, and fstatat() with the AT_EMPTY_PATH flag
     * should be used instead.
     *
     * Three scenarios are handled in this function:
     *
     * 1) The kernel is newer than 3.6 or fstat() with O_PATH support was
     *    backported by vendor.  Then fstat() is used.
     *
     * 2) The kernel is newer than 2.6.39 but older than 3.6.  In this case
     *    the first call of fstat() returns EBADF and we fallback to fstatat()
     *    with AT_EMPTY_PATH which was introduced at the same time as O_PATH.
     *
     * 3) The kernel is older than 2.6.39 but nginx was build with O_PATH
     *    support.  Since descriptors are opened with O_PATH|O_RDONLY flags
     *    and O_PATH is ignored by the kernel then the O_RDONLY flag is
     *    actually used.  In this case fstat() just works.
     */

    if (use_fstat) {
        if (ngx_fd_info(fd, fi) != NGX_FILE_ERROR) {
            return NGX_OK;
        }

        if (ngx_errno != NGX_EBADF) {
            return NGX_ERROR;
        }

        ngx_log_error(NGX_LOG_NOTICE, log, 0,
                      "fstat(O_PATH) failed with EBADF, "
                      "switching to fstatat(AT_EMPTY_PATH)");

        use_fstat = 0;
    }

    if (ngx_file_at_info(fd, "", fi, AT_EMPTY_PATH) != NGX_FILE_ERROR) {
        return NGX_OK;
    }

    return NGX_ERROR;
}

#endif

#endif /* NGX_HAVE_OPENAT */


static ngx_fd_t
ngx_open_file_wrapper(ngx_str_t *name, ngx_open_file_info_t *of,
    ngx_int_t mode, ngx_int_t create, ngx_int_t access, ngx_log_t *log)
{
    ngx_fd_t  fd;

#if !(NGX_HAVE_OPENAT)

    fd = ngx_open_file(name->data, mode, create, access);

    if (fd == NGX_INVALID_FILE) {
        of->err = ngx_errno;
        of->failed = ngx_open_file_n;
        return NGX_INVALID_FILE;
    }

    return fd;

#else

    u_char           *p, *cp, *end;
    ngx_fd_t          at_fd;
    ngx_str_t         at_name;

    if (of->disable_symlinks == NGX_DISABLE_SYMLINKS_OFF) {
        fd = ngx_open_file(name->data, mode, create, access);

        if (fd == NGX_INVALID_FILE) {
            of->err = ngx_errno;
            of->failed = ngx_open_file_n;
            return NGX_INVALID_FILE;
        }

        return fd;
    }

    p = name->data;
    end = p + name->len;

    at_name = *name;

    if (of->disable_symlinks_from) {

        cp = p + of->disable_symlinks_from;

        *cp = '\0';

        at_fd = ngx_open_file(p, NGX_FILE_SEARCH|NGX_FILE_NONBLOCK,
                              NGX_FILE_OPEN, 0);

        *cp = '/';

        if (at_fd == NGX_INVALID_FILE) {
            of->err = ngx_errno;
            of->failed = ngx_open_file_n;
            return NGX_INVALID_FILE;
        }

        at_name.len = of->disable_symlinks_from;
        p = cp + 1;

    } else if (*p == '/') {

        at_fd = ngx_open_file("/",
                              NGX_FILE_SEARCH|NGX_FILE_NONBLOCK,
                              NGX_FILE_OPEN, 0);

        if (at_fd == NGX_INVALID_FILE) {
            of->err = ngx_errno;
            of->failed = ngx_openat_file_n;
            return NGX_INVALID_FILE;
        }

        at_name.len = 1;
        p++;

    } else {
        at_fd = NGX_AT_FDCWD;
    }

    for ( ;; ) {
        cp = ngx_strlchr(p, end, '/');
        if (cp == NULL) {
            break;
        }

        if (cp == p) {
            p++;
            continue;
        }

        *cp = '\0';

        if (of->disable_symlinks == NGX_DISABLE_SYMLINKS_NOTOWNER) {
            fd = ngx_openat_file_owner(at_fd, p,
                                       NGX_FILE_SEARCH|NGX_FILE_NONBLOCK,
                                       NGX_FILE_OPEN, 0, log);

        } else {
            fd = ngx_openat_file(at_fd, p,
                           NGX_FILE_SEARCH|NGX_FILE_NONBLOCK|NGX_FILE_NOFOLLOW,
                           NGX_FILE_OPEN, 0);
        }

        *cp = '/';

        if (fd == NGX_INVALID_FILE) {
            of->err = ngx_errno;
            of->failed = ngx_openat_file_n;
            goto failed;
        }

        if (at_fd != NGX_AT_FDCWD && ngx_close_file(at_fd) == NGX_FILE_ERROR) {
            ngx_log_error(NGX_LOG_ALERT, log, ngx_errno,
                          ngx_close_file_n " \"%V\" failed", &at_name);
        }

        p = cp + 1;
        at_fd = fd;
        at_name.len = cp - at_name.data;
    }

    if (p == end) {

        /*
         * If pathname ends with a trailing slash, assume the last path
         * component is a directory and reopen it with requested flags;
         * if not, fail with ENOTDIR as per POSIX.
         *
         * We cannot rely on O_DIRECTORY in the loop above to check
         * that the last path component is a directory because
         * O_DIRECTORY doesn't work on FreeBSD 8.  Fortunately, by
         * reopening a directory, we don't depend on it at all.
         */

        fd = ngx_openat_file(at_fd, ".", mode, create, access);
        goto done;
    }

    if (of->disable_symlinks == NGX_DISABLE_SYMLINKS_NOTOWNER
        && !(create & (NGX_FILE_CREATE_OR_OPEN|NGX_FILE_TRUNCATE)))
    {
        fd = ngx_openat_file_owner(at_fd, p, mode, create, access, log);

    } else {
        fd = ngx_openat_file(at_fd, p, mode|NGX_FILE_NOFOLLOW, create, access);
    }

done:

    if (fd == NGX_INVALID_FILE) {
        of->err = ngx_errno;
        of->failed = ngx_openat_file_n;
    }

failed:

    if (at_fd != NGX_AT_FDCWD && ngx_close_file(at_fd) == NGX_FILE_ERROR) {
        ngx_log_error(NGX_LOG_ALERT, log, ngx_errno,
                      ngx_close_file_n " \"%V\" failed", &at_name);
    }

    return fd;
#endif
}


static ngx_int_t
ngx_file_info_wrapper(ngx_str_t *name, ngx_open_file_info_t *of,
    ngx_file_info_t *fi, ngx_log_t *log)
{
    ngx_int_t  rc;

#if !(NGX_HAVE_OPENAT)

    rc = ngx_file_info(name->data, fi);

    if (rc == NGX_FILE_ERROR) {
        of->err = ngx_errno;
        of->failed = ngx_file_info_n;
        return NGX_FILE_ERROR;
    }

    return rc;

#else

    ngx_fd_t  fd;

    if (of->disable_symlinks == NGX_DISABLE_SYMLINKS_OFF) {

        rc = ngx_file_info(name->data, fi);

        if (rc == NGX_FILE_ERROR) {
            of->err = ngx_errno;
            of->failed = ngx_file_info_n;
            return NGX_FILE_ERROR;
        }

        return rc;
    }

    fd = ngx_open_file_wrapper(name, of, NGX_FILE_RDONLY|NGX_FILE_NONBLOCK,
                               NGX_FILE_OPEN, 0, log);

    if (fd == NGX_INVALID_FILE) {
        return NGX_FILE_ERROR;
    }

    rc = ngx_fd_info(fd, fi);

    if (rc == NGX_FILE_ERROR) {
        of->err = ngx_errno;
        of->failed = ngx_fd_info_n;
    }

    if (ngx_close_file(fd) == NGX_FILE_ERROR) {
        ngx_log_error(NGX_LOG_ALERT, log, ngx_errno,
                      ngx_close_file_n " \"%V\" failed", name);
    }

    return rc;
#endif
}


static ngx_int_t
ngx_open_and_stat_file(ngx_str_t *name, ngx_open_file_info_t *of,
    ngx_log_t *log)
{
    ngx_fd_t         fd;
    ngx_file_info_t  fi;

    if (of->fd != NGX_INVALID_FILE) {

        if (ngx_file_info_wrapper(name, of, &fi, log) == NGX_FILE_ERROR) {
            of->fd = NGX_INVALID_FILE;
            return NGX_ERROR;
        }

        if (of->uniq == ngx_file_uniq(&fi)) {
            goto done;
        }

    } else if (of->test_dir) {

        if (ngx_file_info_wrapper(name, of, &fi, log) == NGX_FILE_ERROR) {
            of->fd = NGX_INVALID_FILE;
            return NGX_ERROR;
        }

        if (ngx_is_dir(&fi)) {
            goto done;
        }
    }

    if (!of->log) {

        /*
         * Use non-blocking open() not to hang on FIFO files, etc.
         * This flag has no effect on a regular files.
         */

        fd = ngx_open_file_wrapper(name, of, NGX_FILE_RDONLY|NGX_FILE_NONBLOCK,
                                   NGX_FILE_OPEN, 0, log);

    } else {
        fd = ngx_open_file_wrapper(name, of, NGX_FILE_APPEND,
                                   NGX_FILE_CREATE_OR_OPEN,
                                   NGX_FILE_DEFAULT_ACCESS, log);
    }

    if (fd == NGX_INVALID_FILE) {
        of->fd = NGX_INVALID_FILE;
        return NGX_ERROR;
    }

    if (ngx_fd_info(fd, &fi) == NGX_FILE_ERROR) {
        ngx_log_error(NGX_LOG_CRIT, log, ngx_errno,
                      ngx_fd_info_n " \"%V\" failed", name);

        if (ngx_close_file(fd) == NGX_FILE_ERROR) {
            ngx_log_error(NGX_LOG_ALERT, log, ngx_errno,
                          ngx_close_file_n " \"%V\" failed", name);
        }

        of->fd = NGX_INVALID_FILE;

        return NGX_ERROR;
    }

    if (ngx_is_dir(&fi)) {
        if (ngx_close_file(fd) == NGX_FILE_ERROR) {
            ngx_log_error(NGX_LOG_ALERT, log, ngx_errno,
                          ngx_close_file_n " \"%V\" failed", name);
        }

        of->fd = NGX_INVALID_FILE;

    } else {
        of->fd = fd;

        if (of->read_ahead && ngx_file_size(&fi) > NGX_MIN_READ_AHEAD) {
            if (ngx_read_ahead(fd, of->read_ahead) == NGX_ERROR) {
                ngx_log_error(NGX_LOG_ALERT, log, ngx_errno,
                              ngx_read_ahead_n " \"%V\" failed", name);
            }
        }

        if (of->directio <= ngx_file_size(&fi)) {
            if (ngx_directio_on(fd) == NGX_FILE_ERROR) {
                ngx_log_error(NGX_LOG_ALERT, log, ngx_errno,
                              ngx_directio_on_n " \"%V\" failed", name);

            } else {
                of->is_directio = 1;
            }
        }
    }

done:

    of->uniq = ngx_file_uniq(&fi);
    of->mtime = ngx_file_mtime(&fi);
    of->size = ngx_file_size(&fi);
    of->fs_size = ngx_file_fs_size(&fi);
    of->is_dir = ngx_is_dir(&fi);
    of->is_file = ngx_is_file(&fi);
    of->is_link = ngx_is_link(&fi);
    of->is_exec = ngx_is_exec(&fi);

    return NGX_OK;
}

///////////////////////////////////////////////////////////////////////
//	end code copied from ngx_open_file_cache

typedef struct {
	ngx_str_t name;
	ngx_open_file_info_t *of;
	ngx_async_open_file_callback_t callback;
	void* context;
	ngx_log_t* log;
	ngx_pool_cleanup_t *cln;
	ngx_int_t err;
} ngx_async_open_file_ctx_t;
	
static void
ngx_thread_open_handler(void *data, ngx_log_t *log)
{
	ngx_async_open_file_ctx_t* ctx = data;
	
	ctx->err = ngx_open_and_stat_file(&ctx->name, ctx->of, log);
}

static void
ngx_async_open_thread_event_handler(ngx_event_t *ev)
{
	ngx_pool_cleanup_file_t *clnf;
	ngx_async_open_file_ctx_t* ctx;
	
	ctx = ev->data;

	// set up the cleanup of the file
	if (ctx->err == NGX_OK && !ctx->of->is_dir) {
		ctx->cln->handler = ngx_pool_cleanup_file;
		clnf = ctx->cln->data;

		clnf->fd = ctx->of->fd;
		clnf->name = ctx->name.data;
		clnf->log = ctx->log;
	}

	// notify the caller
	ctx->callback(ctx->context, ctx->err);
}

ngx_int_t 
ngx_async_open_file(
	ngx_pool_t* pool, 
	ngx_thread_pool_t *tp, 
	ngx_thread_task_t **taskp, 
	ngx_str_t *name, 
	ngx_open_file_info_t *of, 
	ngx_async_open_file_callback_t callback, 
	void* context)
{
	ngx_async_open_file_ctx_t* ctx;
	ngx_thread_task_t *task;
	
	// allocate the task if needed
	task = *taskp;
	
	if (task == NULL) {
        task = ngx_thread_task_alloc(pool, sizeof(ngx_async_open_file_ctx_t));
        if (task == NULL) 
		{
			ngx_log_debug0(NGX_LOG_DEBUG_HTTP, pool->log, 0,
				"ngx_async_open_file: ngx_thread_task_alloc failed");
			return NGX_ERROR;
        }

		task->handler = ngx_thread_open_handler;

        *taskp = task;
	}

	// initialize the context
    ctx = task->ctx;
	ctx->name = *name;
	ctx->of = of;
	ctx->callback = callback;
	ctx->context = context;
	ctx->log = pool->log;

	// allocate a cleanup item for the file
	ctx->cln = ngx_pool_cleanup_add(pool, sizeof(ngx_pool_cleanup_file_t));
	if (ctx->cln == NULL) 
	{
		ngx_log_debug0(NGX_LOG_DEBUG_HTTP, pool->log, 0,
			"ngx_async_open_file: ngx_pool_cleanup_add failed");
		return NGX_ERROR;
	}

	// post the task
    task->event.data = ctx;
    task->event.handler = ngx_async_open_thread_event_handler;

    if (ngx_thread_task_post(tp, task) != NGX_OK) 
	{
		ngx_log_debug0(NGX_LOG_DEBUG_HTTP, pool->log, 0,
			"ngx_async_open_file: ngx_thread_task_post failed");
		return NGX_ERROR;
    }

    return NGX_OK;
}

#endif // NGX_THREADS
