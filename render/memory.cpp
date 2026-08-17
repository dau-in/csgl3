#include "stdafx.h"
#include "memory.h"

namespace Render
{

constexpr int StaticBlockSize = 16 << 20;
constexpr int LevelBlockSize = 16 << 20;
constexpr int TempBlockSize = 32 << 20;

struct MemoryBlock
{
    uint8_t *ptr;
    uint8_t *begin;
    uint8_t *end;
};

static MemoryBlock s_staticBlock;
static MemoryBlock s_levelBlock;
static MemoryBlock s_tempBlock;

static void InitializeBlock(MemoryBlock &block, int size)
{
    block.begin = new uint8_t[size];
    block.ptr = block.begin;
    block.end = block.begin + size;
}

static void ResetBlock(MemoryBlock &block)
{
    block.ptr = block.begin;
}

static void *AllocateFromBlock(MemoryBlock &block, int size, int alignment)
{
    block.ptr = AlignUp(block.ptr, alignment);
    if (size < 0 || size > block.end - block.ptr)
    {
        platformError("Out of memory");
    }

    void *result = block.ptr;
    block.ptr += size;
    return result;
}

void memoryInit()
{
    InitializeBlock(s_staticBlock, StaticBlockSize);
    InitializeBlock(s_levelBlock, LevelBlockSize);
    InitializeBlock(s_tempBlock, TempBlockSize);
}

void *memoryStaticAlloc(int size, int alignment)
{
    return AllocateFromBlock(s_staticBlock, size, alignment);
}

void *memoryLevelAlloc(int size, int alignment)
{
    return AllocateFromBlock(s_levelBlock, size, alignment);
}

void memoryLevelFree()
{
    ResetBlock(s_levelBlock);
}

uint8_t *memoryTempPtr()
{
    return s_tempBlock.ptr;
}

void *memoryTempAlloc(int size, int alignment)
{
    return AllocateFromBlock(s_tempBlock, size, alignment);
}

void memoryTempFree(uint8_t *ptr)
{
    GL3_ASSERT(ptr <= s_tempBlock.ptr);
    s_tempBlock.ptr = ptr;
}

}
