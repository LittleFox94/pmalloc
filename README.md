This is an allocator for physically continuous memory in linux
userspace.

## Implementation details:

Memory is requested from the system with mmap, allocating the smallest hugepage
size available on the system matching the request - so on x86\_64 this means
either a 4KiB, a 2MiB or a 1GiB page. Memory not used in a page will be used for
other allocations.

Each block allocated from the system has a header `struct pmem`, storing the
originally allocated size (how big the `mmap`'d block is), the amount of memory in
use/freed and the physical address of the start of the block, which is retrieved by
reading `/proc/self/pagemap`, this requires special privileges! Check proc(5) for details.
This header also contains pointer to the previous and the next one, forming a
double-linked list of memory blocks. 

When the user of this library makes an allocation, first every already allocated
block is checked if it has enough space for that allocation. Only space after the
last allocation will be used, so if you `a = pmalloc(), b=pmalloc(), free(a)`, the
memory used by `a` is not usable again - only the space needed for it is added to
the `size_freed` of the block. This design decision makes the metadata to store per
block very simple and fits the use case I needed this for - allocating some memory
on start of program, maybe allocating some more at some point and freeing at the end.

If a memory blocks `size_freed` matches the `size_used`, it will be given back to the
system, so removed from the linked list and `munmap`'d.

The available huge page sizes are retrieved by listing the directories in
`/sys/kernel/mm/hugepages` (`retrieve_hugepage_sizes` does this). Before doing a `mmap`,
`pmalloc` will check if enough (mostly 1, if more it might already be problematic) pages
of the needed size are available by checking `/sys/kernel/mm/hugepages/$dir/free_hugepages`.
If there aren't, it tries to increase `nr_hugepages` in the same directory; this might work,
but probably won't when the system is online for some time already, since memory
fragmentation is a thing (check the linux docs for details: admin-guide/mm/hugetlbpage).

The whole thing is quite chatty on anything unexpected. There is no way to make it
non-chatty, deal with it.


## Tests are failing!

One of the tests is allocating a 3MB chunk. On x86\_64, this needs to allocate a 1GiB
huge page. Since those aren't allocated by default, you systems memory might be to
fragmented to allocate one. There is a linux cmdline flag to allocate some on boot
(again, check out linux docs on that admin-guide/mm/hugetlbpage). If only the 3MiB test
is failing, you can consider it as `success`. I might change the test one day to only
log that as error when the system would have had the hugepages for that.


## What did you need this for?

Testing LF OS drivers on linux in userspace, especially one of the few drivers LF OS has
in kernel space: XHCI DbC (debug class). Unbinding the device driver from the PCI node
and then running the driver code against it, using this library for physical memory
allocations.

It's very cursed, and also kinda funny - since this is one of the few drivers in LF OS
kernel space, it makes a lot of sense to test it in linux userspace - with linux having
most drivers in the kernel :D
