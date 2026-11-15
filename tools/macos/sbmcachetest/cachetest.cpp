// Host test of StarbiteMali's buffer cache (pending patch 0459): the driver's own code (cache_cut.inc) with its
// worker thread, hammered by threads that create and close buffers like WindowServer does. Checks: a buffer from
// the cache is always all zero, is never handed out twice or after it was freed, the page count matches the
// slots, the limit holds, and everything unused is freed once the clients stop.
//   cachetest SECONDS THREADS
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <set>
#include <thread>
#include <vector>
typedef uint64_t IOByteCount; typedef uint64_t addr64_t; typedef uint32_t ppnum_t; typedef uint64_t AbsoluteTime; typedef uint32_t UInt32;
typedef int wait_result_t;
#define PAGE_SIZE 0x4000
#define NSEC_PER_SEC 1000000000ULL
#define THREAD_UNINT 0
#define kIOMemoryMapperNone 0
#define atop_64(x) ((x) >> 14)
// time runs 100 times faster than real time, so the cache's 3 s age limit is 30 ms here
static uint64_t mach_absolute_time() { return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count() * 100; }
static void nanoseconds_to_absolutetime(uint64_t ns, uint64_t *o) { *o = ns; }
struct IOLock { std::mutex m; std::condition_variable cv; };
static void IOLockLock(IOLock *l) { l->m.lock(); }
static void IOLockUnlock(IOLock *l) { l->m.unlock(); }
static int IOLockSleep(IOLock *l, void *, UInt32) { std::unique_lock<std::mutex> u(l->m, std::adopt_lock); l->cv.wait(u); u.release(); return 0; }
static int IOLockSleepDeadline(IOLock *l, void *, AbsoluteTime deadline, UInt32) {
	std::unique_lock<std::mutex> u(l->m, std::adopt_lock);
	uint64_t now = mach_absolute_time();
	l->cv.wait_for(u, std::chrono::nanoseconds(deadline > now ? (deadline - now) / 100 : 0)); u.release(); return 0; }
static void IOLockWakeup(IOLock *l, void *, bool) { l->cv.notify_all(); }
static std::atomic<long> made, freed, attr_resets, gpu_unmaps;
static std::mutex freed_lock; static std::set<void *> freed_set;
struct IOMemoryDescriptor {
	std::vector<uint8_t> bytes; std::atomic<int> live{0};
	IOByteCount getLength() { return bytes.size(); }
	addr64_t getPhysicalSegment(IOByteCount off, IOByteCount *, int) { return 0x80000000ull + off; }
	void *getBytesNoCopy() { return bytes.data(); }
	void release() { { std::lock_guard<std::mutex> g(freed_lock); if (!freed_set.insert(this).second) { printf("FAIL: freed twice\n"); abort(); } } if (live) { printf("FAIL: a buffer in use was freed\n"); abort(); } freed++; }
};
struct IOBufferMemoryDescriptor : IOMemoryDescriptor {};
#define OSDynamicCast(T, x) static_cast<T *>(x)
static void starlte_gpu_user_unmap(uint64_t, unsigned) { gpu_unmaps++; }
static void sbm_va_free(uint64_t, uint64_t) {}
static void pmap_set_cache_attributes(ppnum_t, unsigned) { attr_resets++; }
#define IOLog(...) do {} while (0)

#include "cache_cut.inc"

static std::atomic<bool> stop; static std::atomic<long> fails, hits_seen, creates;
static void client(unsigned seed) {
	std::vector<IOBufferMemoryDescriptor *> held;
	auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return seed >> 8; };
	static const uint32_t sizes[] = { 8328, 8328, 8328, 6144, 256, 64, 17, 16, 4 };    // 4 KiB pages; WindowServer's 34 MB target most often
	while (!stop) {
		if (held.size() < 6 && (held.empty() || rnd() % 2)) {
			uint32_t pages = sizes[rnd() % 9];
			IOBufferMemoryDescriptor *mem = NULL; uint64_t va = 0; bool zeroed = false;
			creates++;
			if (sbm_cache_get(pages, &mem, &va, &zeroed)) {
				hits_seen++;
				if (mem->live.exchange(1)) { printf("FAIL: a buffer handed out twice\n"); fails++; }
				{ std::lock_guard<std::mutex> g(freed_lock); if (freed_set.count(mem)) { printf("FAIL: a freed buffer handed out\n"); fails++; } }
				if (mem->bytes.size() != (size_t)pages * 0x1000 || va != (uint64_t)(uintptr_t)mem) { printf("FAIL: wrong buffer for the size\n"); fails++; }
				if (!zeroed) bzero(mem->getBytesNoCopy(), (size_t)pages * 0x1000);
				// all zero? (sampled every 64 bytes plus the ends: the worker zeroes in one call)
				for (size_t i = 0; i < mem->bytes.size(); i += 64) if (mem->bytes[i]) { printf("FAIL: not zero at %zu (zeroed=%d)\n", i, zeroed); fails++; break; }
				if (mem->bytes.back()) { printf("FAIL: last byte not zero\n"); fails++; }
			} else {
				mem = new IOBufferMemoryDescriptor(); mem->bytes.assign((size_t)pages * 0x1000, 0); mem->live = 1; made++;
			}
			held.push_back(mem);
		} else {
			size_t i = rnd() % held.size();
			IOBufferMemoryDescriptor *mem = held[i]; held.erase(held.begin() + i);
			memset(mem->getBytesNoCopy(), 0xAA, mem->bytes.size());     // the GPU drew into it
			uint32_t pages = (uint32_t)(mem->bytes.size() / 0x1000);
			mem->live = 0;
			if (!sbm_cache_put(mem, (uint64_t)(uintptr_t)mem, pages)) sbm_free_buffer(mem, (uint64_t)(uintptr_t)mem, pages);
		}
		// the books: held pages = the reusable slots', never over the limit by more than the buffer just put
		IOLockLock(sbm_cache_lock);
		uint64_t sum = 0; for (int k = 0; k < SBM_CACHE_SLOTS; k++) if (sbm_cache[k].state == SBM_C_DIRTY || sbm_cache[k].state == SBM_C_READY) sum += sbm_cache[k].pages;
		uint64_t busy = 0; for (int k = 0; k < SBM_CACHE_SLOTS; k++) if (sbm_cache[k].state == SBM_C_BUSY) busy += sbm_cache[k].pages;
		if (sbm_cache_pages < sum || sbm_cache_pages > sum + busy) { printf("FAIL: page count %llu, slots %llu (+%llu busy)\n", (unsigned long long)sbm_cache_pages, (unsigned long long)sum, (unsigned long long)busy); fails++; }
		if (sbm_cache_pages > sbm_cache_limit + 8328) { printf("FAIL: %llu pages held, limit %llu\n", (unsigned long long)sbm_cache_pages, (unsigned long long)sbm_cache_limit); fails++; }
		IOLockUnlock(sbm_cache_lock);
		if (rnd() % 64 == 0) std::this_thread::sleep_for(std::chrono::microseconds(rnd() % 3000));
	}
	for (auto mem : held) { mem->live = 0; uint32_t pages = (uint32_t)(mem->bytes.size() / 0x1000); if (!sbm_cache_put(mem, (uint64_t)(uintptr_t)mem, pages)) sbm_free_buffer(mem, (uint64_t)(uintptr_t)mem, pages); }
}

int main(int argc, char **argv) {
	int secs = argc > 1 ? atoi(argv[1]) : 3, nthreads = argc > 2 ? atoi(argv[2]) : 3;
	sbm_cache_lock = new IOLock(); sbm_cache_limit = 160 * 256;
	std::thread(sbm_cache_thread, nullptr, 0).detach();
	std::vector<std::thread> t; for (int i = 0; i < nthreads; i++) t.emplace_back(client, 12345u + 77u * i);
	std::this_thread::sleep_for(std::chrono::seconds(secs)); stop = true; for (auto &x : t) x.join();
	// idle: after the age limit (30 ms here) the worker has freed everything
	std::this_thread::sleep_for(std::chrono::milliseconds(400));
	IOLockLock(sbm_cache_lock);
	int used = 0; for (int k = 0; k < SBM_CACHE_SLOTS; k++) used += sbm_cache[k].state != SBM_C_FREE;
	uint64_t held = sbm_cache_pages; IOLockUnlock(sbm_cache_lock);
	if (used || held) { printf("FAIL: after idling %d slots in use, %llu pages held\n", used, (unsigned long long)held); fails++; }
	if (made != freed) { printf("FAIL: %ld buffers made, %ld freed\n", made.load(), freed.load()); fails++; }
	if (gpu_unmaps != freed) { printf("FAIL: %ld GPU unmaps for %ld frees\n", gpu_unmaps.load(), freed.load()); fails++; }
	printf("%s: %ld creates, %ld from the cache (%ld%%), %ld buffers allocated and freed, %ld page attribute resets\n", fails ? "FAILED" : "ok",
	    creates.load(), hits_seen.load(), creates ? hits_seen * 100 / creates : 0, made.load(), attr_resets.load());
	return fails != 0;
}
