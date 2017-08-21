#include "event.h"
//---------------------------------
const uint16_t TIMER_IDLE = 0xFFFF;
//----
struct
{
    uint16_t timer;
    event_t  event;
} timer[EVENT_MAX_SIZE]; // очередь таймеров
//----------------------------
event_t event[EVENT_MAX_SIZE]; // очередь событий
//----------------------
uint8_t event_count = 0;
uint8_t event_head  = 0;
uint8_t event_tail  = 0;
//-----------------------------
bool EVENT_Insert(event_t evt);
//-------------------
void EVENT_Init(void)
{
    for(uint8_t i = 0; i < EVENT_MAX_SIZE; ++i)
    {
        timer[i].timer = TIMER_IDLE;
        timer[i].event = NULL;
    }
    
    uint32_t systick = 48000 - 1;
    
    SysTick->LOAD = systick; // Загрузка значения
    SysTick->VAL  = systick; // Обнуляем таймеры и флаги записью 
    SysTick->CTRL =	SysTick_CTRL_CLKSOURCE_Msk |
                    SysTick_CTRL_TICKINT_Msk |
                    SysTick_CTRL_ENABLE_Msk;
}
//-------------------------------------------
bool EVENT_Create(uint16_t time, event_t evt)
{
    for(uint8_t i = 0; i < EVENT_MAX_SIZE; ++i)
    {
        if(timer[i].timer == TIMER_IDLE)
        {
            timer[i].timer = time;
            timer[i].event = evt;
            
            return true;
        }
    }
    
    return false;
}
//-------------------------
event_t EVENT_Execute(void)
{
    event_t evt = NULL;
    
    if(event_count > 0)
    {
        evt = event[event_head++];
        
        if(event_head == EVENT_MAX_SIZE)
            event_head = 0;
        
        event_count--;
    }
    
    return evt;
}
//----------------------------
bool EVENT_Insert(event_t evt)
{
    if(event_count == EVENT_MAX_SIZE)
        return false;
    
    event[event_tail++] = evt;
    
    if(event_tail == EVENT_MAX_SIZE)
        event_tail = 0;
    
    event_count++;
    
    return true;
}
//------------------------
void SysTick_Handler(void)
{
    for(uint8_t i = 0; i < EVENT_MAX_SIZE; ++i)
    {
        if(timer[i].timer != TIMER_IDLE)
        {
            timer[i].timer--;
            
            if(timer[i].timer == 0)
            {
                EVENT_Insert(timer[i].event);
                
                timer[i].timer = TIMER_IDLE;
                timer[i].event = NULL;
            }
        }
    }
}
