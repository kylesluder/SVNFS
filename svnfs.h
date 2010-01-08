/*
 * svnfs.h
 * SVNFS File System
 *
 * Kyle Sluder
 */

#ifndef _SVNFS_H_
#define _SVNFS_H_

#define FUSE_USE_VERSION 25

#include <sys/stat.h>
#include <svn_types.h>
#include <svn_string.h>
#include <fuse.h>

/* STRUCTURES {{{1 */

/* 
 * svnfs_cache_file_t
 *
 * A mapping between a filename, a revision, and the name of a temporary file
 * on disk used for caching purposes.
 */
typedef struct svnfs_cache_t
{
	/* Revision for which this file is cached */
	svn_revnum_t rev;

	/* Filename on disk of cached file */
	char *cache_path;
} svnfs_cache_t;

/* }}}1 END STRUCTURES */

/* HELPER OPERATIONS {{{1 */

/*
 * svnfs_path_split
 *
 * Splits a path of the form /{revision}/path... into a svn_revnum_t and a
 * char* path, which is allocated from pool.
 *
 * path:       the path to be split
 * rev:        pointer to svn_revnum_t to receive revision number, or
 *             SVN_IGNORED_REVNUM if the path is the root ("/")
 * repos_path: pointer to char* to receive repository path to file (will be
 *             allocated from the APR pool)
 * return:     nonzero on success, zero on failure
 */
int svnfs_path_split(const char *path, svn_revnum_t *rev,
                           char **repos_path);

/*
 * SVNFS_LOCK_READ
 *
 * Convenience macro for apr_thread_rwlock_rdlock.
 */
#define SVNFS_LOCK_READ do { \
	if(apr_thread_rwlock_rdlock(svnfs_ra_session_lock) != APR_SUCCESS) \
		return -EBUSY; \
} while(0)

/*
 * SVNFS_LOCK_WRITE
 *
 * Convenience macro for apr_thread_rwlock_wrlock.
 */
#define SVNFS_LOCK_WRITE do { \
	if(apr_thread_rwlock_wrlock(svnfs_ra_session_lock) != APR_SUCCESS) \
		return -EBUSY; \
} while(0)

/*
 * SVNFS_UNLOCK
 *
 * Convenience macro for apr_thread_rwlock_unlock.
 */
#define SVNFS_UNLOCK do { \
	if(apr_thread_rwlock_unlock(svnfs_ra_session_lock) != APR_SUCCESS) \
		return -EBUSY; \
} while(0)

/* }}}1 END HELPER OPERATIONS */

/* FUSE OPERATIONS {{{1 */

/*
 * svnfs_fuse_getattr
 *
 * Implements stat(2).
 *
 * path:   path to get attributes of
 * stbuf:  stat struct to fill
 * return: 0 on success, or -errno on error
 */
int svnfs_fuse_getattr(const char *path, struct stat *stbuf);

/*
 * svnfs_fuse_read
 *
 * Reads from a file.
 *
 * path:   path to file to read
 * buf:    buffer to fill
 * offset: offset within file to start reading from
 * fi:     information about the file
 * return: number of bytes requested (or EOF) on success, -errno on failure
 */
int svnfs_fuse_read(const char *path, char *buf, size_t len, off_t offset,
                    struct fuse_file_info *fi);

/*
 * svnfs_fuse_readdir
 *
 * Gets the contents of a directory.
 *
 * path:   path to directory to list
 * buf:    buffer to fill
 * filler: function used to fill buf
 * offset: offset within buf to start filling
 * fi:     information about the directory
 * return: 0 on success, -errno on failure
 */
int svnfs_fuse_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                       off_t offset, struct fuse_file_info *fi);

/*
 * svnfs_fuse_open
 *
 * Prepares a file for reading by retrieving that file's contents from the
 * repository into a temporary file.
 *
 * path: path of the file to be opened
 * return: 0 on success, -errno on failure
 */
int svnfs_fuse_open(const char *path, struct fuse_file_info *fi);

/* END FUSE OPERATIONS }}}1 */

#endif /* _SVNFS_H_ */

/* vim: set tw=80 ts=4 fdm=marker */
