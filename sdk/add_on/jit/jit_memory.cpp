// Memory functions that keep the freed small blocks for reuse, see
// CJITCompiler::AllocMemory. They don't depend on the rest of the add-on

#include "jit.h"

#include <stdlib.h>

#ifndef AS_NO_THREADS
#include <atomic>
#include <thread>
#endif

BEGIN_AS_NAMESPACE

namespace
{

// The blocks of up to kMaxSize bytes are kept in lists by size, in steps of kStep
// bytes. They are cut from chunks allocated with malloc, which are never freed. Each
// block starts with a header that holds the index of its list, or 0 for the larger
// blocks, which are allocated with malloc. The header keeps the memory aligned like
// malloc does, and a free block holds the next one of its list after the header
const size_t kStep    = 16;
const size_t kLists   = 64;
const size_t kMaxSize = kStep * kLists;
const size_t kHeader  = 16;
const size_t kChunk   = 256 * 1024;

// Each thread keeps the blocks it frees in lists of its own, and takes the blocks for
// a list that is empty from the lists shared by the threads in batches of about
// kBatchBytes. When a list of the thread holds more than kCacheBytes it hands half of
// them back, so that the threads that free the blocks that others allocate don't
// hold on to them
const size_t kBatchBytes = 4 * 1024;
const size_t kMaxBatch   = 64;
const size_t kCacheBytes = 32 * 1024;

struct SList
{
	void  *head;
	size_t count;
};

inline void *&Next(void *block)
{
	return *reinterpret_cast<void**>(static_cast<char*>(block) + kHeader);
}

inline size_t &ListIndex(void *block)
{
	return *static_cast<size_t*>(block);
}

// The lists of a thread. The state is 0 before the thread has used them, 1 while it
// does, and 2 after the thread has ended, when its blocks go to the shared lists
struct SCache
{
	SList lists[kLists + 1];
	int   state;
};

// The lists shared by the threads, and the rest of the current chunk
SList  g_lists[kLists + 1];
char  *g_chunk    = 0;
char  *g_chunkEnd = 0;

#ifdef AS_NO_THREADS
SCache g_cache;

inline SCache &Cache()
{
	return g_cache;
}

struct SLock
{
};

inline void SetUp(SCache &cache)
{
	cache.state = 1;
}
#else
std::atomic_flag g_lock = ATOMIC_FLAG_INIT;

// The lists of the thread have no constructor or destructor, so that they can be
// used before they are set up and after the thread has ended. They are set up with
// the owner, whose destructor hands the blocks to the shared lists when the thread
// ends. The lists are held for little time, so the lock just waits for them
thread_local SCache t_cache;

inline SCache &Cache()
{
	return t_cache;
}

struct SLock
{
	SLock()
	{
		while( g_lock.test_and_set(std::memory_order_acquire) )
			std::this_thread::yield();
	}
	~SLock()
	{
		g_lock.clear(std::memory_order_release);
	}
};

void Release(SCache &cache);

struct SCacheOwner
{
	SCacheOwner() : used(false) {}
	~SCacheOwner()
	{
		if( used )
			Release(t_cache);
	}
	bool used;
};

thread_local SCacheOwner t_owner;

inline void SetUp(SCache &cache)
{
	t_owner.used = true;
	cache.state = 1;
}

// Hands all the blocks of the thread to the shared lists
void Release(SCache &cache)
{
	cache.state = 2;
	SLock lock;
	for( size_t n = 1; n <= kLists; n++ )
	{
		SList &list = cache.lists[n];
		while( list.head )
		{
			void *block = list.head;
			list.head = Next(block);
			Next(block) = g_lists[n].head;
			g_lists[n].head = block;
			g_lists[n].count++;
		}
		list.count = 0;
	}
}
#endif

// Moves up to count blocks from the shared list to the list of the thread, and cuts
// the rest from the chunk. The shared lock must be held. Returns false if there is
// no memory for any block
bool Refill(SList &list, size_t index, size_t count)
{
	SList &shared = g_lists[index];
	while( count && shared.head )
	{
		void *block = shared.head;
		shared.head = Next(block);
		shared.count--;
		Next(block) = list.head;
		list.head = block;
		list.count++;
		count--;
	}

	const size_t size = kHeader + index * kStep;
	while( count )
	{
		if( size_t(g_chunkEnd - g_chunk) < size )
		{
			char *chunk = static_cast<char*>(malloc(kChunk));
			if( chunk == 0 )
				return list.head != 0;
			g_chunk    = chunk;
			g_chunkEnd = chunk + kChunk;
		}
		void *block = g_chunk;
		g_chunk += size;
		ListIndex(block) = index;
		Next(block) = list.head;
		list.head = block;
		list.count++;
		count--;
	}
	return true;
}

void *AllocLarge(size_t size)
{
	if( size > size_t(-1) - kHeader )
		return 0;
	void *block = malloc(size + kHeader);
	if( block == 0 )
		return 0;
	ListIndex(block) = 0;
	return static_cast<char*>(block) + kHeader;
}

void *AllocSlow(SCache &cache, size_t index)
{
	if( cache.state == 0 )
		SetUp(cache);

	// After the thread has ended the blocks are taken one by one
	SList &list = cache.lists[index];
	SList single = { 0, 0 };
	SList &target = cache.state == 2 ? single : list;
	size_t count = 1;
	if( cache.state != 2 )
	{
		count = kBatchBytes / (kHeader + index * kStep);
		if( count > kMaxBatch )
			count = kMaxBatch;
	}

	{
		SLock lock;
		if( !Refill(target, index, count) )
			return 0;
	}

	void *block = target.head;
	target.head = Next(block);
	target.count--;
	return static_cast<char*>(block) + kHeader;
}

void FreeSlow(SCache &cache, void *block, size_t index)
{
	if( cache.state == 0 )
		SetUp(cache);

	if( cache.state == 2 )
	{
		SLock lock;
		Next(block) = g_lists[index].head;
		g_lists[index].head = block;
		g_lists[index].count++;
		return;
	}

	// Keeps the first half of the list, with the block, and hands the rest back
	SList &list = cache.lists[index];
	Next(block) = list.head;
	list.head = block;
	list.count++;

	size_t keep = list.count / 2;
	void *last = list.head;
	for( size_t n = 1; n < keep; n++ )
		last = Next(last);
	void *first = Next(last);
	Next(last) = 0;
	void *tail = first;
	size_t moved = 1;
	while( Next(tail) )
	{
		tail = Next(tail);
		moved++;
	}
	list.count = keep;

	SLock lock;
	Next(tail) = g_lists[index].head;
	g_lists[index].head = first;
	g_lists[index].count += moved;
}

} // namespace

void *CJITCompiler::AllocMemory(size_t size)
{
	if( size > kMaxSize )
		return AllocLarge(size);
	size_t index = size ? (size + kStep - 1) / kStep : 1;

	SCache &cache = Cache();
	SList &list = cache.lists[index];
	void *block = list.head;
	if( block == 0 )
		return AllocSlow(cache, index);
	list.head = Next(block);
	list.count--;
	return static_cast<char*>(block) + kHeader;
}

void CJITCompiler::FreeMemory(void *mem)
{
	if( mem == 0 )
		return;
	void *block = static_cast<char*>(mem) - kHeader;
	size_t index = ListIndex(block);
	if( index == 0 )
	{
		free(block);
		return;
	}

	SCache &cache = Cache();
	SList &list = cache.lists[index];
	if( cache.state != 1 || (list.count + 1) * index > kCacheBytes / kStep )
	{
		FreeSlow(cache, block, index);
		return;
	}
	Next(block) = list.head;
	list.head = block;
	list.count++;
}

END_AS_NAMESPACE
