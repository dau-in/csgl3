#ifndef MEMORY_H
#define MEMORY_H

namespace Render
{

void memoryInit();

void *memoryStaticAlloc(int size, int alignment);

void *memoryLevelAlloc(int size, int alignment);
void memoryLevelFree();

uint8_t *memoryTempPtr();
void *memoryTempAlloc(int size, int alignment);
void memoryTempFree(uint8_t *offset);

template<typename T>
T *memoryStaticAlloc(int count)
{
    T *result = static_cast<T *>(memoryStaticAlloc(count * sizeof(T), alignof(T)));
    memset(result, 0, count * sizeof(T));
    return result;
}

template<typename T>
T *memoryLevelAlloc(int count)
{
    T *result = static_cast<T *>(memoryLevelAlloc(count * sizeof(T), alignof(T)));
    memset(result, 0, count * sizeof(T));
    return result;
}

class TempMemoryScope
{
public:
    TempMemoryScope()
        : m_ptr{ memoryTempPtr() }
    {
    }

    TempMemoryScope(const TempMemoryScope &) = delete;
    TempMemoryScope(TempMemoryScope &&) = delete;

    ~TempMemoryScope()
    {
        memoryTempFree(m_ptr);
    }

    template<typename T>
    T *Alloc(int count)
    {
        if (!count)
        {
            return nullptr;
        }

        void *result = memoryTempAlloc(count * sizeof(T), alignof(T));
        memset(result, 0, sizeof(T) * count);
        return static_cast<T *>(result);
    }

private:
    uint8_t *m_ptr{};
};

}

#endif //MEMORY_H
