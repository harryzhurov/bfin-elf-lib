//----------------------------------------------------------------------------
// Работа с heap by zltigo
// Структура heap:
// {first_heap_mcb| memory part} {heap_mcb| memory part}...{heap_mcb| memory part}
//  heap_mcb - описатель элемента памяти (Memory Comtrol Block)
//            memory part - элемент памяти, который описывается соответствующим MCB
//  Поле mcb.next описателя последнего MCB всегда указывает
//  на первый MCB - циклическая структура.
//  Указатель mcb.prev первого MCB указывает сам на себя.
//----------------------------------------------------------------------------

#include <stdlib.h>
#include <stdio.h>
#include <new>
#include "heap.h"

extern std::nothrow_t const std::nothrow = {};

void * operator new(size_t size, std::nothrow_t const &)
{
    return Heap.malloc(size);
}

void * operator new(size_t size)
{
    return Heap.malloc(size);
}

void operator delete(void * ptr)     // delete allocated storage
{
    Heap.free(ptr);
}

extern "C" void * malloc(size_t size)
{
    return Heap.malloc(size);
}

extern "C" void free(void * ptr)
{
    Heap.free(ptr);
}
/*
extern "C" void * _sbrk(size_t n)
{
    return 0;
}
*/
//----------------------------------------------------------------------------
// Инициализирует 'heap'.
// *heap - Указатель на структуру описываюшую heap
//----------------------------------------------------------------------------
heap::heap(uint32_t * pool, int size_items)
: start((mcb *)pool)
, freemem((mcb *)pool)
{
    mcb *fmcb = start;
    // Циклическая структура
    fmcb->next = fmcb;
    // Указатель на предыдущий MCB указывает сам на себя
    fmcb->prev = fmcb;
    // Размер области памяти
    fmcb->ts.size = size_items * sizeof(*pool) - sizeof(mcb);
    // Область памяти свободна
    fmcb->ts.type = mcb::FREE;

    // После инициализации heap представляет собой один свободный блок,
    // который имеет размер size минус размер MCB.
}

/*
void heap::add(void * pool, int size )
{
    mcb *xptr = (mcb *)pool;
    mcb *tptr = freemem;
    // Формирование нового MCB в блоке
    xptr->next = tptr;
    xptr->prev = tptr;
    xptr->ts.size = size - sizeof(mcb);
    xptr->ts.type = mcb::FREE;
    // Reinit Primary MCB
    tptr->next = xptr;
    xptr->prev = xptr; //?????
}
*/

heap::mcb * heap::mcb::split(size_t size, heap::mcb * start)
{
    uintptr_t new_mcb_addr = (uintptr_t)this + size;
    mcb *new_mcb = (mcb *)new_mcb_addr;
    new_mcb->next = next;
    new_mcb->prev = this;
    new_mcb->ts.size = ( ts.size - size );
    new_mcb->ts.type = FREE;
    // Reinit current MCB
    next = new_mcb;
    ts.size = size;
    ts.type = ALLOCATED;            // Mark block as used
    // Если следующий MCB не последний, то mcb.prev следующего за ним
    // должно теперь указывать на выделенный (xptr) MCB
    if( new_mcb->next != start )
        ( new_mcb->next )->prev = new_mcb;
    return new_mcb;
}

//----------------------------------------------------------------------------
// malloc()
//----------------------------------------------------------------------------

void * heap::malloc( size_t size )
{
    // add mcb size and round up to HEAP_ALIGN
    size = (size + sizeof(mcb) + ( HEAP_ALIGN - 1 )) & ~( HEAP_ALIGN - 1 );

    mcb *xptr;
    if(USE_FULL_SCAN)
        xptr = 0;

    void *Allocated;
    size_t free_cnt = 0;

    OS::TMutexLocker Lock(Mutex);
    mcb *tptr = freemem;  // Поиск начинается с первого свободного
    for(;;)
    {
        if( tptr->ts.type == mcb::FREE )
        {
            if( !USE_FULL_SCAN )
                ++free_cnt;
            if( tptr->ts.size >= size                                   // Требуемый и найденный размеры памяти равны
                 && tptr->ts.size <= size + sizeof(mcb) + HEAP_ALIGN)   // или найденый больше, но в свободном месте
                                                                        // не поместится новый блок хотя бы на один элемент
            {
                tptr->ts.type = mcb::ALLOCATED;                         // Резервируем блок
                Allocated = tptr->pool();
                if( USE_FULL_SCAN )
                    ++free_cnt;
                break;
            }
            else
            {
                if( USE_FULL_SCAN )
                {
                    if( xptr == NULL )
                    {
                        if( tptr->ts.size >= size)                      // Массив достаточен для размещения блока и его MCB?
                            xptr = tptr;
                        ++free_cnt;
                    }
                }
                else if( tptr->ts.size >= size )                        // Массив достаточен для размещения блока и его MCB?
                {
                    // Create new free MCB in parent's MCB tail
                    xptr = tptr->split(size, start);
                    Allocated = tptr->pool();
                    break;
                }
            }
        }

        tptr = tptr->next;                                              // Get ptr to next MCB
        if( tptr == start )                                             // End of heap?
        {
            if( USE_FULL_SCAN && xptr != 0 )
            {
                tptr = xptr;
                // Create new free MCB in parent's MCB tail
                xptr = tptr->split(size, start);
                Allocated = tptr->pool();
                break;
            }
            else
            {
                Allocated = 0;                                          // No Memory
                break;
            }
        }
    }

    if( ( free_cnt == 1 )&&( Allocated ) )          // Был занят первый свободный блок памяти?
        freemem = tptr->next;                       // Указатель 'первый свободный' на следующий MCB
                                                    // он или свободен или по крайней мере ближе к следующему свободному
    return Allocated;
}

void heap::mcb::merge_with_next(mcb * start)
{
    // Check Next MCB
    mcb* other = next;
    // Объединяем текущий и следующий MCB
    ts.size = ts.size + other->ts.size;
    other = next = other->next;
    // Если следующий за объединенным MCB не последний, то меняем в нем mcb.prev на текущий
    if( other != start )
        other->prev = this;

}
//----------------------------------------------------------------------------
// free()
//----------------------------------------------------------------------------
void heap::free(void *pool )
{
    // В общем надо контролировать _все_ :( указатели на попадание в RAM, иначе будет exception :(
    // Или использовать тупой перебор MCB и сравнивать с pool

    // Проверка указателя на выровненность
    if( !pool || ((uintptr_t)pool & (HEAP_ALIGN - 1)))
        return;

    mcb *xptr;
    mcb *tptr = (mcb *)pool - 1;

    OS::TMutexLocker Lock(Mutex);
    // Пока? только mem_ptr и то по одной границе.
    // Перекрестная проверка для определения валидности
    xptr = tptr->prev;
    if( (xptr != tptr && xptr->next != tptr) || pool < start )
        return;

    // Valid pointer present ------------------------------------------------
    tptr->ts.type = mcb::FREE;          // Mark as "free"
    // Check Next MCB
    xptr = tptr->next;
    // Если следующий MCB свободен и не первый в heap..
    if( xptr->ts.type == mcb::FREE && xptr != start )
    {
        // Объединяем текущий (tptr) и следующий (xptr) MCB
        tptr->merge_with_next(start);
    }
    // Check previous MCB
    xptr = tptr->prev;
    // Если предыдущий MCB свободен и текущий не первый в heap...
    if( xptr->ts.type == mcb::FREE && tptr != start )
    {
        // Объединяем текущий (tptr) и предыдущий (xptr) MCB
        xptr->merge_with_next(start);
        tptr = xptr;            // tptr всегда на освободившийся блок.
    }
    // Установка heap->freem для более быстрого перебора
    if( tptr < freemem )        // Осводившийся блок находится перед считающимся первым свободным?
        freemem = tptr;         // Новый указатель на первый 'free'
}

heap::summary heap::info()
{
    summary Result =
    {
        { 0, 0, 0 },
        { 0, 0, 0 }
    };

    OS::TMutexLocker Lock(Mutex);
    mcb *pBlock = freemem;
    do
    {
        summary::info * pInfo = pBlock->ts.type == mcb::FREE ? &Result.Free : &Result.Used;
        ++pInfo->Blocks;
        pInfo->Size += pBlock->ts.size;
        if(pInfo->Block_max_size < pBlock->ts.size)
            pInfo->Block_max_size = pBlock->ts.size;
        pBlock = pBlock->next;
    }
    while(pBlock != start);
    return Result;
}
