#pragma once

#include <sys/types.h>

/**
 * User-defined in-memory filesystem. It is as simple as possible.
 * Each file lies in the memory as an array of blocks. A file
 * has an unique file name, and there are no directories, so the
 * FS is a monolithic flat contiguous folder.
 */

/**
 * Here you should specify which features do you want to implement
 * via macros: NEED_OPEN_FLAGS and NEED_RESIZE. If you want to
 * allow advanced flags, do this here:
 *
 *     #define NEED_OPEN_FLAGS 1
 *
 * To allow resize() functions define this:
 *
 *     #define NEED_RESIZE 1
 *
 * It is important to define these macros here, in the header,
 * because it is used by tests.
 */
#define NEED_OPEN_FLAGS 1
#define NEED_RESIZE 1

/**
 * Flags for ufs_open call.
 */
enum open_flags {
	/**
	 * If the flag specified and a file does not exist -
	 * create it.
	 */
	UFS_CREATE = 0b0001,

#if NEED_OPEN_FLAGS
	/**
	 * With this flag it is allowed to only read the file.
	 */
	UFS_READ_ONLY = 0b0010,
	/**
	 * With this flag it is allowed to only write into the
	 * file.
	 */
	UFS_WRITE_ONLY = 0b0100,
	/**
	 * With this flag it is allowed to both read and write
	 * into the file.
	 */
	UFS_READ_WRITE = UFS_READ_ONLY | UFS_WRITE_ONLY,
#endif
};

/** Possible errors from all functions. */
enum ufs_error_code {
	UFS_ERR_NO_ERR = 0,
	UFS_ERR_NO_FILE,
	UFS_ERR_NO_MEM,
	UFS_ERR_NOT_IMPLEMENTED,

#if NEED_OPEN_FLAGS
	UFS_ERR_NO_PERMISSION,
#endif
};

/** Get code of the last error. */
ufs_error_code
ufs_errno();

/**
 * Open a file by filename.
 * @param filename Name of a file to open.
 * @param flags Bitwise combination of open_flags.
 *
 * @retval > 0 File descriptor.
 * @retval -1 Error occurred. Check ufs_errno() for a code.
 *     - UFS_ERR_NO_FILE - no such file, and UFS_CREATE flag is
 *       not specified.
 */
int
ufs_open(const char *filename, int flags);

/**
 * Write data to the file.
 * @param fd File descriptor from ufs_open().
 * @param buf Buffer to write.
 * @param size Size of @a buf.
 *
 * @retval > 0 How many bytes were written.
 * @retval -1 Error occurred. Check ufs_errno() for a code.
 *     - UFS_ERR_NO_FILE - invalid file descriptor.
 *     - UFS_ERR_NO_MEM - not enough memory.
 */
ssize_t
ufs_write(int fd, const char *buf, size_t size);

/**
 * Read data from the file.
 * @param fd File descriptor from ufs_open().
 * @param buf Buffer to read into.
 * @param size Maximum bytes to read.
 *
 * @retval > 0 How many bytes were read.
 * @retval 0 EOF.
 * @retval -1 Error occurred. Check ufs_errno() for a code.
 *     - UFS_ERR_NO_FILE - invalid file descriptor.
 */
ssize_t
ufs_read(int fd, char *buf, size_t size);

/**
 * Close a file.
 * @param fd File descriptor from ufs_open().
 * @retval 0 Success.
 * @retval -1 Error occurred. Check ufs_errno() for a code.
 *     - UFS_ERR_NO_FILE - invalid file descriptor.
 */
int
ufs_close(int fd);

/**
 * Delete a file by its name. Note, that it is allowed to drop the
 * file even if there are opened descriptors. In such a case the
 * file content will live until the last descriptor is closed. If
 * the file is deleted, it is allowed to create a new one with the
 * same name immediately and it should not affect existing opened
 * descriptors of the deleted file.
 *
 * @param filename Name of a file to delete.
 * @retval -1 Error occurred. Check ufs_errno() for a code.
 *     - UFS_ERR_NO_FILE - no such file.
 */
int
ufs_delete(const char *filename);

#if NEED_RESIZE

/**
 * Resize a file opened by the file descriptor @a fd. If current
 * file size is less than @a new_size, then new empty blocks are
 * created and positions of opened file descriptors are not
 * changed. If the current size is bigger than @a new_size, then
 * the blocks are truncated. Opened file descriptors behind the
 * new file size should proceed from the new file end.
 *
 * @param fd File descriptor from ufs_open().
 * @param new_size New file size.
 * @retval 0 Success.
 * @retval -1 Error occurred.
 *     - UFS_ERR_NO_FILE - invalid file descriptor.
 *     - UFS_ERR_NO_PERMISSION - descriptor should have been opened with
 *       UFS_WRITE_ONLY or UFS_READ_WRITE permissions.
 *     - UFS_ERR_NO_MEM - not enough memory. Can appear only when
 *       @a new_size is bigger than the current size.
 */
int
ufs_resize(int fd, size_t new_size);

#endif

/**
 * Destroy all the global variables, free all the memory, close and delete all
 * the files. After the destruction neither of the ufs functions are supposed to
 * be used. Purpose of the destruction is to reclaim all the dynamic memory.
 */
void
ufs_destroy(void);
