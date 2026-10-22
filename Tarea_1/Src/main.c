/**
 ******************************************************************************
 * @file           : main.c
 * @author         : Aaron Igor Vasquez Gil
 * @brief          : Contador en display 7 segmentos, controlado por fotocompuertas de crecimiento y decrecimiento 
 ******************************************************************************
 * @attention
 *
 * Copyright (c) 2025 STMicroelectronics.
 * All rights reserved.
 *
 * This software is licensed under terms that can be found in the LICENSE file
 * in the root directory of this software component.
 * If no LICENSE file comes with this software, it is provided AS-IS.
 *
 ******************************************************************************
 *
 * Contador de 16 bits (0 a 65535) en 4 displays de 7 segmentos multiplexados.
 * El display muestra los 4 dígitos bajos del valor.
 * La fotocompuerta de PA4 suma 1 y la de PC2 resta 1.
 * El LED de PH1 parpadea cada segundo para indicar que la tarjeta funciona.
 *
 * PINES
 *   Dígitos (se prenden con 0)      Segmentos (se prenden con 0)
 *   PIN 1 ----- PC5  (miles)        a ----- PB12    e ----- PC10
 *   PIN 2 ----- PC6  (centenas)     b ----- PA12    f ----- PA11
 *   PIN 3 ----- PC13 (decenas)      c ----- PD2     g ----- PC11
 *   PIN 4 ----- PB7  (unidades)     d ----- PC12
 *
 *   Fotocompuertas                  LED de estado
 *   PA4 ----- Aumenta (EXTI4)       PH1
 *   PC2 ----- Disminuye (EXTI2)
 *
 * FUNCIONAMIENTO
 * - Arranque: se lee el nivel de cada fotocompuerta con el haz libre.
 *   Las dos deben estar libres al encender.
 * - TIM9 (1 s): invierte PH1 (LED 1 s encendido y 1 s apagado).
 * - TIM3 (3 ms): pasa al siguiente dígito y avisa al main con una bandera.
 *   Los 4 dígitos se barren en 12 ms (~83 Hz), así que se ven encendidos
 *   a la vez.
 * - EXTI4 y EXTI2: si el sistema está en REPOSO, pasan a AUMENTA o
 *   DISMINUYE. Si no, el evento se ignora.
 *
 * ESTADOS (main)
 *   El refresco del display va fuera de la máquina de estados, así que
 *   funciona siempre.
 *
 *   REPOSO           : espera a que una ISR cambie el estado.
 *   AUMENTA          : suma 1 al contador.
 *   ESPERA_AUMENTA   : espera a que PA4 se libere y vuelve a REPOSO.
 *   DISMINUYE        : resta 1 al contador.
 *   ESPERA_DISMINUYE : espera a que PC2 se libere y vuelve a REPOSO.
 */

#include <stdint.h>
#include "stm32f4xx.h"
#include "stm32f411xe.h"

// Polaridad de cada sensor: 1 = activo bajo (da 0 al detectar), 0 = activo alto
#define SENSOR_INC_ACTIVO_BAJO  1
#define SENSOR_DEC_ACTIVO_BAJO  0

// Nivel de cada sensor con el haz libre (se lee al arrancar)
volatile uint8_t nivel_libre_inc = 0;
volatile uint8_t nivel_libre_dec = 0;

// Dan 1 cuando el sensor ya volvió a su nivel libre
#define INC_LIBRE()  ((((GPIOA->IDR) >> 4) & 1) == nivel_libre_inc)
#define DEC_LIBRE()  ((((GPIOC->IDR) >> 2) & 1) == nivel_libre_dec)

// Variables compartidas con las ISR
volatile uint16_t contador = 0;             // valor a mostrar
volatile uint8_t digito_actual = 0;         // 0: miles, 1: centenas, 2: decenas, 3: unidades
volatile uint8_t cambio_digito_flag = 0;    // TIM3 la levanta para refrescar el display

uint8_t bit = 0;                            // bit del segmento actual
uint8_t valor[4] = {0, 0, 0, 0};            // cifras [miles, centenas, decenas, unidades]

void gpio_init(void);
void exti_init(void);
void tim9_init(void);
void tim3_init(void);

void cambiar_digito(void);
void escribir(void);
void apagar(void);

// REPOSO: sin objeto pasando
// AUMENTA / DISMINUYE: detectado, falta que el main cuente
// ESPERA_*: ya contó, espera a que el objeto termine de pasar
typedef enum {
    REPOSO,
    AUMENTA,
    ESPERA_AUMENTA,
    DISMINUYE,
    ESPERA_DISMINUYE
} EstadoSistema;

// Puerto y pin de un segmento
typedef struct{
    GPIO_TypeDef *puerto;
    uint8_t pin;
} PinSegmento;

// Pines de los segmentos [a, b, c, d, e, f, g]
const PinSegmento mapeo[7] = {
    {GPIOB, 12}, // a
    {GPIOA, 12}, // b
    {GPIOD, 2},  // c
    {GPIOC, 12}, // d
    {GPIOC, 10}, // e
    {GPIOA, 11}, // f
    {GPIOC, 11}  // g
};

// Bits [g f e d c b a]: 0 enciende el segmento y 1 lo apaga
const uint8_t tabla_7seg[10] = {
    //     gfedcba
    0b11000000, // 0
    0b11111001, // 1
    0b10100100, // 2
    0b10110000, // 3
    0b10011001, // 4
    0b10010010, // 5
    0b10000010, // 6
    0b11111000, // 7
    0b10000000, // 8
    0b10010000  // 9
};

volatile EstadoSistema Estado = REPOSO;

int main(void)
{
    gpio_init();

    // Pausa para que se estabilicen los pull-up/pull-down y se guarda el nivel libre
    for (volatile uint32_t i = 0; i < 200000; i++);
    nivel_libre_inc = (GPIOA->IDR >> 4) & 1;
    nivel_libre_dec = (GPIOC->IDR >> 2) & 1;

    exti_init();
    tim9_init();
    tim3_init();

    while(1){

        // Refresco del display
        if (cambio_digito_flag) {
            apagar();          // evita el efecto fantasma
            escribir();        // segmentos del dígito nuevo
            cambiar_digito();  // enciende el común de ese dígito

            cambio_digito_flag = 0;
        }

        // Máquina de estados
        switch (Estado) {

            case(REPOSO):
                // espera a una ISR
                break;

            case(AUMENTA):
                // después de 65535 sigue 0
                contador++;

                Estado = ESPERA_AUMENTA;
                break;

            case(ESPERA_AUMENTA):
                // un paso se cuenta una sola vez: espera a que el sensor se libere
                if (INC_LIBRE()) {
                    EXTI->PR = EXTI_PR_PR4;   // borra flancos pendientes (rebotes)
                    Estado = REPOSO;
                }
                break;

            case(DISMINUYE):
                // antes de 0 sigue 65535
                contador--;

                Estado = ESPERA_DISMINUYE;
                break;

            case(ESPERA_DISMINUYE):
                if (DEC_LIBRE()) {
                    EXTI->PR = EXTI_PR_PR2;
                    Estado = REPOSO;
                }
                break;

            default:
                Estado = REPOSO;
                break;
        }
    }
}

void gpio_init(void){
    // Relojes de los puertos
    RCC->AHB1ENR |= (RCC_AHB1ENR_GPIOAEN | RCC_AHB1ENR_GPIOBEN |
                     RCC_AHB1ENR_GPIOCEN | RCC_AHB1ENR_GPIODEN |
                     RCC_AHB1ENR_GPIOHEN);

    // PH1: LED de estado (salida push-pull, inicia apagado)
    GPIOH->MODER  &= ~(GPIO_MODER_MODE1);
    GPIOH->MODER  |= (GPIO_MODER_MODE1_0);
    GPIOH->OTYPER &= ~(GPIO_OTYPER_OT1);
    GPIOH->OSPEEDR &= ~(GPIO_OSPEEDR_OSPEED1);
    GPIOH->PUPDR  &= ~(GPIO_PUPDR_PUPD1);
    GPIOH->ODR    &= ~(GPIO_ODR_OD1);

    // Puerto A: segmentos b, f (inician en 1 = apagados)
    GPIOA->ODR    |= (GPIO_ODR_OD12|GPIO_ODR_OD11);
    GPIOA->MODER  &= ~(GPIO_MODER_MODE12|GPIO_MODER_MODE11);
    GPIOA->MODER  |= (GPIO_MODER_MODE12_0|GPIO_MODER_MODE11_0);
    GPIOA->OTYPER &= ~(GPIO_OTYPER_OT12|GPIO_OTYPER_OT11);
    GPIOA->OSPEEDR &= ~(GPIO_OSPEEDR_OSPEED12|GPIO_OSPEEDR_OSPEED11);
    GPIOA->PUPDR  &= ~(GPIO_PUPDR_PUPD12|GPIO_PUPDR_PUPD11);

    // Puerto C: segmentos d, e, g y dígitos de miles, centenas y decenas
    GPIOC->ODR    |= (GPIO_ODR_OD10|GPIO_ODR_OD12|GPIO_ODR_OD11|GPIO_ODR_OD5|GPIO_ODR_OD13|GPIO_ODR_OD6);
    GPIOC->MODER  &= ~(GPIO_MODER_MODE10|GPIO_MODER_MODE12|GPIO_MODER_MODE11|GPIO_MODER_MODE5|GPIO_MODER_MODE13|GPIO_MODER_MODE6);
    GPIOC->MODER  |= (GPIO_MODER_MODE10_0|GPIO_MODER_MODE12_0|GPIO_MODER_MODE11_0|GPIO_MODER_MODE5_0|GPIO_MODER_MODE13_0|GPIO_MODER_MODE6_0);
    GPIOC->OTYPER &= ~(GPIO_OTYPER_OT10|GPIO_OTYPER_OT12|GPIO_OTYPER_OT11|GPIO_OTYPER_OT5|GPIO_OTYPER_OT13|GPIO_OTYPER_OT6);
    GPIOC->OSPEEDR &= ~(GPIO_OSPEEDR_OSPEED10|GPIO_OSPEEDR_OSPEED12|GPIO_OSPEEDR_OSPEED11|GPIO_OSPEEDR_OSPEED5|GPIO_OSPEEDR_OSPEED13|GPIO_OSPEEDR_OSPEED6);
    GPIOC->PUPDR  &= ~(GPIO_PUPDR_PUPD10|GPIO_PUPDR_PUPD12|GPIO_PUPDR_PUPD11|GPIO_PUPDR_PUPD5|GPIO_PUPDR_PUPD13|GPIO_PUPDR_PUPD6);

    // Puerto B: segmento a y dígito de unidades
    GPIOB->ODR    |= (GPIO_ODR_OD12|GPIO_ODR_OD7);
    GPIOB->MODER  &= ~(GPIO_MODER_MODE12|GPIO_MODER_MODE7);
    GPIOB->MODER  |= (GPIO_MODER_MODE12_0|GPIO_MODER_MODE7_0);
    GPIOB->OTYPER &= ~(GPIO_OTYPER_OT12|GPIO_OTYPER_OT7);
    GPIOB->OSPEEDR &= ~(GPIO_OSPEEDR_OSPEED12|GPIO_OSPEEDR_OSPEED7);
    GPIOB->PUPDR  &= ~(GPIO_PUPDR_PUPD12|GPIO_PUPDR_PUPD7);

    // Puerto D: segmento c
    GPIOD->ODR    |= (GPIO_ODR_OD2);
    GPIOD->MODER  &= ~(GPIO_MODER_MODE2);
    GPIOD->MODER  |= (GPIO_MODER_MODE2_0);
    GPIOD->OTYPER &= ~(GPIO_OTYPER_OT2);
    GPIOD->OSPEEDR &= ~(GPIO_OSPEEDR_OSPEED2);
    GPIOD->PUPDR  &= ~(GPIO_PUPDR_PUPD2);

    // PA4: fotocompuerta que aumenta (entrada, pull según la polaridad)
    GPIOA->MODER &= ~(GPIO_MODER_MODE4);
    GPIOA->PUPDR &= ~(GPIO_PUPDR_PUPD4);
#if SENSOR_INC_ACTIVO_BAJO
    GPIOA->PUPDR |= (GPIO_PUPDR_PUPD4_0); // pull-up
#else
    GPIOA->PUPDR |= (GPIO_PUPDR_PUPD4_1); // pull-down
#endif

    // PC2: fotocompuerta que disminuye (entrada, pull según la polaridad)
    GPIOC->MODER &= ~(GPIO_MODER_MODE2);
    GPIOC->PUPDR &= ~(GPIO_PUPDR_PUPD2);
#if SENSOR_DEC_ACTIVO_BAJO
    GPIOC->PUPDR |= (GPIO_PUPDR_PUPD2_0); // pull-up
#else
    GPIOC->PUPDR |= (GPIO_PUPDR_PUPD2_1); // pull-down
#endif
}

void exti_init(void){
    // El SYSCFG conecta cada pin con su línea EXTI
    RCC->APB2ENR |= (RCC_APB2ENR_SYSCFGEN);

    // PA4 -> EXTI4 y PC2 -> EXTI2
    SYSCFG->EXTICR[1] &= ~(SYSCFG_EXTICR2_EXTI4);
    SYSCFG->EXTICR[1] |= (SYSCFG_EXTICR2_EXTI4_PA);
    SYSCFG->EXTICR[0] &= ~(SYSCFG_EXTICR1_EXTI2);
    SYSCFG->EXTICR[0] |= (SYSCFG_EXTICR1_EXTI2_PC);

    EXTI->FTSR &= ~(EXTI_FTSR_TR4 | EXTI_FTSR_TR2);
    EXTI->RTSR &= ~(EXTI_RTSR_TR4 | EXTI_RTSR_TR2);

    // Flanco de cada sensor según su polaridad
#if SENSOR_INC_ACTIVO_BAJO
    EXTI->FTSR |= (EXTI_FTSR_TR4); // bajada
#else
    EXTI->RTSR |= (EXTI_RTSR_TR4); // subida
#endif

#if SENSOR_DEC_ACTIVO_BAJO
    EXTI->FTSR |= (EXTI_FTSR_TR2); // bajada
#else
    EXTI->RTSR |= (EXTI_RTSR_TR2); // subida
#endif

    // Borra pendientes, habilita las líneas y las interrupciones en el NVIC
    EXTI->PR  = (EXTI_PR_PR4 | EXTI_PR_PR2);
    EXTI->IMR |= (EXTI_IMR_MR4 | EXTI_IMR_MR2);

    NVIC_EnableIRQ(EXTI4_IRQn);
    NVIC_EnableIRQ(EXTI2_IRQn);
}

// PA4: aumenta
void EXTI4_IRQHandler(void){
    if (EXTI->PR & EXTI_PR_PR4){
        EXTI->PR = EXTI_PR_PR4;  // se baja escribiendo 1

        // solo se acepta en REPOSO, si hay un conteo en proceso se ignora
        if (Estado == REPOSO) Estado = AUMENTA;
    }
}

// PC2: disminuye
void EXTI2_IRQHandler(void){
    if (EXTI->PR & EXTI_PR_PR2){
        EXTI->PR = EXTI_PR_PR2;

        if (Estado == REPOSO) Estado = DISMINUYE;
    }
}

// TIM9: 16 MHz / 16000 = 1 kHz, con ARR de 1000 desborda cada 1 s
void tim9_init(void){
    RCC->APB2ENR |= (RCC_APB2ENR_TIM9EN);

    TIM9->CNT = 0;
    TIM9->PSC = (16000-1);
    TIM9->ARR = (1000-1);
    TIM9->DIER |= (TIM_DIER_UIE);
    TIM9->CR1 &= ~(TIM_CR1_CKD);
    TIM9->CR1 &= ~(TIM_CR1_ARPE);
    TIM9->SR = ~(TIM_SR_UIF);

    NVIC_EnableIRQ(TIM1_BRK_TIM9_IRQn);
    TIM9->CR1 |= (TIM_CR1_CEN);
}

// Invierte el LED cada 1 s
void TIM1_BRK_TIM9_IRQHandler(void){
    if (TIM9->SR & TIM_SR_UIF){
        GPIOH->ODR ^= (GPIO_ODR_OD1);
        TIM9->SR = ~(TIM_SR_UIF);
    }
}

// TIM3: 16 MHz / 1600 = 10 kHz, con ARR de 30 desborda cada 3 ms
void tim3_init(void){
    RCC->APB1ENR |= (RCC_APB1ENR_TIM3EN);

    TIM3->CNT = 0;
    TIM3->PSC = (1600-1);
    TIM3->ARR = (30-1);
    TIM3->DIER |= (TIM_DIER_UIE);
    TIM3->CR1 &= ~(TIM_CR1_CKD);
    TIM3->CR1 &= ~(TIM_CR1_ARPE);
    TIM3->CR1 &= ~(TIM_CR1_DIR);
    TIM3->SR = ~(TIM_SR_UIF);

    NVIC_EnableIRQ(TIM3_IRQn);
    TIM3->CR1 |= (TIM_CR1_CEN);
}

// Pasa al siguiente dígito (0 -> 1 -> 2 -> 3 -> 0) y avisa al main
void TIM3_IRQHandler(void){
    if(TIM3->SR & TIM_SR_UIF){
        TIM3->SR = ~(TIM_SR_UIF);

        digito_actual = (digito_actual + 1) % 4;
        cambio_digito_flag = 1;
    }
}

// Apaga segmentos y dígitos (todo en 1) para que no quede el número anterior
void apagar(void){
    GPIOA->ODR |= (GPIO_ODR_OD12|GPIO_ODR_OD11);
    GPIOC->ODR |= (GPIO_ODR_OD10|GPIO_ODR_OD12|GPIO_ODR_OD11|GPIO_ODR_OD5|GPIO_ODR_OD13|GPIO_ODR_OD6);
    GPIOB->ODR |= (GPIO_ODR_OD12|GPIO_ODR_OD7);
    GPIOD->ODR |= (GPIO_ODR_OD2);
}

// Enciende el dígito que toca y calcula su cifra
void cambiar_digito(void){
    switch(digito_actual){
        case 0: // miles
            GPIOC->ODR &= ~(GPIO_ODR_OD5);
            valor[0] = (contador / 1000) % 10;
            break;
        case 1: // centenas
            GPIOC->ODR &= ~(GPIO_ODR_OD6);
            valor[1] = (contador / 100) % 10;
            break;
        case 2: // decenas
            GPIOC->ODR &= ~(GPIO_ODR_OD13);
            valor[2] = (contador / 10) % 10;
            break;
        case 3: // unidades
            GPIOB->ODR &= ~(GPIO_ODR_OD7);
            valor[3] = contador % 10;
            break;
    }
}

// Saca la cifra actual a los pines de los segmentos usando la tabla
void escribir(void){
    for(uint8_t i = 0; i < 7; i++){

        // bit i del patrón: 0 enciende, 1 apaga
        bit = (tabla_7seg[valor[digito_actual]] >> i) & 1;

        if(bit == 0){
            mapeo[i].puerto->ODR &= ~(0b1 << mapeo[i].pin);
        }
        else{
            mapeo[i].puerto->ODR |= (0b1 << mapeo[i].pin);
        }
    }
}