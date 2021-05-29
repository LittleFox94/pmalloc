/**
 * This is an allocator for physically continuous memory in linux
 * userspace.
 *
 * Implementation details:
 *
 * Memory is requested from the system with mmap, allocating the smallest hugepage
 * size available on the system matching the request - so on x86_64 this means
 * either a 4KiB, a 2MiB or a 1GiB page. Memory not used in a page will be used for
 * other allocations.
 *
 * Each block allocated from the system has a header `struct pmem`, storing the
 * originally allocated size (how big the mmap'd block is), the amount of memory in
 * use and the amount of memory freed. This header also contains pointer to the
 * previous and the next one, forming a double-linked list of memory blocks. The header
 * also stores the physical address of the start of the block, which is retrieved by
 * reading /proc/self/pagemap, this requires special privileges! Check proc(5) for details.
 *
 * When the user of this library makes an allocation, first every already allocated
 * block is checked if it has enough space for that allocation. Only space after the
 * last allocation will be used, so if you `a = pmalloc(), b=pmalloc(), free(a)`, the
 * memory used by `a` is not usable again - only the space needed for it is added to
 * the `size_freed` of the block. This design decision makes the metadata to store per
 * block very simple and fits the use case I needed this for - allocating some memory
 * on start of program, maybe allocating some more at some point and freeing at the end.
 *
 * If a memory blocks `size_freed` matches the `size_used`, it will be given back to the
 * system, so removed from the linked list and munmap()'d.
 *
 * The available huge page sizes are retrieved by listing the directories in
 * /sys/kernel/mm/hugepages (retrieve_hugepage_sizes does this). Before doing a mmap,
 * pmalloc will check if enough (mostly 1, if more it might already be problematic) pages
 * of the needed size are available by checking
 * /sys/kernel/mm/hugepages/$dir/free_hugepages. If there aren't, it tries to increase
 * nr_hugepages in the same directory; this might work, but probably won't when the system
 * is online for some time already, since memory fragmentation is a thing (check the linux
 * docs for details: admin-guide/mm/hugetlbpage.
 *
 * The whole thing is quite chatty on anything unexpected. There is no way to make it
 * non-chatty, deal with it.
 *
 */

#include "pmalloc.h"

#include <assert.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#include <errno.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <limits.h>

static const char* pmem_header_signature = "LFOSRULZ";
static const char* pmem_freed_signature  = "DISFREED";

//! This is the pmem header
struct pmem {
    char signature[8];

    //! Number of bytes allocated with mmap, including this structure
    size_t   size_allocated;

    //! Used range in bytes from right after the header
    size_t   size_used;

    /**
     * Memory that was in use but now is free'd in bytes. If this
     * matches the size_used, the block is free and can be given
     * back to the system
     */
    size_t   size_freed;

    //! Physical memory address
    uint64_t physical;

    struct pmem* next;
    struct pmem* prev;
};

static struct pmem *pmem_first = 0, *pmem_last = 0;

struct linux_pagemap_entry {
    uint64_t page_frame_num: 55;
    bool     soft_dirty:      1;
    bool     exclusive:       1;
    uint64_t:                 4;
    bool     file_or_shared:  1;
    bool     swapped:         1;
    bool     present:         1;
}__attribute__((packed));

static struct pmem* get_header(void* mem) {
    size_t page_size = sysconf(_SC_PAGESIZE);

    // every allocation is prefixed with a pointer to the header of that block
    struct pmem** header_prefix = (struct pmem**)(mem - sizeof(struct pmem*));

    uint8_t* vec = malloc(1);
    if(mincore((void*)((uint64_t)header_prefix / page_size * page_size), sizeof(struct pmem*), vec) == -1) {
        printf(
            "get_header: cannot check if allocation is prefixed with pointer to header, hoping the best: %s (%d)\n",
            strerror(errno), errno
        );
    }
    else if(!(vec[0] & 1)) {
        printf(
            "get_header: allocation 0x%lx is not prefixed with pointer to header\n",
            (uint64_t)mem
        );

        return 0;
    }

    // we continue if
    // * mincore returned success and vec looks correct
    // * mincore failed and then hope the best

    struct pmem* header = *header_prefix;

    if((uint64_t)header % page_size) {
        printf(
            "get_header: header is not page_size aligned, which should not normally happen: 0x%lx)\n",
            (uint64_t)header
        );
    }

    size_t header_pages = (sizeof(struct pmem) + sysconf(_SC_PAGESIZE) - 1) / sysconf(_SC_PAGESIZE);

    vec = realloc(vec, header_pages);
    if(mincore(header, sizeof(struct pmem), vec) == -1 && errno != ENOMEM) {
        printf(
            "get_header: cannot check if header is in memory, hoping the best: %s (%d)\n",
            strerror(errno), errno
        );
    }
    else if(errno == ENOMEM) {
        printf(
            "get_header: header at 0x%lx is not in memory\n",
            (uint64_t)header
        );
    }

    free(vec);

    if(memcmp(header->signature, pmem_header_signature, sizeof(header->signature)) != 0) {
        printf(
            "get_header: header at 0x%lx did not pass the signature check.\n"
            "It's either not a pmem structure or there is some serious memory corruption goign on\n",
            (uint64_t)header
        );
    }

    return header;
}

uint64_t virt_to_phys(void* mem) {
    struct pmem* header = get_header(mem);

    if(!header) {
        return 0;
    }

    uint64_t phys_base = header->physical;
    uint64_t offset    = mem - (void*)header;

    return phys_base + offset;
}

static size_t file_read_all(char** data, const char* path) {
    int fd = open(path, O_RDONLY);

    if(fd == -1) {
        printf("could not open %s for reading: %s (%d)\n", path, strerror(errno), errno);
        return 0;
    }
    else {
        size_t len = lseek(fd, 0, SEEK_END);
        lseek(fd, 0, SEEK_SET);

        *data    = malloc(len+1);
        size_t r = read(fd, *data, len);

        close(fd);

        // if we read less than we were told the file size is we warn,
        // except when the size we were told is the page size, which is
        // what happens for sysfs and co apparently?
        if(r != len && len != sysconf(_SC_PAGESIZE)) {
            printf("could not read all data from %s, only got %lu of %lu bytes: %s (%d)\n", path, r, len, strerror(errno), errno);
        }

        // zero-terminate for convenience if used as string
        (*data)[len] = 0;

        return r;
    }
}

static int size_t_compare(const void* a, const void* b) {
    const size_t _a = *(const size_t*)a;
    const size_t _b = *(const size_t*)b;

    return (int)(_a - _b);
}

static size_t retrieve_hugepage_sizes(size_t** sizes) {
    size_t ret = 1;
    *sizes = malloc(sizeof(size_t));
    (*sizes)[0] = sysconf(_SC_PAGESIZE);

    const char* hugepagesDir = "/sys/kernel/mm/hugepages";
    DIR* dir = opendir(hugepagesDir);

    if(!dir) {
        printf("Could not open directory %s: %s (%d)\n", hugepagesDir, strerror(errno), errno);
        return ret;
    }

    const char* prefix = "hugepages-";
    size_t prefixLen   = strlen(prefix);

    struct dirent* entry;
    while((entry = readdir(dir))) {
        if(strncmp(entry->d_name, prefix, prefixLen) == 0) {
            char* suffix;
            long num = strtol(entry->d_name + prefixLen, &suffix, 10);

            size_t suffixLen = strlen(suffix);

            if(suffixLen > 2 || (suffixLen == 1 && suffix[0] != 'B')) {
                printf("Don't know how to parse this directory name, unexpected len after number: %s\n", entry->d_name);
                continue;
            }
            else if(suffixLen == 2) {
                if(suffix[1] == 'B') {
                    switch(suffix[0]) {
                        case 'k':
                            num *= 1024;
                            break;
                        default:
                            // *I* know some more, but only k is being used by
                            // the linux kernel and I don't want to implement
                            // more if not needed. Also, we have to rebuild
                            // the directory name later, so failing here because
                            // we don't know the failure when we rebuild the name
                            // in pmalloc()
                            printf("Unknown suffix: %s\n", suffix);
                            continue;
                            break;
                    }
                }
            }
            // else suffix len is 0 or (1 and suffix is 'B') -> its bytes already

            *sizes = realloc(*sizes, ++ret * sizeof(size_t));
            (*sizes)[ret-1] = num;
        }
    }

    closedir(dir);

    qsort(*sizes, ret, sizeof(size_t), size_t_compare);

    return ret;
}

void* pmalloc(size_t size, size_t align) {
    if(align == 1) {
        align = 0; // so we don't add space for it
    }

    // we can then align by incrementing
    // the address returned
    size_t size_plus_align = size + align;

    // We also have to store the pointer
    // to that header right before the
    // address we return
    size_t size_plus_overhead = size_plus_align + sizeof(struct pmem*);

    struct pmem* cur = pmem_first;
    while(cur) {
        if(cur->size_allocated - cur->size_used >= size_plus_overhead) {
            void* ret = (void*)cur           // start of the mmap'd memory block
                      + sizeof(struct pmem)  // .. after header it's usable
                      + sizeof(struct pmem*) // .. but every allocation has the ptr to header in front of it
                      + cur->size_used;      // .. also this block is not fresh

            cur->size_used += size_plus_overhead;

            if(align) {
                ret += align - ((uint64_t)ret % align);
            }

            *((struct pmem**)ret - 1) = cur;
            return ret;
        }

        cur = cur->next;
    }

    // We didn't find a block with enough free space to fit
    // this new allocation, so we have to store the actual
    // header at the start of the mmap'd block of memory
    size_plus_overhead += sizeof(struct pmem);

    static size_t *page_sizes = 0;
    static size_t  page_sizes_count;

    if(!page_sizes) {
        page_sizes_count = retrieve_hugepage_sizes(&page_sizes);

        if(page_sizes_count == 1) {
            // only one page size, this is likely to be the smallest for the platform
            // which is also the default one. It's very unlikely we _only_ have huge
            // pages.
            printf(
                "pmalloc: no hugepage retrieved, allocation likely to fail. Page size: %lu\n",
                page_sizes[0]
            );
        }
    }

    int page_size; // as index for the page_sizes array
    for(page_size = 0; page_size < page_sizes_count; ++page_size) {
        if (size_plus_overhead <= page_sizes[page_size]) {
            break;
        }
    }

    if(page_size > page_sizes_count) {
        printf("pmalloc: trying to allocate memory larger than the max page size, likely to fail\n");
    }

    size_t alloc_size = size_plus_overhead;

    if(page_size < page_sizes_count) {
        alloc_size = page_sizes[page_size];
    }

    int mmapFlags = 0;

    if(page_size && page_size <= page_sizes_count) {
        mmapFlags = MAP_HUGETLB | ((int)log2(page_sizes[page_size]) << MAP_HUGE_SHIFT);
    }

    if(page_size) {
        size_t ps = page_sizes[page_size];

        // try to pre-allocate the huge pages we need
        // Beware: the code line after this assumes we use %lu as placeholder, or
        // rather one thats 3 characters long
        const char* hugepageDirFormat = "/sys/kernel/mm/hugepages/hugepages-%lukB/";

        // we would have to add 1 for the number of digits (log10 is number of digits - 1),
        // but the format string includes the placeholder, which is 3 characters long
        // so in the end we have two more characters than we need - which we subtract
        // this does NOT include space for the zero termination, it's a string length
        size_t sl = strlen(hugepageDirFormat) + log10(ps) - 2;

        const char* freeSuffix = "free_hugepages";
        const char* nrSuffix   = "nr_hugepages";

        size_t freePathLen = sl + strlen(freeSuffix) + 1;
        char* freePath = malloc(freePathLen);
        snprintf(freePath, freePathLen, hugepageDirFormat, ps / 1024);
        strcat(freePath, freeSuffix);

        size_t nrPathLen = sl + strlen(nrSuffix) + 1;
        char* nrPath = malloc(nrPathLen);
        snprintf(nrPath, freePathLen, hugepageDirFormat, ps / 1024);
        strcat(nrPath, nrSuffix);

        char *freeStr = 0, *nrStr = 0;

        size_t freeLen = file_read_all(&freeStr, freePath);
        size_t nrLen   = file_read_all(&nrStr,   nrPath);

        size_t num_pages_needed = (alloc_size + ps-1) / ps;

        size_t nr_pages = 0, free_pages = 0;

        if(nrLen) {
            char* end;
            nr_pages = strtol(nrStr, &end, 10);

            // data ends with a \n
            if(end > nrStr + nrLen + 1) {
                printf("pmalloc: nr_hugepages file (%s) contained garbage: '%s'\n", nrPath, end);
            }

            free(nrStr);
        }

        if(freeLen) {
            char* end;
            free_pages = strtol(freeStr, &end, 10);

            // data ends with a \n
            if(end > freeStr + freeLen + 1) {
                printf("pmalloc: free_hugepages file (%s) contained garbage: '%s'\n", freePath, end);
            }

            free(freeStr);

            if(free_pages < num_pages_needed) {
                // lets try to increase nr_hugepages
                int fd = open(nrPath, O_RDWR | O_SYNC);

                if(fd == -1) {
                    printf("pmalloc: cannot open %s for reading and writing (increasing the amount): %s (%d)", nrPath, strerror(errno), errno);
                }
                else {
                    size_t len = lseek(fd, 0, SEEK_END);
                    lseek(fd, 0, SEEK_SET);

                    char* buffer = malloc(len + 1);
                    memset(buffer, 0, len+1);
                    read(fd, buffer, len);

                    size_t nr = strtol(buffer, 0, 10);

                    if(nr != nr_pages) {
                        // hm this seems "very" dynamic on this system ..
                        // in theory we would have to read free_pages again, too. But since this could
                        // get very homestuck^W recursive we don't and hope for the best (but warn)
                        printf("pmalloc: nr_hugepages for hugepages-%lukB seems to be kinda dynamic, might be problematic\n", ps / 1024);
                    }

                    nr += num_pages_needed - free_pages;

                    len = log10(nr) + 2; // digits = log10(num) + 1, another +1 for zero termination
                    buffer = realloc(buffer, len);
                    snprintf(buffer, len, "%lu", nr);

                    size_t w = write(fd, buffer, len-1);

                    if(w != len - 1) {
                        printf("could not write all data to %s, only %lu of %lu bytes written: %s (%d)\n", nrPath, w, len, strerror(errno), errno);
                    }

                    close(fd);

                    free(buffer);
                }
            }
        }

        free(freePath);
        free(nrPath);
    }

    struct pmem* pmem = mmap(0,
                    alloc_size,
                    PROT_READ|PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS |
                    MAP_LOCKED  | MAP_POPULATE |
                    mmapFlags,
                    -1, 0
                );

    if(pmem == MAP_FAILED) {
        printf("pmalloc: error in mmap: %s (%d)\n", strerror(errno), errno);

        if(errno == ENOMEM) {
            printf("  -> check for available huge pages of size %lukB\n", page_sizes[page_size] / 1024);
        }

        return 0;
    }

    memcpy(pmem->signature, pmem_header_signature, sizeof(pmem->signature));
    pmem->size_allocated = alloc_size;
    pmem->size_used      = size_plus_overhead - sizeof(struct pmem);

    const char* pagemapPath = "/proc/self/pagemap";
    int pagemapFile = open(pagemapPath, O_RDONLY | O_SYNC);

    if(pagemapFile == -1) {
        printf("pmalloc: error opening %s for reading: %s (%d)\n", pagemapPath, strerror(errno), errno);
        return 0;
    }

    uint64_t lastPageFrameNumber = 0;

    for(void* i = (void*)pmem; i < (void*)pmem + alloc_size; i += sysconf(_SC_PAGESIZE)) {
        uint64_t pi = (uint64_t)i / sysconf(_SC_PAGESIZE); // page index, not Pi

        off_t seek = lseek(pagemapFile, pi * sizeof(uint64_t), SEEK_SET);

        if(seek != pi * sizeof(uint64_t)) {
            printf("pmalloc: error seeking to page entry %lu, only to to %lu(+%lu)\n", pi, seek / sizeof(uint64_t), seek % sizeof(uint64_t));
            return 0;
        }

        struct linux_pagemap_entry pagemapEntry;
        assert(sizeof(pagemapEntry) == 8);

        ssize_t r = read(pagemapFile, &pagemapEntry, sizeof(pagemapEntry));
        if(r != sizeof(uint64_t)) {
            printf(
                "pmalloc: error reading page entry, only got %lu of %lu bytes: %s (%d)\n",
                r, sizeof(pagemapEntry), strerror(errno), errno
            );

            return 0;
        }

        bool error = false;
        if(!pagemapEntry.present) {
            printf("pmalloc: page is not in memory - how?!\n");
            error = true;
        }

        if(pagemapEntry.swapped) {
            printf("pmalloc: kernel swapped that memory - ouch poor disk\n");
        }

        if(pagemapEntry.file_or_shared) {
            printf("pmalloc: page is either file mapped or shared\n");
        }

        if(!pagemapEntry.present) {
            printf("pmalloc: not mapped exclusively, might be problematic -> bailing out\n");
            error = true;
        }

        if(!pagemapEntry.page_frame_num) {
            printf("pmalloc: no page frame number, check permissions\n");
            error = true;
        }

        if(pagemapEntry.page_frame_num) {
            if(lastPageFrameNumber && pagemapEntry.page_frame_num != lastPageFrameNumber + 1) {
                printf("pmalloc: mmap'd region is not physically continuous, read other messages printed to figure out why\n");
                error = true;
            }

#if defined DEBUG
            if(!lastPageFrameNumber) {
                printf("pmalloc: pmem 0x%lx is at physical address 0x%lx\n", (uint64_t)pmem, pagemapEntry.page_frame_num * sysconf(_SC_PAGESIZE));
            }
#endif
        }

        if(error) {
            printf("pmalloc: page map entry: 0x%lx\n", *(uint64_t*)&pagemapEntry);
            return 0;
        }

        if(!lastPageFrameNumber) {
            pmem->physical = pagemapEntry.page_frame_num << 12;
        }

        lastPageFrameNumber = pagemapEntry.page_frame_num;
    }

    close(pagemapFile);

    pmem->prev = pmem_last;

    if(pmem_last) {
        pmem_last->next = pmem;
    }

    pmem_last = pmem;

    if(!pmem_first) {
        pmem_first = pmem;
    }

    void* ret = (void*)pmem + sizeof(struct pmem) + sizeof(struct pmem*);

    *((struct pmem**)ret - 1) = pmem;
    return ret;
}

void pfree(void* mem) {
    struct pmem* header = get_header(mem);

    if(!header) {
        // XXX: we should check with mincore() again
        if(*(uint64_t*)(header - sizeof(uint64_t)) == *(uint64_t*)pmem_freed_signature) {
            printf("pfree: tried to free allocation that already was freed: 0x%lx\n", (uint64_t)mem);
        }

        return;
    }

    size_t offset = mem - (void*)header;

    assert(sizeof(struct pmem*) == sizeof(uint64_t));

    // we retrieve the size of this allocation by searching from
    // mem to (mem + header->size_used - offset) and stop when we
    // either find another pointer to header or the freed-signature
    size_t size;

    for(size = 0; size < header->size_used - (offset - sizeof(struct pmem*)) + sizeof(struct pmem); ++size) {
        uint64_t val = *(uint64_t*)(mem + size);

        if(val == (uint64_t)header
        || val == *(uint64_t*)pmem_freed_signature) {
            break;
        }
    }

    if(size > header->size_used - header->size_freed) {
        printf("pfree: detected size larger than what can be allocated. Found %lu, max %lu\n", size, header->size_used - header->size_freed);
        size = header->size_used - header->size_freed;
    }

    header->size_freed += size;
    *(uint64_t*)(mem - sizeof(uint64_t)) = *(uint64_t*)pmem_freed_signature;

    // nothing is left allocated, give memory back to system
    if(header->size_freed == header->size_used) {
        if(header->prev) {
            header->prev->next = header->next;
        }

        if(header->next) {
            header->next->prev = header->prev;
        }

        if(header == pmem_first) {
            pmem_first = header->next;
        }

        if(header == pmem_last) {
            pmem_last = header->prev;
        }

        munmap((void*)header, header->size_allocated);
    }
}
