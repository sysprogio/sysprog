#include <cstdlib>
#include <cxxabi.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <map>
#include <new>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>

#include <mutex>

namespace
{
enum {
	MAX_BACKTRACE_LEN = 64,
	ALLOCATION_BATCH_SIZE = 1024,
};

enum report_mode {
	// Report always. Even when have no leaks.
	REPORT_MODE_VERBOSE,
	// Report only if there are leaks.
	REPORT_MODE_LEAKS,
	// Do not report anything.
	REPORT_MODE_QUIET,
};

enum backtrace_mode {
	BACKTRACE_ON,
	BACKTRACE_OFF,
};

enum content_mode {
	// Leave the resulting memory untouched like returned from the standard library.
	CONTENT_MODE_ORIGINAL,
	// Fill the new memory with trash bytes to easier catch bugs when people use not
	// initialized memory.
	CONTENT_MODE_TRASH,
};

//////////////////////////////////////////////////////////////////////////////////////////

// Single allocation done on the heap by a user.
struct allocation {
	void *trace[MAX_BACKTRACE_LEN];
	int trace_size;
	int depth;
	void *mem;
	size_t size;
	allocation *next;
};

// Allocation objects are created in batches via mmap(). They can't be easily allocated on
// the heap as that would cause recursion.
struct allocation_batch {
	allocation allocs[ALLOCATION_BATCH_SIZE];
	int used;
};

struct symbol {
	const char *file;
	const char *name;
};

//////////////////////////////////////////////////////////////////////////////////////////

static void
heaph_assert_do(bool flag, const char *expr, int line)
{
	if (flag)
		return;
	// Keep volatile errors on the stack. To see them in debugger.
	volatile int err = errno;
	volatile const char *err_msg = strerror(err);
	(void)err;
	(void)err_msg;
	printf("\n");
	printf("HH: assertion failure, line %d\n", line);
	printf("HH: %s\n", expr);
	_exit(-1);
}

#define heaph_assert(expr) heaph_assert_do((expr), #expr, __LINE__)

//////////////////////////////////////////////////////////////////////////////////////////

static int
trace_resolve(void *const *addrs, int count, struct symbol *syms)
{
	int rc = 0;
	Dl_info info;
	for (int i = 0; i < count; ++i) {
		struct symbol *s = &syms[i];
		if (dladdr(addrs[i], &info) == 0) {
			rc = -1;
			memset(s, 0, sizeof(*s));
			continue;
		}
		s->name = info.dli_sname;
		s->file = info.dli_fname;
	}
	return rc;
}

//////////////////////////////////////////////////////////////////////////////////////////

template<class T>
struct hh_allocator
{
	using value_type = T;

	T *
	allocate(size_t n)
	{
		T *res = (T *)std::malloc(n * sizeof(T));
		heaph_assert(res != nullptr);
		return res;
	}

	void
	deallocate(T *p, std::size_t)
	{
		std::free(p);
	}
};

using allocation_node = std::map<void *, allocation *>::value_type;
using allocation_map = std::map<
	void *,
	allocation *,
	std::less<>,
	hh_allocator<allocation_node>
>;

//////////////////////////////////////////////////////////////////////////////////////////

class heap_help
{
public:
	heap_help();
	~heap_help();

	void
	trace(void *ptr, size_t size);

	void
	untrace(void *ptr);

private:
	std::mutex m_mutex;
	allocation_map m_allocations;
	// Unused allocation objects. For re-use.
	allocation *m_alloc_pool;
	// Freshly created allocation objects. Taken from here when the pool is empty.
	allocation_batch *m_alloc_batch;
	uint64_t m_alloc_count;

	report_mode m_report_mode;
	content_mode m_content_mode;
	backtrace_mode m_backtrace_mode;
};

heap_help::heap_help()
	: m_alloc_pool(nullptr)
	, m_alloc_batch(nullptr)
	, m_alloc_count(0)
	, m_report_mode(REPORT_MODE_LEAKS)
	, m_content_mode(CONTENT_MODE_ORIGINAL)
	, m_backtrace_mode(BACKTRACE_ON)
{
	const char *hh_report = getenv("HHREPORT");
	if (hh_report != nullptr) {
		if (strcmp(hh_report, "v") == 0)
			m_report_mode = REPORT_MODE_VERBOSE;
		else if (strcmp(hh_report, "l") == 0)
			m_report_mode = REPORT_MODE_LEAKS;
		else if (strcmp(hh_report, "q") == 0)
			m_report_mode = REPORT_MODE_QUIET;
	}

	const char *hh_content = getenv("HHCONTENT");
	if (hh_content != nullptr) {
		if (strcmp(hh_content, "o") == 0)
			m_content_mode = CONTENT_MODE_ORIGINAL;
		else if (strcmp(hh_content, "t") == 0)
			m_content_mode = CONTENT_MODE_TRASH;
	}

	const char *bt_mode = getenv("HHBACKTRACE");
	if (bt_mode != nullptr) {
		if (strcmp(bt_mode, "on") == 0)
			m_backtrace_mode = BACKTRACE_ON;
		else if (strcmp(bt_mode, "off") == 0)
			m_backtrace_mode = BACKTRACE_OFF;
	}
}

heap_help::~heap_help()
{
	if (m_report_mode == REPORT_MODE_QUIET)
		return;
	m_mutex.lock();
	if (m_allocations.empty())
	{
		uint64_t count = m_alloc_count;
		m_mutex.unlock();

		if (m_report_mode == REPORT_MODE_VERBOSE) {
			printf("\n");
			printf("HH: found no leaks\n");
			printf("HH: total allocation count - %llu\n",
			       (long long)count);
		}
		return;
	}
	const int report_limit = 10;
	uint64_t report_count = 0;
	uint64_t leak_size = 0;
	symbol syms[MAX_BACKTRACE_LEN];
	// People often do not write '\n' in the end of their program. That makes it
	// harder to read HH output unless the latter prepends itself with a line wrap.
	const char *prefix = "\n";
	char *demangled_name = nullptr;
	size_t demangled_size = 0;
	for (const auto& [ptr, a] : m_allocations) {
		leak_size += a->size;
		if (report_count >= report_limit)
			continue;
		bool has_trace = trace_resolve(a->trace, a->trace_size, syms) == 0;
		printf("%s", prefix);
		prefix = "";
		printf("#### Leak %llu (%zu bytes) ####\n",
			(long long)++report_count, a->size);
		if (!has_trace) {
			printf("Couldn't get the trace\n");
			continue;
		}
		for (int i = 0; i < a->trace_size; ++i) {
			int status = 0;
			const char *original_name = syms[i].name;
			const char *name = abi::__cxa_demangle(
				original_name, demangled_name, &demangled_size, &status);
			if (name == nullptr)
				name = original_name;
			printf("%d - %s\n", i, name);
		}
	}
	std::free(demangled_name);
	printf("%s", prefix), prefix = "";
	printf("HH: found %lld leaks (%llu bytes)\n", (long long)m_allocations.size(),
		(long long)leak_size);
	if (report_count < m_allocations.size()) {
		printf("HH: only first %llu reports are shown\n",
			(long long)report_count);
	}
	printf("HH: total allocation count - %llu\n", (long long)m_alloc_count);
	m_mutex.unlock();
	_exit(-1);
}

void
heap_help::trace(void *ptr, size_t size)
{
	m_mutex.lock();
	allocation *a = m_alloc_pool;
	if (a != nullptr) {
		m_alloc_pool = a->next;
	} else {
		if (m_alloc_batch == nullptr ||
		    m_alloc_batch->used == ALLOCATION_BATCH_SIZE) {
			m_alloc_batch =
				(allocation_batch *)std::malloc(sizeof(*m_alloc_batch));
			heaph_assert(m_alloc_batch != nullptr);
			m_alloc_batch->used = 0;
		} else {
			heaph_assert(m_alloc_batch->used < ALLOCATION_BATCH_SIZE);
		}
		a = &m_alloc_batch->allocs[m_alloc_batch->used++];
	}
	a->mem = ptr;
	a->size = size;
	heaph_assert(m_allocations.emplace(ptr, a).second);
	++m_alloc_count;
	m_mutex.unlock();

	if (m_backtrace_mode == BACKTRACE_ON)
		a->trace_size = backtrace(a->trace, MAX_BACKTRACE_LEN);
	heaph_assert(a->trace_size >= 0);
}

void
heap_help::untrace(void *ptr)
{
	m_mutex.lock();
	auto it = m_allocations.find(ptr);
	if (it == m_allocations.end())
	{
		m_mutex.unlock();
		heaph_assert(! "Freeing unknown or already freed memory");
		return;
	}
	allocation *a = it->second;
	a->next = m_alloc_pool;
	m_alloc_pool = a;
	m_allocations.erase(it);
	m_mutex.unlock();
}

//////////////////////////////////////////////////////////////////////////////////////////

static heap_help glob_hh;
}

void *
operator new(std::size_t n)
{
	void *res = std::malloc(n);
	heaph_assert(res != nullptr);
	glob_hh.trace(res, n);
	return res;
}

void *
operator new(std::size_t count, std::align_val_t al)
{
	void *res = std::aligned_alloc((size_t)al, count);
	heaph_assert(res != nullptr);
	glob_hh.trace(res, count);
	return res;
}

void
operator delete(void *ptr) noexcept
{
	glob_hh.untrace(ptr);
	std::free(ptr);
}

void
operator delete(void *ptr, std::size_t) noexcept
{
	glob_hh.untrace(ptr);
	std::free(ptr);
}

void
operator delete(void *ptr, std::align_val_t) noexcept
{
	glob_hh.untrace(ptr);
	std::free(ptr);
}
