#ifdef USE_MEM_VM

#include "current_sandbox.h"
#include "panic.h"
#include "runtime.h"
#include "sandbox_types.h"
#include "types.h"

#include <sys/mman.h>

void
expand_memory(void)
{
	struct sandbox *sandbox = current_sandbox_get();

	// FIXME: max_pages = 0 => no limit. Issue #103.
	assert((sandbox->sandbox_size + local_sandbox_context_cache.memory.size) / WASM_PAGE_SIZE < WASM_MAX_PAGES);
	assert(sandbox->state == SANDBOX_RUNNING);
	// Remap the relevant wasm page to readable
	char *mem_as_chars = local_sandbox_context_cache.memory.start;
	char *page_address = &mem_as_chars[local_sandbox_context_cache.memory.size];

	void *map_result = mmap(page_address, WASM_PAGE_SIZE, PROT_READ | PROT_WRITE,
	                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);

	// TODO: Refactor to return RC signifying out-of-mem to caller. Issue #96.
	if (map_result == MAP_FAILED) panic("Mapping of new memory failed");
	if (local_sandbox_context_cache.memory.size > local_sandbox_context_cache.memory.max)
		panic("expand_memory - Out of Memory!. %u out of %lu\n", local_sandbox_context_cache.memory.size,
		      local_sandbox_context_cache.memory.max);

	local_sandbox_context_cache.memory.size += WASM_PAGE_SIZE;

#ifdef LOG_SANDBOX_MEMORY_PROFILE
	// Cache the runtime of the first N page allocations
	if (likely(sandbox->timestamp_of.page_allocations_size < SANDBOX_PAGE_ALLOCATION_TIMESTAMP_COUNT)) {
		sandbox->timestamp_of.page_allocations[sandbox->timestamp_of.page_allocations_size++] =
		  sandbox->duration_of_state.running
		  + (uint32_t)(__getcycles() - sandbox->timestamp_of.last_state_change);
	}
#endif

	// local_sandbox_context_cache is "forked state", so update authoritative member
	sandbox->memory.size = local_sandbox_context_cache.memory.size;
}

INLINE char *
get_memory_ptr_for_runtime(uint32_t offset, uint32_t bounds_check)
{
	// Due to how we setup memory for x86, the virtual memory mechanism will catch the error, if bounds <
	// WASM_PAGE_SIZE
	assert(bounds_check < WASM_PAGE_SIZE
	       || (local_sandbox_context_cache.memory.size > bounds_check
	           && offset <= local_sandbox_context_cache.memory.size - bounds_check));

	char *mem_as_chars = (char *)local_sandbox_context_cache.memory.start;
	char *address      = &mem_as_chars[offset];

	return address;
}

#else
#error "Incorrect runtime memory module!"
#endif
