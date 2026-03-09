#include "userfs.h"

#include "rlist.h"

#include <stddef.h>
#include <string>
#include <vector>
#include <assert.h>
#include <cstring>
#include <algorithm>

enum {
	BLOCK_SIZE = 512,
	MAX_FILE_SIZE = 1024 * 1024 * 100,
	MAX_FILE_BLOCKS_NUM = MAX_FILE_SIZE / BLOCK_SIZE,
};

/** Global error code. Set from any function on any error. */
static enum ufs_error_code ufs_error_code = UFS_ERR_NO_ERR;

struct block {
	/** Block memory. */
	char memory[BLOCK_SIZE];
	/** A link in the block list of the owner-file. */
	rlist in_block_list = RLIST_LINK_INITIALIZER;

	/* PUT HERE OTHER MEMBERS */
	size_t size = 0;
};

static size_t
block_write(struct block *b, size_t start, const char *buf, size_t size) 
{
	assert(BLOCK_SIZE != start);

	size_t to_write = std::min(size, BLOCK_SIZE - start);
	memcpy(b->memory + start, buf, to_write);
	b->size += size;

	return to_write;
}

static size_t
block_read(struct block *b, size_t start, char *buf, size_t size) 
{
	size_t to_read = std::min(size, b->size - start);
	memcpy(buf, b->memory + start, to_read);

	return to_read;
}

struct file {
	/**
	 * Doubly-linked intrusive list of file blocks. Intrusiveness of the
	 * list gives you the full control over the lifetime of the items in the
	 * list without having to use double pointers with performance penalty.
	 */
	rlist blocks = RLIST_HEAD_INITIALIZER(blocks);
	/** How many file descriptors are opened on the file. */
	int refs = 0;
	/** File name. */
	std::string name;
	/** A link in the global file list. */
	rlist in_file_list = RLIST_LINK_INITIALIZER;

	/* PUT HERE OTHER MEMBERS */
	bool unlinked = false;
	size_t block_num;
};

static struct block*
file_get_block(struct file *f, size_t block_ind)
{
	struct block *b = rlist_first_entry(&f->blocks, block, in_block_list);
	
	// Skip blocks.
	for (size_t i = 0; i < block_ind; i++) {
		b = rlist_next_entry(b, in_block_list);
		assert(b);
	}

	return b;
}

static ssize_t
file_read(struct file *f, size_t *position, char *buf, size_t size)
{
	size_t total_bytes_read = 0;
	assert(size != 0);

	size_t cur_block_ind = *position / BLOCK_SIZE;
	assert(cur_block_ind <= f->block_num);
	if (cur_block_ind == f->block_num) {
		// EOF.
		return 0;
	}

	struct block *b = file_get_block(f, cur_block_ind);
	size_t cur_block_postiton = *position % BLOCK_SIZE;
	if (b->size == cur_block_postiton) {
		// EOF.
		return 0;
	}

	for (;;) {
		size_t bytes_read = block_read(b, cur_block_postiton, buf, size);

		total_bytes_read += bytes_read;
		*position += bytes_read;

		// Check for eof.
		if (bytes_read < b->size - cur_block_postiton) {
			// Eof.
			return total_bytes_read;
		}

		if (size == 0) {
			// All read.
			return total_bytes_read;
		}

		b = rlist_next_entry(b, in_block_list);
		if (!b) {
			// No next block.
			return total_bytes_read;
		}

		buf += bytes_read;
		size -= bytes_read;

		cur_block_postiton = 0;
	}
}

static int
file_add_block(struct file *f) 
{
	if (f->block_num + 1 > MAX_FILE_BLOCKS_NUM) {
		return -1;
	}

	f->block_num++;
	struct block *b = new block;
	rlist_add_tail_entry(&f->blocks, b, in_block_list);

	return 0;
}

// Returns new position.
static size_t
file_write(struct file *f, size_t position, const char *buf, size_t size)
{
	assert(size != 0);

	size_t cur_block_ind = position / BLOCK_SIZE;

	assert(cur_block_ind <= f->block_num);
	if (cur_block_ind == f->block_num) {
		if (!file_add_block(f)) {
			return 0;
		}
	}

	struct block *b = file_get_block(f, cur_block_ind);

	size_t cur_block_postiton = position % BLOCK_SIZE;

	for (;;) {
		size_t written = block_write(b, cur_block_postiton, buf, size);
		
		buf += written;
		size -= written;
		position += written;

		if (size == 0) {
			// All written.
			return position;
		}

		if (!file_add_block(f)) {
			return 0;
		}
		b = rlist_next_entry(b, in_block_list);

		cur_block_postiton = 0;
	}
}

// postition = 513, size = 1023
// 0   1   2   3 .. 511 (skip)
// 512 513 .. 1023
// 1024

/**
 * Intrusive list of all files. In this case the intrusiveness of the list also
 * grants the ability to remove items from any position in O(1) complexity
 * without having to know their iterator.
 */
static rlist file_list = RLIST_HEAD_INITIALIZER(file_list);

static void 
file_init(struct file *f, const char *filename) 
{
	rlist_add_tail_entry(&file_list, f, in_file_list);
	f->name = std::string(filename);
}

static void
file_destroy(struct file* f) 
{
	assert(f);

	struct block *b;
	rlist_foreach_entry(b, &f->blocks, in_block_list) {
		delete b;
	}

	delete f;
}

static struct file*
file_find(const char *filename) 
{
	struct file *f;
	rlist_foreach_entry(f, &file_list, in_file_list) {
		if (f->name == filename && !f->unlinked) {
			return f;
		}
	}
	return NULL;
}

static void
file_try_remove(struct file *f) 
{
	if (f->refs == 0) {
		file_destroy(f);
		rlist_del_entry(f, in_file_list);
	}
}

static void 
file_acquire(struct file *f) 
{
	f->refs++;
}

static void 
file_release(struct file *f) 
{
	if (f->refs > 0) {
		f->refs--;
	}
}

static void
files_clear_all() 
{
	struct file *f;
	rlist_foreach_entry(f, &file_list, in_file_list) {
		file_destroy(f);
	}
	rlist_create(&file_list);
}

struct filedesc {
	file *atfile;
	/* PUT HERE OTHER MEMBERS */
	int flags;

	// Position in block.
	size_t pos;

};

// static void 
// filedesc_destroy(struct filedesc* fdesc) 
// {
// 	assert(fdesc);
// 	file_destroy(fdesc->atfile);
// 	delete fdesc->atfile;
// }

/**
 * An array of file descriptors. When a file descriptor is
 * created, its pointer drops here. When a file descriptor is
 * closed, its place in this array is set to NULL and can be
 * taken by next ufs_open() call.
 */
static std::vector<filedesc*> file_descriptors;

// static struct filedesc*
// filedescs_get_by_name(const char *filename) 
// {
// 	for (size_t i = 0; i < file_descriptors.size(); i++) {
// 		filedesc* fdesc = file_descriptors[i];
// 		if (!fdesc || fdesc->deleted) {
// 			continue;
// 		}

// 		if (fdesc->atfile->name == filename) {
// 			return fdesc;
// 		}
// 	}
// 	return NULL;
// }

static int
filedescs_new(struct file* const f, int flags)
{
	struct filedesc* fdesc = new filedesc;
	fdesc->atfile = f;
	fdesc->flags = flags;
	file_acquire(f);

	size_t i = 0;
	for (; i < file_descriptors.size(); i++) {
		if (file_descriptors[i]) {
			continue;
		}

		// fdesc->fd = i;
		file_descriptors[i] = fdesc;
		return i;
	}
	
	// fdesc->fd = i;
	file_descriptors.push_back(fdesc);

	return i;
}

static void
filedescs_clear() {
	for (size_t i = 0; i < file_descriptors.size(); i++) {
		delete file_descriptors[i];
	}
	std::vector<filedesc*>().swap(file_descriptors);
}

static int
filedescs_get_safe(int fd, struct filedesc** fdesc) {
	if (fd < 0 || static_cast<size_t>(fd) >= file_descriptors.size()) {
		return -1;
	}
	
	*fdesc = file_descriptors[fd];
	if (!(*fdesc)) {
		return -1;
	}

	return 0;
}

// static void
// filedescs_try_remove(int fd) 
// {
// 	struct filedesc* fdesc = file_descriptors[fd];
// 	assert(fdesc);

// 	if (file_is_released(fdesc->atfile)) {
// 		delete fdesc;
// 		file_descriptors[fd] = NULL;
// 	}
// }

enum ufs_error_code
ufs_errno()
{
	return ufs_error_code;
}

int
ufs_open(const char *filename, int flags)
{
	/* IMPLEMENT THIS FUNCTION */
	// (void)filename;
	// (void)flags;
	// (void)file_list;
	// (void)file_descriptors;
	// ufs_error_code = UFS_ERR_NOT_IMPLEMENTED;

	struct file *f = file_find(filename);
	if (f) {
		return filedescs_new(f, flags);
	}

	if (!(flags & open_flags::UFS_CREATE)) {
		ufs_error_code = UFS_ERR_NO_FILE;
		return -1;
	}

	// Create new file;
	f = new file;
	file_init(f, filename);

	return filedescs_new(f, flags);
}

ssize_t
ufs_write(int fd, const char *buf, size_t size)
{
	/* IMPLEMENT THIS FUNCTION */
	// (void)fd;
	// (void)buf;
	// (void)size;
	// ufs_error_code = UFS_ERR_NOT_IMPLEMENTED;
	struct filedesc* fdesc = NULL;
	if (filedescs_get_safe(fd, &fdesc) != 0) {
		ufs_error_code = UFS_ERR_NO_FILE;
		return -1;
	}

	if (size == 0) {
		return 0;
	}

	struct file *f = fdesc->atfile;
	size_t new_pos = file_write(f, fdesc->pos, buf, size);
	if (new_pos == 0) {
		ufs_error_code = UFS_ERR_NO_MEM;
		return -1;
	}
	fdesc->pos = new_pos;

	return size;
}

ssize_t
ufs_read(int fd, char *buf, size_t size)
{
	struct filedesc* fdesc = NULL;
	if (filedescs_get_safe(fd, &fdesc) != 0) {
		ufs_error_code = UFS_ERR_NO_FILE;
		return -1;
	}

	if (size == 0) {
		return 0;
	}

	struct file *f = fdesc->atfile;

	ssize_t bytes_read = file_read(f, &fdesc->pos, buf, size);
	return bytes_read;
}

int
ufs_close(int fd)
{
	/* IMPLEMENT THIS FUNCTION */
	// (void)fd;
	// ufs_error_code = UFS_ERR_NOT_IMPLEMENTED;

	struct filedesc* fdesc;
	if (filedescs_get_safe(fd, &fdesc) != 0) {
		ufs_error_code = UFS_ERR_NO_FILE;
		return -1;
	}

	struct file *f = fdesc->atfile;
	assert(f);

	file_release(f);
	if (f->unlinked) {
		file_try_remove(f);
	}

	delete fdesc;
	file_descriptors[fd] = NULL;

	return 0;
}

int
ufs_delete(const char *filename)
{
	/* IMPLEMENT THIS FUNCTION */
	// (void)filename;
	// ufs_error_code = UFS_ERR_NOT_IMPLEMENTED;

	struct file *f = file_find(filename);
	if (!f) {
		ufs_error_code = UFS_ERR_NO_FILE;
		return -1;
	}

	f->unlinked = true;
	file_try_remove(f);

	return 0;
}

#if NEED_RESIZE

int
ufs_resize(int fd, size_t new_size)
{
	/* IMPLEMENT THIS FUNCTION */
	(void)fd;
	(void)new_size;
	ufs_error_code = UFS_ERR_NOT_IMPLEMENTED;
	return -1;
}

#endif

void
ufs_destroy(void)
{
	/*
	 * The file_descriptors array is likely to leak even if
	 * you resize it to zero or call clear(). This is because
	 * the vector keeps memory reserved in case more elements
	 * would be added.
	 *
	 * The recommended way of freeing the memory is to swap()
	 * the vector with a temporary empty vector.
	 */

	filedescs_clear();
	files_clear_all();
}
