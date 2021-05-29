#include <stdint.h>
#include <stdlib.h>

/**
 * Returns the physical address to the given virtual one.
 *
 * \returns Physical address, not as a pointer to make you think twice before dereferencing it
 */
uint64_t virt_to_phys(void* mem);

/**
 * allocate physically continuous block of memory
 * with at least $size usable bytes and the start
 * aligned to $align bytes.
 *
 * Beware! This is doing classic kernel things, in userspace.
 *
 * \param size  Size of the newly allocated memory region in bytes
 * \param align Alignment of the start of the allocated memory region
 * \returns     Virtual address pointer to the start
 *              of the allocated memory region with ($ret % $align == 0)
 */
void* pmalloc(size_t size, size_t align);

/**
 * Free the given memory allocation, making it invalid and possibly return
 * memory back to the system
 *
 * \param mem Memory allocation (that must be made by pmalloc) to free
 */
void pfree(void* mem);
