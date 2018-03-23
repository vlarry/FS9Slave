#include "device.h"
//---------------------------------------
void IO_Clock_Enable(GPIO_TypeDef* gpio);
void IO_Init(io_t io, uint8_t io_dir);
void CHANNEL_Out_Set(uint8_t index);
void CHANNEL_Out_Reset(uint8_t index);
void TIM_Scan_Init(void);
void TIM_Scan_Update(void);
void TIM_INT_Init(void);
void TIM_INT_Start(void);
float Get_Temp(uint16_t val, uint8_t in_num);
float UAIN_to_TResistance(uint16_t val, uint8_t in_num); // преобразование напряжения в сопротивление температуры
void  blink2Hz(void* output); // мигание с частотой 2Гц (для МИК-01)
void  crash(void* output); // для обработки аварийной ситуации (нет запросов от ЦП 5 сек)
void  queue_init(void);
void  insert_task(uint8_t id); // вставка задачи в очередь для мигания (МИК-01)
void  kill_task(uint8_t id); // убить задачу в очереди для мигания (МИК-01)
//----------------------
PORT_Input_Type*  io_in;
PORT_Output_Type* io_out;
//---------------------
uint8_t devAddr = 0xFF;
uint8_t devID   = 0xFF;
//-------------------------
bool Input_Changed = false;
//---------------------------
bool      is_crash   = false; // авария - отключение выхода (происходит в случае отсутствия запросов от ЦП 5 сек)
output_t* out_crash  = NULL; // выход аварийной сигнализации
input_t*  io_inOff   = 0;
input_t*  io_inOn    = 0;
input_t*  io_inPhase = NULL;
//--------------------------------------------------------------------------------
key_t keys = { 0x00000000, KEY_EMPTY_MASK, KEY_EMPTY_MASK, KEY_MODE_NONE, false };
//-----------------------------
error_t error = { 0, 0, 0, 0 };
//----------------------------
Blink_queue_t out_queue_blink;
//-----------------------------------------------------------------------
volatile pwrok_t _pwr_ok = { false, false, false, false, false, 0x0000 };
//---------------------------------------
uint16_t AIN_TEMP[MAX_SIZE_AIN_TEMP][3] = 
{
    { 0, 13, 18 }, // сопротивление термодатчика, напряжение AIN1, напряжение AIN2 (напряжения умножены на 10000)
    { 1, 170, 170 },
    { 2, 330, 340 },
    { 10, 1650, 1650 },
    { 20, 3280, 3270 },
    { 37, 6080, 6070 },
    { 48, 8010, 7980 },
    { 66, 10800, 10780 },
    { 80, 13230, 13190 },
    { 92, 15080, 15040 },
    { 100, 16470, 16420 },
    { 115, 18850, 18790 },
    { 127, 20800, 20700 },
    { 166, 27200, 27100 },
    { 178, 29200, 29100 },
    { 187, 30800, 30700 },
    { 189, 31000, 30900 },
    { 195, 32200, 32100 },
    { 200, 32900, 32900 }
};
//-----------------------------------------------------
void DEV_Create(GPIO_TypeDef* gpio, uint16_t addr_pins)
{
    io_t pin = { gpio, addr_pins };
    
    IO_Init(pin, DEV_IO_INPUT);
    
    devAddr = (uint8_t)((gpio->IDR & addr_pins) >> 14); // set the address device
    
    if(devAddr == DEVICE_MDVV_01) // a device is MDVV-01
    {
        devID = 0x48;
    }
    else if(devAddr == DEVICE_MDVV_02) // a device is MDVV-02
    {
        devID = 0x49;
    }
    else if(devAddr == DEVICE_MIK_01) // a device is MIK-01
    {
        devID = 0x50;
    }
    
    //devAddr = 0x02; // для теста МИК
}
//---------------------------------------------------------------
void DEV_Init(PORT_Input_Type* inputs, PORT_Output_Type* outputs)
{
    io_in  = inputs;
    io_out = outputs;
    
    for(uint8_t i = 0; i < io_in->size; ++i)
    {
        if((devAddr == DEVICE_MIK_01 && i == 10) || 
           (devAddr == DEVICE_MIK_01 && i == 11))
        {
            // устройство МИК-01 и это 10 или 11 канал
            IO_Init(io_in->list[i].pin, DEV_IO_OUTPUT); // настраиваем как выход (сканирование)
            
            // устанавливаем на выходе лог "1"
            io_in->list[i].pin.gpio->ODR |= io_in->list[i].pin.io;
        }
        else
        {
            IO_Init(io_in->list[i].pin, DEV_IO_INPUT); // настраиваем как вход
            io_in->list[i].frequency = 0; // обнуляем переменную, которая сохраняет предыдущую частоту
        }
        
        io_in->list[i].pin.num = i;
    }
    
    for(uint8_t i = 0; i < io_out->size; ++i)
    {
        IO_Init(io_out->list[i].pin, DEV_IO_OUTPUT);
        io_out->list[i].pin.num = i;
        io_out->list[i].state   = OUTPUT_STATE_OFF;

        DEV_Out_Reset(&io_out->list[i]); // выключить выход - состояние по умолчанию
    }
    
    io_t pin_int = { GPIO_INT, GPIO_INT_PIN };
    IO_Init(pin_int, DEV_IO_OUTPUT); // вывод INT как выход
    
    GPIO_INT->BSRR |= GPIO_INT_SET; // включить выход INT (default state)
    
    DEV_Input_Set_Default();
    
    if(devAddr != DEVICE_MIK_01) // только для МДВВ
    {
        TIM_Scan_Init();
        
        if(devAddr == DEVICE_MDVV_01) // только для МДВВ-01
        {
            io_inPhase = &io_in->list[0]; // искробезопасный вход DI_1 для определения фазы
            io_inOff   = &io_in->list[1]; // искробезопасный вход DI_2 - кнопка СТОП
            io_inOn    = &io_in->list[2]; // искробезопасный вход DI_3 - кнопка СТАРТ
        }
    }
    else
    {
        EVENT_Create(5, false, DEV_Keyboard_Scan, NULL, 0xFF); // опрос клавиатуры
        
        queue_init(); // инициализация очереди входов для работы в режиме мигания
        
        EVENT_Create(1000, true, blink2Hz, NULL, 0xFF); // создание задачи мигания
    }
    
    TIM_INT_Init();
}
//--------------------------------------
void IO_Clock_Enable(GPIO_TypeDef* gpio)
{
    if(gpio == GPIOA)
        RCC->AHBENR |= RCC_AHBENR_GPIOAEN;
    else if(gpio == GPIOB)
        RCC->AHBENR |= RCC_AHBENR_GPIOBEN;
    else if(gpio == GPIOC)
        RCC->AHBENR |= RCC_AHBENR_GPIOCEN;
    else if(gpio == GPIOD)
        RCC->AHBENR |= RCC_AHBENR_GPIODEN;
    else if(gpio == GPIOF)
        RCC->AHBENR |= RCC_AHBENR_GPIOFEN;
}
//------------------------------------
void IO_Init(io_t pin, uint8_t io_dir)
{
    GPIO_InitTypeDef GPIO_InitStruct;
    
    IO_Clock_Enable(pin.gpio);
    
    /*Configure GPIO pins*/
    GPIO_InitStruct.Pin  = pin.io;
    GPIO_InitStruct.Mode = (io_dir == 0x01)?GPIO_MODE_OUTPUT_PP:GPIO_MODE_INPUT;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(pin.gpio, &GPIO_InitStruct);
}
//-----------------------------
void DEV_Out_Set(output_t* out)
{
    uint32_t io = ((uint32_t)out->pin.io);
    
    if(out->level == false) // включение выхода по лог "0"
    {
        io <<= 16; // смещаем влево на 2 байта для получения RESET (GPIO_BSRR_BRx)
    }
    
    out->pin.gpio->BSRR |= io;
}
//-------------------------------
void DEV_Out_Reset(output_t* out)
{
    uint32_t io = ((uint32_t)out->pin.io);
    
    if(out->level == true) // включение выхода по лог "1"
    {
        io <<= 16; // смещаем влево на 2 байта для получения RESET (GPIO_BSRR_BRx)
    }
    
    out->pin.gpio->BSRR |= io;
}
//--------------------------------
void DEV_Out_Toggle(output_t* out)
{
    if(DEV_Is_Out(out))
    {
        DEV_Out_Reset(out);
    }
    else
    {
        DEV_Out_Set(out);
    }
}
//----------------------------
bool DEV_Is_Out(output_t* out)
{
    bool state = (out->pin.gpio->ODR & out->pin.io);
    
    if(out->level == true) // выход управляется лог "1" (прямой)
    {
        return state;
    }
    
    return !state; // иначе управление инверсное, т.е. лог "0"
}
//---------------------------------
void CHANNEL_Out_Set(uint8_t index)
{
    if(index < io_out->size)
    {
        output_t* out = &io_out->list[index];

        DEV_Out_Set(out);
    }
}
//-----------------------------------
void CHANNEL_Out_Reset(uint8_t index)
{
    if(index < io_out->size)
    {
        output_t* out = &io_out->list[index];

        DEV_Out_Reset(out);
    }
}
//----------------------
void TIM_Scan_Init(void)
{
    RCC->APB2ENR |= RCC_APB2ENR_TIM16EN;
    
    TIM16->PSC   = F_CPU/1000000UL - 1;
    
    TIM_Scan_Update();
    
    TIM16->CR1  |= TIM_CR1_ARPE;
    TIM16->DIER |= TIM_DIER_UIE;
    TIM16->CR1  |= TIM_CR1_CEN;
    
    NVIC_EnableIRQ(TIM16_IRQn);
}
//------------------------
void TIM_Scan_Update(void)
{
    TIM16->ARR   = 10000/io_in->set.Ndiscret - 1;
    TIM16->DIER &= ~TIM_DIER_UIE;
    TIM16->EGR  |= TIM_EGR_UG;
    TIM16->DIER |= TIM_DIER_UIE;
}
//---------------------
void TIM_INT_Init(void)
{
    RCC->APB2ENR |= RCC_APB2ENR_TIM17EN;
    
    TIM17->PSC   = F_CPU/1000000UL - 1;
    TIM17->ARR   = 5000; // 5 ms сигнал INT
    TIM17->DIER |= TIM_DIER_UIE;
    TIM17->CR1  |= TIM_CR1_OPM;
    
    NVIC_EnableIRQ(TIM17_IRQn);
}
//----------------------
void TIM_INT_Start(void)
{
    GPIO_INT->BSRR |= GPIO_INT_RESET;
    TIM17->CR1 |= TIM_CR1_CEN;
}
//-----------------------
void DEV_PWROK_Init(void)
{
    RCC->AHBENR  |= RCC_AHBENR_GPIOAEN;
    RCC->APB2ENR |= RCC_APB2ENR_SYSCFGEN;
		RCC->APB1ENR |= RCC_APB1ENR_TIM14EN;
    
    GPIOA->MODER &= ~GPIO_MODER_MODER12;
    GPIOA->PUPDR &= ~GPIO_PUPDR_PUPDR12;
    GPIOA->PUPDR |= GPIO_PUPDR_PUPDR12_1; // pull down
    
    SYSCFG->EXTICR[3] &= ~SYSCFG_EXTICR4_EXTI12_PA;
    
    EXTI->IMR  |= EXTI_IMR_MR12;
    EXTI->RTSR |= EXTI_RTSR_TR12;
    
    EXTI->PR = EXTI_PR_PR12; // clear flag
	
    TIM14->PSC   = 48000 - 1; // 1ms
    TIM14->ARR   = 12 - 1;
    TIM14->CR1  |= TIM_CR1_ARPE | TIM_CR1_OPM;
    TIM14->DIER &= ~TIM_DIER_UIE;
    TIM14->EGR  |= TIM_EGR_UG;
    TIM14->SR   &= ~TIM_SR_UIF;
    TIM14->DIER |= TIM_DIER_UIE;
    
    NVIC_EnableIRQ(EXTI4_15_IRQn);
    NVIC_SetPriority(EXTI4_15_IRQn, 0);
	
    NVIC_EnableIRQ(TIM14_IRQn);
    NVIC_SetPriority(TIM14_IRQn, 0);
    
    TIM14->CR1 |= TIM_CR1_CEN;
}
//-----------------------
void DEV_Crash_Init(void)
{
    // настройка аварийного выхода
    out_crash        = &io_out->list[2];
    out_crash->param = EVENT_Create(5000, true, crash, out_crash, 0xFF);

    //DEV_Out_Set(out_crash); // включаем аварийный выход - только для теста
}
//-----------------------
uint8_t DEV_Address(void)
{
    return devAddr;
}
//------------------------------------------------------
bool DEV_Request(FS9Buffer_t* source, FS9Buffer_t* dest)
{
    is_crash = true; // получили запрос от ЦП - сброс аварийной ситуации
   
    ERROR_request_inc(); // увеличиваем счетчик запросов, если адрес устройства верный
    
    uint8_t checksum = DEV_Checksum(source, source->size - 1);
    
    if(checksum != (uint8_t)source->data[source->size - 1]) // ошибка контрольной суммы
    {
        ERROR_checksum_inc(); // увеличиваем счетчик ошибок контрольной суммы
        
        return false;
    }
    
    FS9Buffer_t data = { 0 };
    
    data.cmd      = dest->cmd = source->cmd;
    data.cmd_code = dest->cmd_code = source->cmd_code;
    data.size     = source->size - 2;
    
    if(data.size > 0)
    {
        memcpy(&data.data[0], &source->data[1], sizeof(data.data[0])*data.size);
    }
    
    bool answer = DEV_Driver(&data, dest);
    
    if(!answer && dest->cmd_code != 0x19) // если получили ложь и код команды не равен 0x19 (искробезопасные входы)
        return false; // ответа нет
    
    if(dest->cmd.is_ack)
    {
        if(dest->cmd_code == 0x19 && !answer) // если команда (запись искробезопасных входов) и ответ ложь
            dest->data[dest->size++] = NAK; // отправляем отказ
        else
            dest->data[dest->size++] = ACK; // в другом любом случае отправляем подтверждение
    }
    
    // append checksum for packet
    checksum = DEV_Checksum(dest, dest->size);
    dest->data[dest->size++] = checksum;
    
    return true;
}
//-----------------------------------------------------
bool DEV_Driver(FS9Buffer_t* source, FS9Buffer_t* dest)
{
    uint8_t bit_count = 0; // счетчик бит (позиция канала в байте)
    
    int32_t   temp  = 0x00;
    uint8_t   byte  = 0x00;
    uint8_t   state = 0x00;
    uint8_t   n_out = 0x00;
	uint16_t  time  = 0x0000;
    output_t* out   = NULL;
    
    uint8_t eeprom[15] = { 0x10, 0x20, 0x30, 0x40, 0x5F, 0x07, 0x32, 0x14, 0x02, 0x0A,
                           0x11, 0x21, 0x31, 0x41, 0x60}; // data write to eeprom - test
//    uint8_t eeprom2[5] = { 0x00, 0x00, 0x00, 0x00, 0x00 }; // buffer for read data from eeprom - test
    
    union
    {
        uint16_t count;
        uint8_t  byte[2];
    } utemp;
    
    utemp.count = 0x0000;
    
    switch(source->cmd_code)
    {
        case 0x00: // чтение дискретных каналов входов
            bit_count = 0;
        
            for(uint8_t i = 0; i < io_in->size; ++i)
            {
                if(bit_count == 8)
                {
                    bit_count = 0;
                    dest->index++;
                    dest->data[dest->index] = 0x00;
                }
                
                uint8_t  channel_state = 0x00;
                input_t* channel       = &io_in->list[i];
                
                if(channel->state == true && channel->error == false)
                {
                    // состояние канала входа активно и ошибок в канале нет
                    channel_state = 0x01; // сигнал на входе присутствует
                }
                else if(channel->error == true)
                {
                    // зафиксирована ошибка канала входа
                    channel_state = 0x02;
                    
                    // сбрасываем ошибку канала входа при чтении
                    channel->error = false;
                }
                
                dest->data[dest->index] |= channel_state << bit_count;
                
                bit_count += 2;
            }
            
            dest->size = 3;
        break;
            
        case 0x01: // чтение дискретных каналов выходов
            dest->data[0] = 0x00;
        
            for(uint8_t i = 0; i < io_out->size; ++i)
            {
                io_t pin = io_out->list[i].pin;
                
                if((pin.gpio->ODR & pin.io) == pin.io)
                {
                    dest->data[0] |= 0x01 << i;
                }
            }
            
            dest->size = 1;
        break;
            
        case 0x02: // чтение аналоговых величин 1..4
            while(AIN_Is_Ready() == false); // ожидание готовности результатов
            
            temp = AIN_Get_Temperature();
            
            union float_t t;
            
            if(devAddr == 0x00)
            {
                uint16_t vdda = AIN_Get_VDDA();
                
                t.number = ((float)(AIN_Get_Channel_1()/4095.0f*vdda/1000.0f));
            }
            else if(devAddr == 0x01)
            {
                uint16_t temp = ((float)AIN_Get_Channel_1()/4095)*AIN_TEMP[MAX_SIZE_AIN_TEMP - 1][1];
                t.number = Get_Temp(temp, 1);
            }
            else if(devAddr == 0x02)
            {
                t.number = 0.0f;
            }
            
            dest->data[0] = t.byte[0];
            dest->data[1] = t.byte[1];
            dest->data[2] = t.byte[2];
            dest->data[3] = t.byte[3];
        
            if(devAddr == 0x00)
            {
                uint16_t vdda = AIN_Get_VDDA();
                t.number = ((float)AIN_Get_Channel_2()/4095)*vdda/1000.0f; // шунт 0.1 Ом, усиление ОУ 20, делитель 2
                //t.number /= 0.1f;                         // т.е. значение пропорционально току
            }
            else if(devAddr == 0x01)
            {
                uint16_t temp = ((float)AIN_Get_Channel_2()/4095)*AIN_TEMP[MAX_SIZE_AIN_TEMP - 1][2];
                t.number = Get_Temp(temp, 2);
            }
            else if(devAddr == 0x02)
            {
                t.number = 0.0f;
            }
            
            dest->data[4] = t.byte[0];
            dest->data[5] = t.byte[1];
            dest->data[6] = t.byte[2];
            dest->data[7] = t.byte[3];
        
            t.number = temp/1000.0f;
        
            dest->data[8]  = t.byte[0];
            dest->data[9]  = t.byte[1];
            dest->data[10] = t.byte[2];
            dest->data[11] = t.byte[3];

            if(devAddr == 0x00)
            {
                t.number = DS18B20_Temperature();
            }
            else
            {
                t.number = 0.0f;
            }
            
            dest->data[12] = t.byte[0];
            dest->data[13] = t.byte[1];
            dest->data[14] = t.byte[2];
            dest->data[15] = t.byte[3];
        
            dest->size = 16;
        break;
            
        case 0x03: // чтение регистра расширения дискретных каналов входов
            dest->data[0] = keys.last_state&0x000000FF;
            dest->data[1] = (keys.last_state >> 8)&0x000000FF;
            dest->data[2] = (keys.last_state >> 16)&0x0000000F;
            
            dest->size = 3;
        break;
            
        case 0x04: // чтение регистра расширения дискретных каналов выходов
            for(uint8_t i = 0; i < io_out->size; ++i)
            {
                out = &io_out->list[i];
                
                if(bit_count == 8)
                {
                    bit_count = 0;
                    dest->data[++dest->index] = 0x00;
                }
                
                uint8_t channel_state = 0x00;
                
                if(out->state == OUTPUT_STATE_ON)
                {
                    channel_state = 0x01;
                }
                else if(out->state == OUTPUT_STATE_FREQ_2HZ)
                {
                    channel_state = 0x02;
                }
                else if(out->state == OUTPUT_STATE_RESERVE)
                {
                    channel_state = 0x03;
                }
                
                dest->data[dest->index] |= channel_state << bit_count;
                
                bit_count += 2;
            }
                
            dest->size = 3;
        break;
            
        case 0x05: // запись регистра расширения дискретных каналов выходов
            for(uint8_t i = 0; i < source->size; ++i)
            {
                byte = source->data[i]; // текущий байт
                
                for(uint8_t j = 0; j < 8; j += 2) // 8 бит по 2 на описание каждого канала
                {
                    state = (byte >> j)&0x03; // состояние текущего канала
                    n_out = i*4 + j/2; // порядковый номер канала
                    out   = &io_out->list[n_out];
                    
                    switch(state)
                    {
                        case OUTPUT_STATE_OFF: // отключение выхода
                            if(out->state == OUTPUT_STATE_OFF)
                            {
                                break;
                            }
                            else if(out->state == OUTPUT_STATE_FREQ_2HZ || out->state == OUTPUT_STATE_RESERVE)
                            {
                                kill_task(n_out);
                            }
                            
                            out->state = OUTPUT_STATE_OFF;

                            DEV_Out_Reset(out);
                        break;
                        
                        case OUTPUT_STATE_ON: // включение выхода
                            if(out->state == OUTPUT_STATE_ON)
                            {
                                break;
                            }
                            else if(out->state == OUTPUT_STATE_FREQ_2HZ || out->state == OUTPUT_STATE_RESERVE)
                            {
                                kill_task(n_out);

                                DEV_Out_Reset(out);
                            }
                            
                            out->state = OUTPUT_STATE_ON;
                            
                            DEV_Out_Set(out);
                        break;
                        
                        case OUTPUT_STATE_FREQ_2HZ: // включение выхода с альтернативной функцией
                        case OUTPUT_STATE_RESERVE:
                            if(out->state == OUTPUT_STATE_FREQ_2HZ || out->state == OUTPUT_STATE_RESERVE)
                            {
                                break;
                            }
                            else if(out->state == OUTPUT_STATE_ON)
                            {
                                DEV_Out_Reset(out);
                            }
                            
                            out->state = OUTPUT_STATE_FREQ_2HZ;
                            insert_task(n_out);
                        break;
                    }
                }
            }
        break;
        
        case 0x06: // установка значения 0 на выходе канала 0
            CHANNEL_Out_Reset(0);
        break;
            
        case 0x07: // установка значения 0 на выходе канала 1
            CHANNEL_Out_Reset(1);
        break;
            
        case 0x08: // установка значения 0 на выходе канала 2
            CHANNEL_Out_Reset(2);
        break;
            
        case 0x09: // установка значения 0 на выходе канала 3
            CHANNEL_Out_Reset(3);
        break;
            
        case 0x0A: // установка значения 0 на выходе канала 4
            CHANNEL_Out_Reset(4);
        break;
            
        case 0x0B: // установка значения 0 на выходе канала 5
            CHANNEL_Out_Reset(5);
        break;
            
        case 0x0C: // установка значения 0 на выходе канала 6
            CHANNEL_Out_Reset(6);
        break;
            
        case 0x0D: // установка значения 0 на выходе канала 7
            CHANNEL_Out_Reset(7);
        break;
            
        case 0x0E: // установка значения 1 на выходе канала 0
            CHANNEL_Out_Set(0);
        break;
            
        case 0x0F: // установка значения 1 на выходе канала 1
            CHANNEL_Out_Set(1);
        break;
            
        case 0x10: // установка значения 1 на выходе канала 2
            CHANNEL_Out_Set(2);
        break;
            
        case 0x11: // установка значения 1 на выходе канала 3
            CHANNEL_Out_Set(3);
        break;
            
        case 0x12: // установка значения 1 на выходе канала 4
            CHANNEL_Out_Set(4);
        break;
            
        case 0x13: // установка значения 1 на выходе канала 5
            CHANNEL_Out_Set(5);
        break;
            
        case 0x14: // установка значения 1 на выходе канала 6
            CHANNEL_Out_Set(6);
        break;
            
        case 0x15: // установка значения 1 на выходе канала 7
            CHANNEL_Out_Set(7);
        break;
        
        case 0x18: // чтение байта конфигурации искробезопасных входов
            if(devAddr != DEVICE_MDVV_01) // если устройство не МДВВ-01,
            {
                dest->data[0] = SPARK_SECURITY_MODE_NONE; // то возвращаем ответ - нет искробезопасных входов
            }
            else
            {
                dest->data[0] = io_in->list[0].spark_security; // то возвращаем текущую настройку первого входа
                                                                   // остальные настроены однотипно (блок)
            }
            
            dest->size = 1;
        break;
        
        case 0x19: // запись байта конфигурации искробезопасных входов
            if(devAddr == DEVICE_MDVV_01 && source->size == 1) // настройка искробезопасных входов только для МДВВ-01
            {
                if((int8_t)source->data[0] >= SPARK_SECURITY_MODE_NONE && (int8_t)source->data[0] <= SPARK_SECURITY_MODE_3)
                {
                    // присваиваем новое значение режима для четырех искробезопасных входов
                    io_in->list[0].spark_security = io_in->list[1].spark_security = io_in->list[2].spark_security =
                    io_in->list[3].spark_security = source->data[0];
                    
                    // в режимах №2 и №3 частота входного сигнала 50Гц
                    if(source->data[0] == SPARK_SECURITY_MODE_2 || source->data[0] == SPARK_SECURITY_MODE_3)
                        io_in->list[0].duration = io_in->list[1].duration = io_in->list[2].duration = io_in->list[3].duration = 20;
                    else
                        io_in->list[0].duration = io_in->list[1].duration = io_in->list[2].duration = io_in->list[3].duration = 10;
                }
                else
                    return false;
            }
            else
                return false;
        break;
            
        case 0x1D: // чтение отладочной информации (счетчиков ошибок)
            utemp.count = ERROR_request();; // чтение счетчика количества запросов
            
            dest->data[0] = utemp.byte[0];
            dest->data[1] = utemp.byte[1];
            
            utemp.count = ERROR_command(); // чтение счетчика ошибок команд
            
            dest->data[2] = utemp.byte[0];
            dest->data[3] = utemp.byte[1];
            
            utemp.count = ERROR_checksum(); // чтение счетчика ошибок контрольной суммы
            
            dest->data[4] = utemp.byte[0];
            dest->data[5] = utemp.byte[1];
        
            utemp.count = ERROR_no_process(); // чтение счетчика ошибок отсутствия обработчика команды
            
            dest->data[6] = utemp.byte[0];
            dest->data[7] = utemp.byte[1];
        
            utemp.count = ERROR_overrun(); // чтение счетчика ошибок переполнения
            
            dest->data[8] = utemp.byte[0];
            dest->data[9] = utemp.byte[1];
            
            utemp.count = ERROR_timeout(); // чтение счетчика ошибок таймаута
            
            dest->data[10] = utemp.byte[0];
            dest->data[11] = utemp.byte[1];
        
            // структура инициализируется при передаче нулями, поэтому нет смысла обнулять резервные ячейки
            
            dest->size = 16;
        break;
        
        case 0x1E:
            dest->data[0] = devID;
            dest->data[1] = DEVICE_NUMBER&0x00FF;
            dest->data[2] = DEVICE_NUMBER&0xFF00;
            dest->data[3] = DEVICE_LOT;
            dest->data[4] = DEVICE_FIRMWARE_VARIANT;
            dest->data[5] = (DEVICE_FIRMWARE_DATE&0x00FF0000) >> 16; // year
            dest->data[6] = (DEVICE_FIRMWARE_DATE&0x0000FF00) >> 8; // month
            dest->data[7] = DEVICE_FIRMWARE_DATE&0x000000FF; // day
        
            dest->size = 8;
        break;
            
        case 0x1F: // чтение времени срабатывания выделенного входного дискретного канала                
			if(_pwr_ok.is_crash == true)
			{
				uint8_t state = DSDIN_TRIGGER_ON_0;
				
				if(_pwr_ok.IN_change == true)
				{
					state = (_pwr_ok.IN_state == true)?DSDIN_TRIGGER_ON_1:DSDIN_TRIGGER_ON_0;
					time  = _pwr_ok.IN_time;
				}
				else
				{
					state = (io_in->list[PWROK_INPUT].state == true)?DSDIN_TRIGGER_ON_1:
						                                             DSDIN_TRIGGER_ON_0;
				}
				
				_pwr_ok.is_crash  = false;
				_pwr_ok.IN_change = false;
				_pwr_ok.IN_state  = false;
				_pwr_ok.IN_time   = 0x0000;
				
				dest->data[0] = state;
			}
			else
			{
				dest->data[0] = DSDIN_TRIGGER_OFF;
			}
			
			dest->data[1] = (uint8_t)(time&0x00FF);
			dest->data[2] = (uint8_t)((time&0xFF00) >> 8);
            
            dest->size = 3;
        break;

        case 0x3B: // чтение из памяти (тест eeprom и flash)
            //return I2C_EE_ReadBytes(0xA0, 0x00, eeprom2, 5);
            FLASH_Unlock();
            FLASH_Erase(FLASH_BASE_ADDRESS);
            FLASH_Lock();
        break;
            
        case 0x3C: // запись в память (тест eeprom и flash)
            //return I2C_EE_WriteBytes(0xA0, 0x00, eeprom, 5);
            FLASH_WriteBlock(eeprom, 15);
        break;
        
        case 0x3E: // изменение параметров фильтрации
            if(source->size == 3)
            {
                io_in->set.Nperiod  = source->data[0]; // количество периодов фильтрации
                io_in->set.Ndiscret = source->data[1]; // количество выборок на период
                io_in->set.SGac     = source->data[2]; // длительность сигнала считаемая, что сигнал валидный
                
                TIM_Scan_Update();
            }
        break;
            
        case 0x3F: // изменение входа
            if(source->size == 4)
            {
                uint8_t in_num = source->data[0]; // номер настраиваемого входа
                io_in->list[in_num].mode  = source->data[1]; // режим работы входа AC или DC
              
                io_in->list[in_num].duration = source->data[2]; // длительность периода
                
                if(io_in->list[in_num].mode == IN_MODE_AC)
                {
                    io_in->list[in_num].fault = source->data[3]; // погрешность допускаемая за один период - в процентах
                }
                else
                {
                    io_in->set.P0dc = io_in->set.P1dc = source->data[3];
                }
            }
        break;
        
        default:
        {
            ERROR_no_process_inc(); // увеличиваем счетчик - нет обработчика команды
            return false;
        }
    };
    
    return true;
}
//-----------------------------------------------------
uint8_t DEV_Checksum(FS9Buffer_t* packet, uint8_t size)
{
    uint8_t checksum = 0;
    
    for(uint8_t i = 0; i < size; ++i)
    {
        checksum += (uint8_t)(packet->data[i]&0x00FF);
    }
    
    checksum += size;
    checksum ^= 0xFF;
    
    return checksum;
}
//-----------------------
void DEV_Input_Scan(void)
{    
    if(_pwr_ok.state == false)
    {
        for(uint8_t i = 0; i < io_in->size; ++i)
        {
            DEV_Input_Filter(i);
        }
    }
    else
    {
        DEV_Input_Filter(4);
    }
    
    if(Input_Changed)
    {
        TIM_INT_Start();
    }
}
//----------------------------------
void DEV_Input_Filter(uint8_t index)
{
    input_t* input = &io_in->list[index];
    
    bool in_state  = input->pin.gpio->IDR & input->pin.io;
    bool act_level = !input->state; // ожидаемый уровень (при выключенном входе - лог "1", при включенном лог "0")
    
    // если уровень на входе равен ожидаемому уровню (обратное значение от состояния входа)
    if(in_state == act_level && input->filter.is_capture == false)
    {
        input->filter.is_capture = true; // захватываем вход
        
        if(in_state == true)
            input->filter.c_lev_1++;
        else
            input->filter.c_lev_0++;
        
        input->filter.c_clock++;
    }
    else if(input->filter.is_capture == true) // если вход захвачен, то набираем данные для фильтрации
    {
        if(in_state == true)
            input->filter.c_lev_1++;
        else
            input->filter.c_lev_0++;
        
        input->filter.c_clock++;
        
        if(input->filter.c_clock >= input->duration) // набрали данные на очередной период
        {
            input->filter.c_period++;
            
            if(input->mode == IN_MODE_AC) // режим входа АС
            {
                // если ожидаемый уровень лог "1" и количество лог "0" равно нулю, то это либо постоянный
                // сигнал, либо сигнал с частотой меньше установленно, т.е. это ошибка
                if(act_level == true && input->filter.c_lev_0 == 0)
                {
                    input->filter.c_error++;

                    input->filter.c_clock = 0;
                    input->filter.c_lev_0 = 0;
                    input->filter.c_lev_1 = 0;

                    return;
                }
                
                // если ожидаемый уровень лог "1" и длительность сигнала меньше пороговой, то это ошибка
                if(act_level == true && input->filter.c_lev_1 < io_in->set.SGac)
                {
                    input->filter.c_error++;

                    input->filter.c_clock = 0;
                    input->filter.c_lev_0 = 0;
                    input->filter.c_lev_1 = 0;

                    return;
                }
                
                // если ожидаемый уровень лог "0" (снятие сигнала со входа) и длительность лог "1"
                // больше нуля, то значит это не снятие сигнала и нет смысла дальше анализировать
                if(act_level == false && input->filter.c_lev_1 > 0 && (input->spark_security != SPARK_SECURITY_MODE_2 || 
                                                                       input->spark_security != SPARK_SECURITY_MODE_3))
                {
                    input->filter.c_clock    = 0;
                    input->filter.c_error    = 0;
                    input->filter.c_lev_0    = 0;
                    input->filter.c_lev_1    = 0;
                    input->filter.c_period   = 0;
                    input->filter.c_state    = 0;
                    input->filter.is_capture = false;
                    
                    return;
                }
                
                uint16_t tdur = (act_level == true)?(input->filter.c_lev_0 + input->filter.c_lev_1):
                                                     input->filter.c_lev_0;
                
                // определяем количество тиков до перезагрузки таймера сканирования, т.е. чему равно одно прерывание
                // процессор настроен на 48 МГц, т.е. таймер сканирования считает 1 тик = 1 мкс
                uint16_t tick_count = 10000/io_in->set.Ndiscret; // количество мкс до перезагрузки таймера, н-р: дискретность равна 10, тогда 1000 = 1мс
                
                // расчет частоты входного сигнала исходя из счетчиков нулей и единиц (длительность сигнала tdur)
                uint16_t frequency = tick_count/tdur; // н-р: единиц и нулей по 5, тогда 1000/10 = 100Гц
                uint16_t fault     = (tick_count/io_in->set.Ndiscret)*input->fault/100; // погрешность частоты, н-р: частота 100Гц, 
                                                                                        // погрешность 10% - 100*10/100 = 10Гц
                
                input->frequency = frequency; // обновляем переменную предыдущего значения частоты сигнала
                
                if(input->spark_security == SPARK_SECURITY_MODE_NONE || input->spark_security == SPARK_SECURITY_MODE_1)
                {
                    // обрабатываются не искробезопасные входы, либо искробезопасные в режиме №1
                    if(frequency >= (100 - fault) && frequency <= (100 + fault)) // частота в пределах 100Гц
                        input->filter.c_state++;
                    else
                        input->filter.c_error++;
                }
                else if(input->spark_security == SPARK_SECURITY_MODE_2 || input->spark_security == SPARK_SECURITY_MODE_3)
                {
                    /*! обработка искробезопасных входов в режиме №2 и №3 (сигнал на входе 50Гц)
                     *  Алгоритм работает следующим образом:
                     *  Вход ON фиксирует входной сигнал только в том случае, если на входе DI_1 и DI_3 (ПУСК) присутсвтвует сигнал 50Гц в противофазе
                     *  Состояние входа DI_3 (ПУСК) считается ошибкой, если сигнал находится в фазе с сигналом на входе DI_1 (прямое включение диода)
                     *  Состосние входа DI_2 (СТОП) считается ошибкой, если сигнал 50Гц на входе DI_1 присутствует, а налинии DI_2 нет (обрыв линии)
                     *  Состояние на любом из искробезопасных входов DI_1 - DI_4 является ошибкой, если частота сигнала меньше или больше 50Гц
                     */
                    
                    // мгновенные значения состояний входов DI_1, DI_2 (СТОП) и DI_3 (ПУСК)
                    bool io_phaseState = io_inPhase->pin.gpio->IDR & io_inPhase->pin.io;
                    bool io_offState   = io_inOff->pin.gpio->IDR & io_inOff->pin.io;
                    bool io_onState    = io_inOn->pin.gpio->IDR & io_inOn->pin.io;
                    
                    if((frequency >= (50 - fault) && frequency <= (50 + fault)) == false) // частота меньше или больше 50Гц с учетом погрешности
                    {
                        input->filter.c_error++;
                    }
                    else
                    {
                        if(input == io_inOn)
                        {
                            if(!input->state && io_onState && (!io_phaseState && !io_offState) && (io_inPhase->state && io_inOff->state))
                                input->filter.c_state++;
                            else if(input->state && !io_onState)
                                input->filter.c_state++;
                            else if(io_onState && io_offState && io_phaseState && input->spark_security == SPARK_SECURITY_MODE_2)
                            {
                                // ошибка включения диода на линии ON (должен быть влкючен в обратном) - только для режим №2 (в режиме №3 норма)
                                input->filter.c_error++;
                            }
                        }
                        else
                            input->filter.c_state++;
                    }
                }
            }
            else if(input->mode == IN_MODE_DC) // режим входа DC
            {
                uint16_t tfault_lev = (act_level == true)?input->duration*io_in->set.P1dc:
                                                          input->duration*io_in->set.P0dc;
                uint16_t tdur = (act_level == true)?input->filter.c_lev_1:input->filter.c_lev_0;
                
                if(tdur >= tfault_lev)
                    input->filter.c_state++;
            }
            
            input->filter.c_clock = 0;
            input->filter.c_lev_0 = 0;
            input->filter.c_lev_1 = 0;
        }
        
        if(input->filter.c_period >= io_in->set.Nperiod) // конец фильтрации - принятие решения
        {
            if(input->filter.c_state >= (io_in->set.Nperiod - 1))
            {
                // обработка состояния ошибки для искробезопасного входа в режиме 2 и №3
                if(input == io_inOff && !act_level && io_inPhase->state && (input->spark_security == SPARK_SECURITY_MODE_2 || 
                                                                            input->spark_security == SPARK_SECURITY_MODE_3))
                {
                    input->error  = true;
                    input->state  = false;
                    Input_Changed = true;
                }
                else
                {
                    input->error  = false;
                    input->state  = act_level;
                    Input_Changed = true;
                    
                    if(_pwr_ok.state == true && index == PWROK_INPUT)
                    {
                        if(_pwr_ok.IN_change == false)
                        {
                            _pwr_ok.IN_state  = input->state;
                            _pwr_ok.IN_time   = TIM14->CNT;
                            _pwr_ok.IN_change = true;
                        }
                    }
                }
            }
            else if(input->filter.c_error >= io_in->set.Nperiod)
            {
                input->error  = true;
                Input_Changed = true;
            }
            
            input->filter.c_clock    = 0;
            input->filter.c_error    = 0;
            input->filter.c_lev_0    = 0;
            input->filter.c_lev_1    = 0;
            input->filter.c_period   = 0;
            input->filter.c_state    = 0;
            input->filter.is_capture = false;
        }
    }
}
//--------------------------------
void DEV_Keyboard_Scan(void* data)
{
    io_t scan;
    
    switch(keys.mode)
    {
        case KEY_MODE_NONE:
            scan = io_in->list[10].pin; // первая сканлиния
            scan.gpio->BSRR |= scan.io << 16; // прижимаем первую сканлинию
        
            keys.mode = KEY_MODE_SCAN_1;
        
            EVENT_Create(5, false, DEV_Keyboard_Scan, NULL, 0xFF);
        break;
        
        case KEY_MODE_SCAN_1:
            scan  = io_in->list[10].pin;
            keys.temp = scan.gpio->IDR;
            keys.temp &= 0x000003FF; // считываем состояние кнопок
            
            scan.gpio->BSRR |= scan.io; // поднимаем первую сканлинию
        
            scan = io_in->list[11].pin; // вторая сканлиния
            scan.gpio->BSRR |= scan.io << 16; // прижимаем вторую сканлинию
        
            keys.mode = KEY_MODE_SCAN_2;
        
            EVENT_Create(5, false, DEV_Keyboard_Scan, NULL, 0xFF);
        break;
        
        case KEY_MODE_SCAN_2:
            scan = io_in->list[11].pin;
            keys.temp |= (scan.gpio->IDR&0x000003FF) << 10; // считываем состояние кнопок
            scan.gpio->BSRR |= scan.io; // поднимаем вторую сканлинию
        
            if(keys.is_bounce == false)
            {
                keys.is_bounce = true;
                keys.cur_state = keys.temp;
            }
            else
            {
                if(keys.cur_state == keys.temp)
                {
                    keys.temp ^= KEY_EMPTY_MASK; // инвертируем значение
                    keys.temp &= KEY_EMPTY_MASK; // обрезаем по маске (до 20-ти значащих бит)
                    
                    if(keys.temp != keys.last_state) // предыдущее состояние не равно текущему (произошли изменения)
                    {
                        keys.last_state = keys.temp; // сохраняем текущее состояние входов
                        
                        Input_Changed = true; // устанавливаем сигнал INT для оповещении ЦП об изменении состояния входов
                    }
                }
                
                keys.is_bounce = false;
                keys.cur_state = KEY_EMPTY_MASK;
            }
            
            keys.temp = KEY_EMPTY_MASK;
            keys.mode = KEY_MODE_NONE;
            
            EVENT_Create(5, false, DEV_Keyboard_Scan, NULL, 0xFF);
        break;
    }
    
    if(Input_Changed == true)
    {
        TIM_INT_Start();
    }
}
//------------------------------
void DEV_Input_Set_Default(void)
{
    io_in->set.Ndiscret = 10;
    io_in->set.Nperiod  = 3;
    io_in->set.SGac     = 5;
    io_in->set.P0dc     = 50;
    io_in->set.P1dc     = 50;
    
    for(uint8_t i = 0; i < io_in->size; ++i)
    {
        io_in->list[i].mode              = IN_MODE_AC;
        io_in->list[i].fault             = 10;
        io_in->list[i].state             = false;
        io_in->list[i].error             = false;
        io_in->list[i].filter.c_clock    = 0;
        io_in->list[i].filter.c_period   = 0;
        io_in->list[i].filter.c_state    = 0;
        io_in->list[i].filter.c_error    = 0;
        io_in->list[i].filter.is_capture = false;
        
        if(io_in->list[i].spark_security == SPARK_SECURITY_MODE_2 || io_in->list[i].spark_security == SPARK_SECURITY_MODE_3)
            io_in->list[i].duration = 20;
        else
            io_in->list[i].duration = 10;
    }
}
//----------------------------------
bool DEV_Input_Changed_Channel(void)
{
    return Input_Changed;
}
//----------------------------
void EXTI4_15_IRQHandler(void)
{
    if((EXTI->PR & EXTI_PR_PR12) == EXTI_PR_PR12)
    {
        _pwr_ok.is_ok = true;
		
		if(_pwr_ok.state == true)
		{
			_pwr_ok.state = false;
			
			TIM14->DIER &= ~TIM_DIER_UIE;
			TIM14->ARR   = 12 - 1;
			TIM14->EGR  |= TIM_EGR_UG;
			TIM14->SR   &= ~TIM_SR_UIF;
			TIM14->DIER |= TIM_DIER_UIE;
			TIM14->CR1  |= TIM_CR1_CEN;
		}
               
        EXTI->PR = EXTI_PR_PR12; // clear flag
    }
}
//-------------------------
void TIM14_IRQHandler(void)
{
	if((TIM14->SR & TIM_SR_UIF) == TIM_SR_UIF)
	{
		TIM14->SR &= ~TIM_SR_UIF;
		
		if(_pwr_ok.state == false)
		{
			if(_pwr_ok.is_ok == false)
			{
				_pwr_ok.state = _pwr_ok.is_crash = true;
				
				TIM14->DIER &= ~TIM_DIER_UIE;
				TIM14->ARR   = PWROK_TIME - 1;
				TIM14->EGR  |= TIM_EGR_UG;
				TIM14->SR   &= ~TIM_SR_UIF;
				TIM14->DIER |= TIM_DIER_UIE;
				TIM14->CR1  |= TIM_CR1_CEN;
				
				TIM_INT_Start();
			}
			
			_pwr_ok.is_ok = false;
			
			TIM14->CR1 |= TIM_CR1_CEN;
		}
	}
}
//-------------------------
void TIM16_IRQHandler(void)
{
    if((TIM16->SR & TIM_SR_UIF) == TIM_SR_UIF)
    {
        DEV_Input_Scan();
        
        TIM16->SR &= ~TIM_SR_UIF;
    }
}
//-------------------------
void TIM17_IRQHandler(void)
{
    if((TIM17->SR & TIM_SR_UIF) == TIM_SR_UIF)
    {
        GPIO_INT->BSRR |= GPIO_INT_SET; // отключить выход INT
        
        Input_Changed = false;
        
        TIM17->SR &= ~TIM_SR_UIF;
    }
}
//------------------------------------------
float Get_Temp(uint16_t val, uint8_t in_num)
{
    float Rt    = UAIN_to_TResistance(val, in_num);
    float Pt100 = 3383.8098f - 8658.0088f*sqrtf(0.1758481f - 0.000231f*Rt);
    
    return Pt100;
}
//-----------------------------------------------------
float UAIN_to_TResistance(uint16_t val, uint8_t in_num)
{
    // значение val приходит умноженное на 10 (значение ацп целочисленное, т.е. ацп*1000), т.к. в таблице значения
    // хранятся умноженные на 10000 для приведения к целочисленному виду
    float res_beg = 0;
    float res_end = 0;
    float ain_beg = 0;
    float ain_end = 0;
    
    for(uint8_t i = 1; i < 19; ++i)
    {
        if(AIN_TEMP[i - 1][in_num] == val)
            return AIN_TEMP[i - 1][0];
        else if(val > AIN_TEMP[i - 1][in_num] && val < AIN_TEMP[i][in_num])
        {
            res_beg = AIN_TEMP[i - 1][0];
            res_end = AIN_TEMP[i][0];
            ain_beg = AIN_TEMP[i - 1][in_num];
            ain_end = AIN_TEMP[i][in_num];
            
            break;
        }
    }
    
    if(res_beg == 0 && res_end == 0) // not equals value
    {
        return AIN_TEMP[MAX_SIZE_AIN_TEMP - 1][0]; // return max value
    }
    
    float Rt = res_beg + ((val - ain_beg)/(ain_end - ain_beg))*(res_end - res_beg);
    
    return Rt;
}
//-------------------------
void blink2Hz(void* output)
{    
    if(out_queue_blink.count == 0)
        return;
    
    for(uint8_t i = 0; i < MAX_SIZE_QUEUE_OUT; ++i)
    {
        if(out_queue_blink.queue[i] != 0xFF)
        {
            output_t* out = &io_out->list[out_queue_blink.queue[i]];
            
            if(out_queue_blink.state == true)
            {
                DEV_Out_Set(out);
            }
            else
            {
                DEV_Out_Reset(out);
            }
        }
    }
    
    out_queue_blink.state = !out_queue_blink.state;
}
//----------------------
void crash(void* output)
{
    output_t* out = ((output_t*)output);
    
    if(is_crash == true) // запрос пришел
    {
        is_crash = false;
    }
    else // запроса нет - отключаем выход
    {
        DEV_Out_Reset(out);
    }
}
//--------------------
void  queue_init(void)
{
    for(uint8_t i = 0; i < MAX_SIZE_QUEUE_OUT; ++i)
    {
        out_queue_blink.queue[i] = 0xFF;
    }

    out_queue_blink.state = false;
    out_queue_blink.count = 0;
}
//--------------------------
void insert_task(uint8_t id)
{
    if(out_queue_blink.count < MAX_SIZE_QUEUE_OUT)
    {
        out_queue_blink.queue[id] = id;
        out_queue_blink.count++;
    }
}
//------------------------
void kill_task(uint8_t id)
{
    if(out_queue_blink.count < MAX_SIZE_QUEUE_OUT)
    {
        out_queue_blink.queue[id] = 0xFF;
        out_queue_blink.count--;
    }
}
