#include <gtest/gtest.h>

extern "C" {
#include "pmalloc.h"
}

TEST(pmalloc, base) {
    void* mem = pmalloc(20, 1);
    EXPECT_TRUE(mem);
}

TEST(pmalloc, with_0_align) {
    void* mem = pmalloc(20, 0);
    EXPECT_TRUE(mem);
}

TEST(pmalloc, with_8_align) {
    void* mem = pmalloc(20, 8);
    EXPECT_TRUE((uint64_t)mem % 8 == 0);
}

TEST(pmalloc, with_16_align) {
    void* mem = pmalloc(20, 16);
    EXPECT_TRUE((uint64_t)mem % 16 == 0);
}

TEST(pmalloc, 1M_alloc) {
    void* mem = pmalloc(1024 * 1024, 0);
    EXPECT_TRUE(mem);
}

TEST(pmalloc, 3M_alloc) {
    void* mem = pmalloc(3 * 1024 * 1024, 0);
    EXPECT_TRUE(mem);
}

TEST(virt_to_phys, test) {
    void* memA = pmalloc(20, 1);
    EXPECT_TRUE(memA);

    void* memB = pmalloc(20, 1);
    EXPECT_TRUE(memB);

    void* memC = pmalloc(20, 1);
    EXPECT_TRUE(memC);

    EXPECT_TRUE(virt_to_phys(memA));
    EXPECT_TRUE(virt_to_phys(memB));
    EXPECT_TRUE(virt_to_phys(memC));

    size_t pagemask = sysconf(_SC_PAGESIZE) - 1;

    EXPECT_EQ(virt_to_phys(memA) & pagemask, (uint64_t)memA & pagemask);
    EXPECT_EQ(virt_to_phys(memB) & pagemask, (uint64_t)memB & pagemask);
    EXPECT_EQ(virt_to_phys(memC) & pagemask, (uint64_t)memC & pagemask);

    // since we allocate 20 bytes each, the physical address of one allocation
    // has to be larger than physical address of the one before + 20
    // the exact offset from one allocation to the next is an implementation
    // detail ...
    EXPECT_GT(virt_to_phys(memB), virt_to_phys(memA) + 20);
    EXPECT_GT(virt_to_phys(memC), virt_to_phys(memB) + 20);

    // let's test that implementation detail anyway, since we have an exact value
    // to test against. Each allocation is prefixed with a pointer to the start
    // of the block. We assume all data pointers have the same size
    EXPECT_EQ(virt_to_phys(memB), virt_to_phys(memA) + 20 + sizeof(void*));
    EXPECT_EQ(virt_to_phys(memC), virt_to_phys(memB) + 20 + sizeof(void*));
}

TEST(pfree, WritingSignature) {
    void* memA = pmalloc(64, 0);
    ASSERT_TRUE(memA);

    // we allocate a second time to not have our whole block be unmapped
    // on pfree
    void* memB = pmalloc(64, 0);
    ASSERT_TRUE(memB);

    uint64_t val = *((uint64_t*)memA - 1);
    pfree(memA);

    EXPECT_NE(*((uint64_t*)memA - 1), val);

    const char* signature = "DISFREED";

    EXPECT_EQ(*((uint64_t*)memA - 1), *(uint64_t*)signature);
}

TEST(pfreeDeathTest, UnmappingPage) {
    uint64_t* memA = (uint64_t*)pmalloc(64, 0);
    ASSERT_TRUE(memA);

    *memA = 0x13374223;
    pfree(memA);

    auto die = [](void* mem) {
        if(*(uint64_t*)mem == 0x13374223) {
            SUCCEED(); // which actualy signals a failure, since this won't die
        }
    };

    ASSERT_DEATH(die(memA), "");
}
