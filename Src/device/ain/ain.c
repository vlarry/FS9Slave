#include "ain.h"
//-----------------------------------
const uint8_t MAX_NUM_CHANNELS   = 4; // максимальное количество каналов
const uint8_t MAX_NUM_CONVERSION = 10; // максимальное количество
                                       // преобразований (усреднение)
//-----------------------------------------------
volatile uint16_t AIN_channels[MAX_NUM_CHANNELS];
volatile uint16_t AIN_buffer[MAX_NUM_CHANNELS];
volatile uint16_t AIN_result[MAX_NUM_CHANNELS];
volatile uint8_t  conv_count = 0;
volatile bool     AIN_is_eoc = false;
volatile uint16_t VDDA = 0;
volatile uint8_t  ADDR = 0xFF; // адрес устройства
//-------------------------
void AIN_Init(uint8_t addr)
{
    ADDR = addr;
    
    RCC->AHBENR |= RCC_AHBENR_DMA1EN; // тактирование порта B и DMA
    RCC->APB2ENR |= RCC_APB2ENR_ADC1EN; // тактирование АЦП
 
    uint8_t num_channels = MAX_NUM_CHANNELS/2;
    
    if(ADDR == 0x00 || ADDR == 0x01)
    {
        RCC->AHBENR  |= RCC_AHBENR_GPIOBEN;    
        GPIOB->MODER |= GPIO_MODER_MODER0 | GPIO_MODER_MODER1; // вывод 0 и 1 как аналоговые входы
        
        num_channels = MAX_NUM_CHANNELS;
    }
    
    if((ADC1->CR & ADC_CR_ADEN) == ADC_CR_ADEN)
        ADC1->CR |= ADC_CR_ADDIS;
    
    while((ADC1->CR & ADC_CR_ADEN) != 0);
    
    ADC1->CFGR1 &= ~ADC_CFGR1_DMAEN;
    
    ADC1->CR |= ADC_CR_ADCAL;
    
    while((ADC1->CR & ADC_CR_ADCAL) != 0); // ожидание окончания калибровки
    
    if((ADC1->ISR & ADC_ISR_ADRDY) != 0)
        ADC1->ISR |= ADC_ISR_ADRDY;
    
    ADC1->CR  |= ADC_CR_ADEN;
    
    while((ADC1->ISR & ADC_ISR_ADRDY) == 0);
    
    uint32_t channels = ADC_CHSELR_CHSEL16 | ADC_CHSELR_CHSEL17;
    
    if(ADDR == 0x00 || ADDR == 0x01)
    {
        channels |= ADC_CHSELR_CHSEL8 | ADC_CHSELR_CHSEL9;
    }
    
    ADC1->CHSELR |= channels;
    ADC1->SMPR   |= ADC_SMPR1_SMPR_0 | ADC_SMPR1_SMPR_1 | ADC_SMPR1_SMPR_2; // 239.5 + 12.5 = ~18us
    ADC1->CFGR1  |= ADC_CFGR1_CONT;
    ADC->CCR     |= ADC_CCR_TSEN | ADC_CCR_VREFEN;
    ADC1->CFGR1  |= ADC_CFGR1_DMAEN | ADC_CFGR1_DMACFG;
    
    DMA1_Channel1->CPAR   = (uint32_t)(&(ADC1->DR));
    DMA1_Channel1->CMAR   = (uint32_t)(AIN_channels);
    DMA1_Channel1->CNDTR  = num_channels;
    DMA1_Channel1->CCR   |= DMA_CCR_MINC | DMA_CCR_MSIZE_0 | DMA_CCR_PSIZE_0 | DMA_CCR_TCIE | DMA_CCR_CIRC;
    DMA1_Channel1->CCR   |= DMA_CCR_EN;
    
    NVIC_EnableIRQ(DMA1_Ch1_IRQn);
    NVIC_SetPriority(DMA1_Ch1_IRQn, 0);
    
    ADC1->CR |= ADC_CR_ADSTART;
}
//---------------------
bool AIN_Is_Ready(void)
{
    return AIN_is_eoc;
}
//-------------------------------
int32_t AIN_Get_Temperature(void)
{
    uint32_t result = (uint32_t)AIN_result[0];
    
    if(ADDR == 0x00 || ADDR == 0x01)
    {
        result = (uint32_t)AIN_result[2];
    }
    
    int32_t temp = ((uint32_t)*TEMP30_CAL_ADDR - (result*VDDA/3300))*1000;
    temp = (temp/4300.0f + 30.0f)*1000;
    
    return temp;
}
//------------------------------
uint16_t AIN_Get_Channel_1(void)
{
    return AIN_result[0];
}
//------------------------------
uint16_t AIN_Get_Channel_2(void)
{
    return AIN_result[1];
}
//----------------------------
void DMA1_Ch1_IRQHandler(void)
{
    if((DMA1->ISR & DMA_ISR_TCIF1) == DMA_ISR_TCIF1)
    {
        DMA1_Channel1->CCR &= ~DMA_CCR_TCIE; // отключаем прерывания DMA
        
        if(ADDR == 0x00 || ADDR == 0x01)
        {
            AIN_buffer[0] += AIN_channels[0];
            AIN_buffer[1] += AIN_channels[1];
            AIN_buffer[2] += AIN_channels[2];
            AIN_buffer[3] += AIN_channels[3];
        }
        else if(ADDR == 0x02)
        {
            AIN_buffer[0] += AIN_channels[0];
            AIN_buffer[1] += AIN_channels[1];
        }
        
        conv_count++;
        
        if(conv_count == MAX_NUM_CONVERSION)
        {
            AIN_is_eoc = false; // снимаем флаг готовности данных (на время сохранения новых)
            
            if(ADDR == 0x00 || ADDR == 0x01)
            {
                AIN_result[0] = AIN_buffer[0]/MAX_NUM_CONVERSION;
                AIN_result[1] = AIN_buffer[1]/MAX_NUM_CONVERSION;
                AIN_result[2] = AIN_buffer[2]/MAX_NUM_CONVERSION;
                AIN_result[3] = AIN_buffer[3]/MAX_NUM_CONVERSION;
            }
            else if(ADDR == 0x02)
            {
                AIN_result[0] = AIN_buffer[0]/MAX_NUM_CONVERSION;
                AIN_result[1] = AIN_buffer[1]/MAX_NUM_CONVERSION;
            }
            
            AIN_buffer[0] = 0;
            AIN_buffer[1] = 0;
            AIN_buffer[2] = 0;
            AIN_buffer[3] = 0;
            
            AIN_is_eoc = true; // устанавливаем флаг готовности данных
            conv_count = 0;
            
            uint16_t result = AIN_result[1];
            
            if(ADDR != 0xFF)
            {
                if(ADDR == 0x00 || ADDR == 0x01)
                {
                    result = AIN_result[3];
                }
                
                VDDA = 3300*(*VREFINT_CAL_ADDR)/result;
            }
        }
        
        DMA1->IFCR         |= DMA_IFCR_CTCIF1;
        DMA1_Channel1->CCR |= DMA_CCR_TCIE; // подключаем прерывания DMA
    }
}
//-------------------------
uint16_t AIN_Get_VDDA(void)
{
    return VDDA;
}
