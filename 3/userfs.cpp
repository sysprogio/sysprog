#include "userfs.h"

#include "rlist.h"

#include <algorithm>
#include <stddef.h>
#include <string.h>
#include <string>
#include <vector>

enum {
	BLOCK_SIZE = 512,
	MAX_FILE_SIZE = 1024 * 1024 * 100,
};

/** Global error code. Set from any function on any error. */
static ufs_error_code ufs_error_code = UFS_ERR_NO_ERR;

struct block {
	/** Block memory. */
	char memory[BLOCK_SIZE];
	/** A link in the block list of the owner-file. */
	rlist in_block_list = RLIST_LINK_INITIALIZER;
};

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
	size_t size = 0;
	bool deleted = false;
	std::vector<block*> block_index;
};

/**
 * Intrusive list of all files. In this case the intrusiveness of the list also
 * grants the ability to remove items from any position in O(1) complexity
 * without having to know their iterator.
 */
static rlist file_list = RLIST_HEAD_INITIALIZER(file_list);

struct filedesc {
	file *atfile;
	size_t pos = 0;
#if NEED_OPEN_FLAGS
	int flags = 0;
#endif
};

/**
 * An array of file descriptors. When a file descriptor is
 * created, its pointer drops here. When a file descriptor is
 * closed, its place in this array is set to NULL and can be
 * taken by next ufs_open() call.
 */
static std::vector<filedesc*> file_descriptors;

enum ufs_error_code
ufs_errno()
{
	return ufs_error_code;
}

static file *
find_file(const char *filename)
{
	file *f;
	rlist_foreach_entry(f, &file_list, in_file_list) {
		if (!f->deleted && f->name == filename)
			return f;
	}
	return NULL;
}

static block *
block_at(file *f, size_t index)
{
	if (index >= f->block_index.size())
		return NULL;
	return f->block_index[index];
}

static block *
ensure_block(file *f, size_t index)
{
	size_t count = f->block_index.size();
	while (count <= index) {
		block *b = new block();
		memset(b->memory, 0, sizeof(b->memory));
		rlist_add_tail_entry(&f->blocks, b, in_block_list);
		f->block_index.push_back(b);
		++count;
	}
	return block_at(f, index);
}

static void
free_blocks(file *f)
{
	for (size_t i = 0; i < f->block_index.size(); ++i) {
		block *b = f->block_index[i];
		rlist_del_entry(b, in_block_list);
		delete b;
	}
	f->block_index.clear();
}

static bool
can_read(const filedesc *d)
{
#if NEED_OPEN_FLAGS
	return (d->flags & UFS_READ_ONLY) != 0;
#else
	(void)d;
	return true;
#endif
}

static bool
can_write(const filedesc *d)
{
#if NEED_OPEN_FLAGS
	return (d->flags & UFS_WRITE_ONLY) != 0;
#else
	(void)d;
	return true;
#endif
}

static int
alloc_fd(filedesc *desc)
{
	for (size_t i = 0; i < file_descriptors.size(); ++i) {
		if (file_descriptors[i] == NULL) {
			file_descriptors[i] = desc;
			return (int)i + 1;
		}
	}
	file_descriptors.push_back(desc);
	return (int)file_descriptors.size();
}

int
ufs_open(const char *filename, int flags)
{
	file *f = find_file(filename);
	if (f == NULL) {
		if ((flags & UFS_CREATE) == 0) {
			ufs_error_code = UFS_ERR_NO_FILE;
			return -1;
		}
		f = new file();
		f->name = filename;
		rlist_add_tail_entry(&file_list, f, in_file_list);
	}

	filedesc *desc = new filedesc();
	desc->atfile = f;
	desc->pos = 0;
#if NEED_OPEN_FLAGS
	int rw = flags & (UFS_READ_ONLY | UFS_WRITE_ONLY);
	if (rw == 0)
		rw = UFS_READ_WRITE;
	desc->flags = rw;
#endif
	++f->refs;
	ufs_error_code = UFS_ERR_NO_ERR;
	return alloc_fd(desc);
}

ssize_t
ufs_write(int fd, const char *buf, size_t size)
{
	if (fd <= 0 || (size_t)fd > file_descriptors.size() ||
	    file_descriptors[fd - 1] == NULL) {
		ufs_error_code = UFS_ERR_NO_FILE;
		return -1;
	}
	filedesc *desc = file_descriptors[fd - 1];
	if (!can_write(desc)) {
		ufs_error_code = UFS_ERR_NO_PERMISSION;
		return -1;
	}
	if (size == 0) {
		ufs_error_code = UFS_ERR_NO_ERR;
		return 0;
	}
	file *f = desc->atfile;
	if (desc->pos + size > MAX_FILE_SIZE) {
		ufs_error_code = UFS_ERR_NO_MEM;
		return -1;
	}
	size_t end_pos = desc->pos + size;
	size_t last_index = (end_pos - 1) / BLOCK_SIZE;
	if (ensure_block(f, last_index) == NULL) {
		ufs_error_code = UFS_ERR_NO_MEM;
		return -1;
	}

	size_t written = 0;
	while (written < size) {
		size_t pos = desc->pos;
		size_t idx = pos / BLOCK_SIZE;
		size_t off = pos % BLOCK_SIZE;
		block *b = block_at(f, idx);
		size_t chunk = std::min(size - written, BLOCK_SIZE - off);
		memcpy(b->memory + off, buf + written, chunk);
		desc->pos += chunk;
		written += chunk;
	}
	if (end_pos > f->size)
		f->size = end_pos;
	ufs_error_code = UFS_ERR_NO_ERR;
	return (ssize_t)written;
}

ssize_t
ufs_read(int fd, char *buf, size_t size)
{
	if (fd <= 0 || (size_t)fd > file_descriptors.size() ||
	    file_descriptors[fd - 1] == NULL) {
		ufs_error_code = UFS_ERR_NO_FILE;
		return -1;
	}
	filedesc *desc = file_descriptors[fd - 1];
	if (!can_read(desc)) {
		ufs_error_code = UFS_ERR_NO_PERMISSION;
		return -1;
	}
	if (size == 0) {
		ufs_error_code = UFS_ERR_NO_ERR;
		return 0;
	}
	file *f = desc->atfile;
	if (desc->pos >= f->size) {
		ufs_error_code = UFS_ERR_NO_ERR;
		return 0;
	}
	size_t to_read = std::min(size, f->size - desc->pos);
	size_t read = 0;
	while (read < to_read) {
		size_t pos = desc->pos;
		size_t idx = pos / BLOCK_SIZE;
		size_t off = pos % BLOCK_SIZE;
		block *b = block_at(f, idx);
		size_t chunk = std::min(to_read - read, BLOCK_SIZE - off);
		memcpy(buf + read, b->memory + off, chunk);
		desc->pos += chunk;
		read += chunk;
	}
	ufs_error_code = UFS_ERR_NO_ERR;
	return (ssize_t)read;
}

int
ufs_close(int fd)
{
	if (fd <= 0 || (size_t)fd > file_descriptors.size() ||
	    file_descriptors[fd - 1] == NULL) {
		ufs_error_code = UFS_ERR_NO_FILE;
		return -1;
	}
	filedesc *desc = file_descriptors[fd - 1];
	file_descriptors[fd - 1] = NULL;
	file *f = desc->atfile;
	delete desc;
	--f->refs;
	if (f->deleted && f->refs == 0) {
		rlist_del_entry(f, in_file_list);
		free_blocks(f);
		delete f;
	}
	ufs_error_code = UFS_ERR_NO_ERR;
	return 0;
}

int
ufs_delete(const char *filename)
{
	file *f = find_file(filename);
	if (f == NULL) {
		ufs_error_code = UFS_ERR_NO_FILE;
		return -1;
	}
	f->deleted = true;
	if (f->refs == 0) {
		rlist_del_entry(f, in_file_list);
		free_blocks(f);
		delete f;
	}
	ufs_error_code = UFS_ERR_NO_ERR;
	return 0;
}

#if NEED_RESIZE

int
ufs_resize(int fd, size_t new_size)
{
	if (fd <= 0 || (size_t)fd > file_descriptors.size() ||
	    file_descriptors[fd - 1] == NULL) {
		ufs_error_code = UFS_ERR_NO_FILE;
		return -1;
	}
	filedesc *desc = file_descriptors[fd - 1];
	if (!can_write(desc)) {
		ufs_error_code = UFS_ERR_NO_PERMISSION;
		return -1;
	}
	if (new_size > MAX_FILE_SIZE) {
		ufs_error_code = UFS_ERR_NO_MEM;
		return -1;
	}
	file *f = desc->atfile;
	if (new_size == f->size) {
		ufs_error_code = UFS_ERR_NO_ERR;
		return 0;
	}
	if (new_size < f->size) {
		size_t keep_blocks = (new_size + BLOCK_SIZE - 1) / BLOCK_SIZE;
		for (size_t i = keep_blocks; i < f->block_index.size(); ++i) {
			block *b = f->block_index[i];
			rlist_del_entry(b, in_block_list);
			delete b;
		}
		f->block_index.resize(keep_blocks);
		f->size = new_size;
		for (size_t i = 0; i < file_descriptors.size(); ++i) {
			filedesc *d = file_descriptors[i];
			if (d != NULL && d->atfile == f && d->pos > f->size)
				d->pos = f->size;
		}
		ufs_error_code = UFS_ERR_NO_ERR;
		return 0;
	}
	if (new_size > f->size) {
		size_t last_index = (new_size - 1) / BLOCK_SIZE;
		if (ensure_block(f, last_index) == NULL) {
			ufs_error_code = UFS_ERR_NO_MEM;
			return -1;
		}
		f->size = new_size;
	}
	ufs_error_code = UFS_ERR_NO_ERR;
	return 0;
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
	for (size_t i = 0; i < file_descriptors.size(); ++i) {
		if (file_descriptors[i] == NULL)
			continue;
		file *f = file_descriptors[i]->atfile;
		delete file_descriptors[i];
		file_descriptors[i] = NULL;
		--f->refs;
		if (f->deleted && f->refs == 0) {
			rlist_del_entry(f, in_file_list);
			free_blocks(f);
			delete f;
		}
	}
	file *f, *tmp;
	rlist_foreach_entry_safe(f, &file_list, in_file_list, tmp) {
		rlist_del_entry(f, in_file_list);
		free_blocks(f);
		delete f;
	}
	std::vector<filedesc*> empty;
	file_descriptors.swap(empty);
}
