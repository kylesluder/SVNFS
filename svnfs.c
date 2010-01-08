/*
 * svnfs.c
 * SVNFS File System
 *
 * Kyle Sluder
 */

#include "svnfs.h"

#include <stddef.h>
#include <stdlib.h>

#define FUSE_USE_VERSION 25
#include <fuse.h>
#include <fuse_opt.h>

#include <apr_general.h>
#include <apr_strings.h>
#include <apr_thread_rwlock.h>

#include <svn_types.h>
#include <svn_auth.h>
#include <svn_ra.h>

/* STATIC GLOBALS {{{1 */

/*
 * svnfs_repository
 *
 * The repository URL as provided in the parameters.
 *
 * IMPORTANT: No function (other than main()) may modify this variable, as it is
 * not thread-safe.
 */
static char *svnfs_repository;

/*
 * svnfs_mountpoint
 *
 * The mount point of the filesystem, as provided in the parameters.
 *
 * IMPORTANT: No function (other than main()) may modify this variable, as it is
 * not thread-safe.
 */
static char *svnfs_mountpoint;

/*
 * svnfs_pool
 *
 * The APR pool used to dynamically allocate memory.
 */
static apr_pool_t *pool;

/*
 * svnfs_ra_session
 *
 * Subversion repository access session.
 */
svn_ra_session_t *svnfs_ra_session;

/* 
 * svnfs_ra_session_lock
 *
 * Lock used to prevent other operations during read.  "Writers" are actually
 * read() operations, which require mutually exclusive access to the
 * repository; any other operation is a "reader".
 */
apr_thread_rwlock_t *svnfs_ra_session_lock;

/* 
 * svnfs_cache_files
 *
 * An apr_hash_t that maps paths in the filesystems to temporary files on disk
 * that contain the contents of the represented files.  For example, the path
 * /1/foo might map to "/tmp/svnfs.7jalg2G", a file which contains the contents
 * of the repository file "/foo" at revision 1.
 */
static apr_hash_t *svnfs_cache_files;

/*
 * svnfs_opts
 *
 * Flags and parameters passable to SVNFS on the command line.  There are no
 * named parameters as yet, but the first unnamed parameter must be the URL of
 * the repository, and the second must be the mountpoint.
 */
#define SVNFS_OPT(t, o, v) { t, offsetof(struct svnfs_context_t, o), v }
static struct fuse_opt svnfs_opts[] = 
{
	FUSE_OPT_END
};

/*
 * svnfs_fuse_operations
 *
 * The FUSE operations SVNFS implements.
 */
static struct fuse_operations svnfs_fuse_operations =
{
	.getattr = svnfs_fuse_getattr,
	.read    = svnfs_fuse_read,
	.open    = svnfs_fuse_open,
	.readdir = svnfs_fuse_readdir
};

/* }}} END STATIC GLOBALS */

/* FUSE OPERATIONS {{{1 */

int svnfs_fuse_getattr(const char *path, struct stat *stbuf)
{
	svn_revnum_t rev;
	char *repos_path;
	svn_dirent_t *dirent;
	svn_error_t *err;

	memset(stbuf, 0, sizeof(struct stat));
	if(strcmp(path, "/") == 0)
	{
		stbuf->st_mode = S_IFDIR | 0755;
		return 0;
	}

	if(!svnfs_path_split(path, &rev, &repos_path))
		return -ENOENT;

	SVNFS_LOCK_READ;
		printf("Attempting to stat '%s@@%ld'\n", repos_path, rev);
		err = svn_ra_stat(svnfs_ra_session, repos_path, rev, &dirent, pool);
	SVNFS_UNLOCK;

	if(err != SVN_NO_ERROR)
	{
		svn_handle_error2(err, stderr, FALSE, "svnfs: ");
		svn_error_clear(err);
		return -EPIPE;
	}

	stbuf->st_size = dirent->size;
	switch(dirent->kind)
	{
		case svn_node_file:
			stbuf->st_mode = S_IFREG | 0644;
			break;
		case svn_node_dir:
			stbuf->st_mode = S_IFDIR | 0755;
			break;
		default:
			stbuf->st_mode = S_IFREG | 0000;
			break;
	}

	return 0;
}

int svnfs_fuse_open(const char *path, struct fuse_file_info *fi)
{
	char *cache_path;
	char *repos_path;
	svn_revnum_t rev;
	const char *temp_dir;
	apr_file_t *cache_file;
	svn_stream_t *cache_stream;
	svn_error_t *err;

	if(!svnfs_path_split(path, &rev, &repos_path))
	{
		printf("Attempted to open malformed path \"%s\"\n", path);
		return -ENOENT;
	}

	/* Verify that we have a cache of the data */
	cache_path = apr_hash_get(svnfs_cache_files, path, APR_HASH_KEY_STRING);
	if(!cache_path)
	{
		/* CACHE MISS */
		printf("Cache miss on path \"%s\"\n", path);

		if(apr_temp_dir_get(&temp_dir, pool) != APR_SUCCESS)
		{
			printf("Could not retrieve temporary directory\n");
			return -ENOMEM;
		}

		cache_path = apr_psprintf(pool, "%s/svnfs.XXXXXX", temp_dir);

		SVNFS_LOCK_WRITE;
			/* This stuff happens in a mutex because svn_ra_get_file is not
			 * thread-safe, and because we can't afford to have another
			 * thread take over between apr_file_mktemp and us actually
			 * writing the data. */

			/* We want our temporary file to persist after this read */
			if(apr_file_mktemp(&cache_file, cache_path, 
				               APR_CREATE | APR_EXCL | APR_WRITE, pool)
			   != APR_SUCCESS)
			{
				SVNFS_UNLOCK;
				printf("Could not create temp file\n");
				return -ENOMEM;
			}

			cache_stream = svn_stream_from_aprfile(cache_file, pool);

			err = svn_ra_get_file(svnfs_ra_session, repos_path, rev,
			                      cache_stream, NULL, NULL, pool);
			if(err != SVN_NO_ERROR)
			{
				SVNFS_UNLOCK;
				printf("Could not get %s@%ld\n", repos_path, rev);
				svn_error_clear(err);
				return -ENOENT;
			}

			err = svn_stream_close(cache_stream);
			if(err != SVN_NO_ERROR)
			{
				SVNFS_UNLOCK;
				printf("Could not close stream\n");
				svn_error_clear(err);
				return -EIO;
			}

			if(apr_file_close(cache_file) != APR_SUCCESS)
			{
				SVNFS_UNLOCK;
				printf("Could not close temp file\n");
				return -EIO;
			}

			apr_hash_set(svnfs_cache_files, path, APR_HASH_KEY_STRING,
			             cache_path);

		SVNFS_UNLOCK;
	}

	return 0;
}

int svnfs_fuse_read(const char *path, char *buf, size_t len, off_t offset,
                    struct fuse_file_info *fi)
{
	char *cache_path;
	apr_file_t *cache_file;
	apr_status_t read_err;
	apr_size_t bytes_read;
	apr_off_t my_offset;

	cache_path = apr_hash_get(svnfs_cache_files, path, APR_HASH_KEY_STRING);
	if(!cache_path)
	{
		printf("Could not find cache for \"%s\"\n", path);
		return -EIO;
	}

	if(apr_file_open(&cache_file, cache_path, APR_READ, APR_OS_DEFAULT, pool)
	   != APR_SUCCESS)
	{
		printf("Could not open temporary file \"%s\"\n", cache_path);
		return -EIO;
	}

	my_offset = offset;
	if(apr_file_seek(cache_file, APR_SET, &my_offset) != APR_SUCCESS)
	{
		apr_file_close(cache_file);
		printf("Could not seek to requested location\n");
		return -EIO;
	}

	read_err = apr_file_read_full(cache_file, buf, len, &bytes_read);
	if(read_err != APR_SUCCESS && read_err != APR_EOF)
	{
		apr_file_close(cache_file);
		printf("Failed to read from cache file\n");
		return -EIO;
	}

	if(apr_file_close(cache_file) != APR_SUCCESS)
	{
		printf("Failed to close cache file\n");
		return -EIO;
	}

	return bytes_read;
}

int svnfs_fuse_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                       off_t offset, struct fuse_file_info *fi)
{
	svn_error_t *err;
	svn_revnum_t rev;
	char *repos_path;

	apr_hash_t *dirents;
	apr_hash_index_t *iter;
	char *cur_dir;

	if(strcmp(path, "/") == 0)
	{
		SVNFS_LOCK_READ;
			err = svn_ra_get_latest_revnum(svnfs_ra_session, &rev, pool);
		SVNFS_UNLOCK;

		if(err != SVN_NO_ERROR)
		{
			svn_handle_error2(err, stderr, FALSE, "svnfs: ");
			svn_error_clear(err);
			return -EPIPE;
		}

		filler(buf, ".", NULL, 0);
		filler(buf, "..", NULL, 0);

		while(rev > 0)
			filler(buf, apr_itoa(pool, rev--), NULL, 0);

		return 0;
	}

	if(!svnfs_path_split(path, &rev, &repos_path))
		return -ENOENT; /* Invalid path */

	filler(buf, ".", NULL, 0);
	filler(buf, "..", NULL, 0);

	SVNFS_LOCK_READ;
		printf("Attempting to get '%s@@%ld'...\n", repos_path, rev);
		err = svn_ra_get_dir(svnfs_ra_session, repos_path, rev, &dirents, NULL,
	                         NULL, pool);
	SVNFS_UNLOCK;

	if(err != SVN_NO_ERROR)
	{
		svn_handle_error2(err, stderr, FALSE, "svnfs: ");
		svn_error_clear(err);
		return -EPIPE;
	}

	for(iter = apr_hash_first(pool, dirents); iter;
	    iter = apr_hash_next(iter))
	{
		apr_hash_this(iter, (const void **)(&cur_dir), NULL, NULL);
		filler(buf, cur_dir, NULL, 0);
	}

	return 0;
}

/* }}}1 END FUSE OPERATIONS */

/* HELPER OPERATIONS {{{1 */

int svnfs_path_split(const char *path, svn_revnum_t *rev,
                           char **repos_path)
{
	printf("Splitting directory %s\n", path);

	/* Verify we have a path */
	if(path[0] != '/')
	{
		printf("First character not /\n");
		return 0;
	}

	/* Special case: root */
	if(path[1] == '\0')
	{
		printf("Cannot split root directory\n");
		return 0;
	}

	*rev = strtol(path + 1, repos_path, 10);
	if(*rev == 0) /* See if we had an error */
	{
		if(*repos_path == path + 1) /* No revision specified */
		{
			printf("No revision specified\n");
			return 0;
		}
		else if (*repos_path[0] != '/') /* Invalid character in revision */
		{
			printf("Invalid character in revision\n");
			return 0;
		}
	}

	if(*repos_path[0] == '\0') /* Path specified revision root */
		*repos_path = "/";

	return 1;
}

/* END HELPER OPERATIONS }}}1 */

/* MAIN OPERATIONS {{{1 */

/*
 * svnfs_opt_proc
 *
 * Parses parameters one at a time.  Since there are currently no flags
 * available, this function simply tries to fill svnfs_ctx->repository and
 * svnfs_ctx->mount_point.
 *
 * data:    user data provided by caller
 * arg:     the argument being processed
 * key:     the key (which pattern was matched)
 * outargs: the actual set of arguments being iterated
 * return:  -1 on error, 0 to delete arg, 1 to keep arg
 */
static int svnfs_opt_proc(void *data, const char *arg, int key,
                   struct fuse_args *outargs)
{
	switch(key)
	{
		case FUSE_OPT_KEY_NONOPT:
			if(!svnfs_repository)
			{
				svnfs_repository = apr_pstrdup(pool, arg);
				return 0;
			}
			else if(!svnfs_mountpoint)
			{
				svnfs_mountpoint = apr_pstrdup(pool, arg);
				return 1;
			}
			else
			{
				return -1;
			}
			break;
		default:
			return 1;
	}
}

/*
 * svnfs_svn_init
 *
 * Initializes the Subversion library.
 *
 * return: SVN_NO_ERROR on success, or an svn_error_t* on failure.
 */
svn_error_t *svnfs_svn_init(void)
{
	apr_array_header_t *auth_objs;
	svn_ra_callbacks2_t *callbacks;
	svn_auth_provider_object_t *simple_provider;

	SVN_ERR(svn_ra_create_callbacks(&callbacks, pool));

	/* Auth stuff */
	auth_objs = apr_array_make(pool, 0, 0);
	*(void **)apr_array_push(auth_objs) = simple_provider;
	svn_auth_open(&callbacks->auth_baton, auth_objs, pool);

	/* Open the connection */
	SVN_ERR(svn_ra_open2(&svnfs_ra_session, svnfs_repository, callbacks, NULL,
	        NULL, pool));

	return SVN_NO_ERROR;
}

int main(int argc, char **argv)
{
	struct fuse_args args;
	int phony_argc;

	/* We're going to cheat APR here.  Since we do all argument processing
	 * ourselves, we'll just pass off a phony, short version of argc/argv. */
	phony_argc = 1;
	if(apr_app_initialize(&phony_argc, (char const *const **)argv, NULL)
		!= APR_SUCCESS)
		abort();

	if(atexit(apr_terminate) != 0)
		abort();

	if(apr_pool_create(&pool, NULL) != APR_SUCCESS)
		abort();

	args.argc      = argc;
	args.argv      = argv;
	args.allocated = 0;
	svnfs_repository = NULL;
	svnfs_mountpoint = NULL;
	if(fuse_opt_parse(&args, NULL, svnfs_opts, svnfs_opt_proc) != 0)
		return EXIT_FAILURE;

	if(!svnfs_repository || !svnfs_mountpoint)
		return EXIT_FAILURE;

	if(svnfs_svn_init() != SVN_NO_ERROR)
		return EXIT_FAILURE;

	if(apr_thread_rwlock_create(&svnfs_ra_session_lock, pool) != APR_SUCCESS)
		return EXIT_FAILURE;
	
	svnfs_cache_files = apr_hash_make(pool);

	return fuse_main(args.argc, args.argv, &svnfs_fuse_operations);
}

/* }}} END MAIN OPERATIONS */

/* vim: set tw=80 ts=4 fdm=marker: */
