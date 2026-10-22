/**
 ******************************************************************************
 * @file           : main.c
 * @author         : Aaron Igor Vasquez Gil
 * @brief          : Main program body
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
 * Contador de 4 digitos (0 a 65535) en display de 7 segmentos multiplexado.
 * La fotocompuerta de PA4 aumenta el contador y la de PC2 lo disminuye.
 * El led de PH1 parpadea cada segundo para saber que la tarjeta esta funcionando.
 *
 * Digitos (se prenden con 0):   Segmentos (se prenden con 0):
 * PIN 1 ----- PC5  (miles)       a ----- PB12    e ----- PC10
 * PIN 2 ----- PC6  (centenas)    b ----- PA12    f ----- PA11
 * PIN 3 ----- PC13 (decenas)     c ----- PD2     g ----- PC11
 * PIN 4 ----- PB7  (unidades)    d ----- PC12
 *
 * Fotocompuertas:
 * PA4 ----- Aumenta (EXTI4)
 * PC2 ----- Disminuye (EXTI2)
 *
 * LED de estado ----- PH1
 *
 *
 * FUNCIONAMIENTO DEL SISTEMA
 * - Arranque: tras configurar los GPIO se espera un momento y se lee una sola
 *   vez el nivel de cada fotocompuerta con el haz libre (nivel_libre_inc y
 *   nivel_libre_dec). Por eso las dos deben estar libres al encender.
 * - TIM9 (interrupción cada 1 s): invierte PH1, así el LED queda 1 s
 *   encendido y 1 s apagado (blinky de 0,5 Hz) e indica que el MCU funciona.
 * - TIM3 (interrupción cada 3 ms): avanza al siguiente dígito y levanta
 *   cambio_digito_flag. Los 4 dígitos se barren en 12 ms (~83 Hz), por lo
 *   que el ojo los percibe encendidos a la vez (efecto POV). En cada cambio
 *   se apaga todo antes de escribir el siguiente dígito para evitar el
 *   "fantasma".
 * - EXTI4 (PA4) y EXTI2 (PC2): cada ISR baja su bandera y, solo si el sistema
 *   está en REPOSO, pasa el estado a AUMENTA o DISMINUYE. Si ya hay un
 *   conteo en proceso, el evento se ignora.
 * - Total: 4 interrupciones (TIM9, TIM3, EXTI4, EXTI2).
 *
 * MÁQUINA DE ESTADOS (main)
 *
 *   El refresco del display va fuera de la máquina de estados, así que se
 *   ejecuta en cualquier estado y la pantalla no se apaga mientras se cuenta.
 *
 *   REPOSO           : espera a que una ISR cambie el estado.
 *   AUMENTA          : suma 1 al contador (una sola vez).
 *   ESPERA_AUMENTA   : espera a que PA4 vuelva a su nivel libre, borra los
 *                      flancos pendientes (rebotes) y vuelve a REPOSO.
 *   DISMINUYE        : resta 1 al contador (una sola vez).
 *   ESPERA_DISMINUYE : igual que ESPERA_AUMENTA, pero con PC2.
 */

#include <stdint.h>
#include "stm32f4xx.h"
#include "stm32f411xe.h"

// Límite del contador (4 dígitos -> máximo 9999)
#define CONTADOR_MAX      9999

// Configuración de la lógica de los sensores de las fotocompuertas.
// 1 = El sensor entrega un '0' cuando detecta paso (Activo Bajo).
// 0 = El sensor entrega un '1' cuando detecta paso (Activo Alto).
#define SENSOR_INC_ACTIVO_BAJO  1
#define SENSOR_DEC_ACTIVO_BAJO  0

// Nivel (0 o 1) que tiene cada sensor cuando NO hay nada tapando el haz.
// Se lee una sola vez al arrancar, asi no se depende de adivinar la polaridad.
volatile uint8_t nivel_libre_inc = 0;
volatile uint8_t nivel_libre_dec = 0;

// Estas macros devuelven 1 cuando el sensor ya volvio a su nivel de reposo (haz libre).
// Se usan en los estados de espera para saber cuando termino el paso del objeto.
#define INC_LIBRE()  ((((GPIOA->IDR) >> 4) & 1) == nivel_libre_inc)
#define DEC_LIBRE()  ((((GPIOC->IDR) >> 2) & 1) == nivel_libre_dec)

// Variables globales volátiles porque cambian dentro de las ISR (interrupciones)
volatile uint16_t contador = 0;             // Valor global a mostrar (0 - 9999)
volatile uint8_t digito_actual = 0;         // Índice del dígito activo (0: miles, 1: centenas, 2: decenas, 3: unidades)
volatile uint8_t cambio_digito_flag = 0;    // Bandera levantada por el Timer 3 para refrescar la pantalla

uint8_t bit = 0;                            // Variable auxiliar para extraer bits individuales
uint8_t valor[4] = {0, 0, 0, 0};            // Arreglo para guardar el número descompuesto [miles, centenas, decenas, unidades]

// Declaración de funciones
void gpio_init(void);
void exti_init(void);
void tim9_init(void);
void tim3_init(void);

void cambiar_digito(void);
void escribir(void);
void apagar(void);

// Estados del sistema.
// REPOSO: no hay ningun objeto pasando, se acepta cualquier fotocompuerta.
// AUMENTA / DISMINUYE: la fotocompuerta detecto, falta que el main cuente (1 sola vez).
// ESPERA_*: ya se conto, se espera a que el objeto termine de pasar antes de aceptar otro.
typedef enum {
    REPOSO,
    AUMENTA,
    ESPERA_AUMENTA,
    DISMINUYE,
    ESPERA_DISMINUYE
} EstadoSistema;

// Estructura conveniente para mapear qué pin y puerto le corresponden a cada segmento (a..g)
typedef struct{
    GPIO_TypeDef *puerto;
    uint8_t pin;
} PinSegmento;

// Arreglo ordenado con los pines de cada segmento [a, b, c, d, e, f, g].
// Esto facilita iterar en un 'for' para encender/apagar segmentos.
const PinSegmento mapeo[7] = {
    {GPIOB, 12}, // Segmento a -> PB12
    {GPIOA, 12}, // Segmento b -> PA12
    {GPIOD, 2},  // Segmento c -> PD2
    {GPIOC, 12}, // Segmento d -> PC12
    {GPIOC, 10}, // Segmento e -> PC10
    {GPIOA, 11}, // Segmento f -> PA11
    {GPIOC, 11}  // Segmento g -> PC11
};

// Tabla de codificación 7 segmentos (Activo en BAJO -> 0 enciende, 1 apaga).
// Los bits representan los segmentos [g f e d c b a] del bit 6 al bit 0.
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

// Variable de estado inicializada en REPOSO
volatile EstadoSistema Estado = REPOSO;

int main(void)
{
    // Inicialización de todos los periféricos
    gpio_init();

    // Pequeña pausa para que las resistencias pull-up/pull-down se estabilicen
    // y despues se guarda el nivel de cada sensor en reposo (con el haz libre).
    // IMPORTANTE: al encender, las dos fotocompuertas deben estar libres.
    for (volatile uint32_t i = 0; i < 200000; i++);
    nivel_libre_inc = (GPIOA->IDR >> 4) & 1;
    nivel_libre_dec = (GPIOC->IDR >> 2) & 1;

    exti_init();
    tim9_init();
    tim3_init();

    while(1){

        // El refresco del display va FUERA de la maquina de estados para que
        // siga funcionando en cualquier estado y no se apague mientras se cuenta.
        if (cambio_digito_flag) {
            apagar();          // 1. Apagamos todo para evitar sombras/efecto fantasma (ghosting)
            escribir();        // 2. Colocamos los segmentos del nuevo dígito
            cambiar_digito();  // 3. Encendemos el común del dígito correspondiente

            cambio_digito_flag = 0; // Limpiamos la bandera
        }

        // Maquina de estados: las ISR de las fotocompuertas son las que sacan al
        // sistema de REPOSO y el main lo va llevando por los demas estados.
        switch (Estado) {

            case(REPOSO):
                // No hace nada, espera a que una interrupcion cambie el estado
                break;

            case(AUMENTA):
                // Da la vuelta: despues de 65535 sigue 0
                contador++;

                Estado = ESPERA_AUMENTA;   // ya conto, ahora espera a que el sensor se libere
                break;

            case(ESPERA_AUMENTA):
                // Mientras el objeto siga tapando el sensor no se hace nada,
                // asi un mismo paso no se cuenta mas de una vez.
                if (INC_LIBRE()) {
                    EXTI->PR = EXTI_PR_PR4;   // se borran flancos que hayan llegado (rebotes) mientras esperaba
                    Estado = REPOSO;
                }
                break;

            case(DISMINUYE):
                // Da la vuelta: antes de 0 sigue 65535
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

/*
 * Configuración de Puertos de Entrada/Salida (GPIO)
 */
void gpio_init(void){
    // Encendemos los relojes de todos los puertos de GPIO que utilizaremos
    RCC->AHB1ENR |= (RCC_AHB1ENR_GPIOAEN | RCC_AHB1ENR_GPIOBEN |
                     RCC_AHB1ENR_GPIOCEN | RCC_AHB1ENR_GPIODEN |
                     RCC_AHB1ENR_GPIOHEN);

    // Configuración PH1: LED de estado
    GPIOH->MODER  &= ~(GPIO_MODER_MODE1);
    GPIOH->MODER  |= (GPIO_MODER_MODE1_0); // Salida
    GPIOH->OTYPER &= ~(GPIO_OTYPER_OT1);    // Push-pull
    GPIOH->OSPEEDR &= ~(GPIO_OSPEEDR_OSPEED1);
    GPIOH->PUPDR  &= ~(GPIO_PUPDR_PUPD1);
    GPIOH->ODR    &= ~(GPIO_ODR_OD1);       // Inicia apagado

    // Configuramos el Puerto A (Segmentos b, f)
    GPIOA->ODR    |= (GPIO_ODR_OD12|GPIO_ODR_OD11); // Estado inicial en alto (Apagados)
    GPIOA->MODER  &= ~(GPIO_MODER_MODE12|GPIO_MODER_MODE11);
    GPIOA->MODER  |= (GPIO_MODER_MODE12_0|GPIO_MODER_MODE11_0); // Salidas
    GPIOA->OTYPER &= ~(GPIO_OTYPER_OT12|GPIO_OTYPER_OT11);
    GPIOA->OSPEEDR &= ~(GPIO_OSPEEDR_OSPEED12|GPIO_OSPEEDR_OSPEED11);
    GPIOA->PUPDR  &= ~(GPIO_PUPDR_PUPD12|GPIO_PUPDR_PUPD11);

    // Configuramos el Puerto C (Segmentos d, e, g y Comunes de Miles, Centenas, Decenas)
    GPIOC->ODR    |= (GPIO_ODR_OD10|GPIO_ODR_OD12|GPIO_ODR_OD11|GPIO_ODR_OD5|GPIO_ODR_OD13|GPIO_ODR_OD6);
    GPIOC->MODER  &= ~(GPIO_MODER_MODE10|GPIO_MODER_MODE12|GPIO_MODER_MODE11|GPIO_MODER_MODE5|GPIO_MODER_MODE13|GPIO_MODER_MODE6);
    GPIOC->MODER  |= (GPIO_MODER_MODE10_0|GPIO_MODER_MODE12_0|GPIO_MODER_MODE11_0|GPIO_MODER_MODE5_0|GPIO_MODER_MODE13_0|GPIO_MODER_MODE6_0);
    GPIOC->OTYPER &= ~(GPIO_OTYPER_OT10|GPIO_OTYPER_OT12|GPIO_OTYPER_OT11|GPIO_OTYPER_OT5|GPIO_OTYPER_OT13|GPIO_OTYPER_OT6);
    GPIOC->OSPEEDR &= ~(GPIO_OSPEEDR_OSPEED10|GPIO_OSPEEDR_OSPEED12|GPIO_OSPEEDR_OSPEED11|GPIO_OSPEEDR_OSPEED5|GPIO_OSPEEDR_OSPEED13|GPIO_OSPEEDR_OSPEED6);
    GPIOC->PUPDR  &= ~(GPIO_PUPDR_PUPD10|GPIO_PUPDR_PUPD12|GPIO_PUPDR_PUPD11|GPIO_PUPDR_PUPD5|GPIO_PUPDR_PUPD13|GPIO_PUPDR_PUPD6);

    // Configuramos el Puerto B (Segmento a y Común de Unidades)
    GPIOB->ODR    |= (GPIO_ODR_OD12|GPIO_ODR_OD7);
    GPIOB->MODER  &= ~(GPIO_MODER_MODE12|GPIO_MODER_MODE7);
    GPIOB->MODER  |= (GPIO_MODER_MODE12_0|GPIO_MODER_MODE7_0);
    GPIOB->OTYPER &= ~(GPIO_OTYPER_OT12|GPIO_OTYPER_OT7);
    GPIOB->OSPEEDR &= ~(GPIO_OSPEEDR_OSPEED12|GPIO_OSPEEDR_OSPEED7);
    GPIOB->PUPDR  &= ~(GPIO_PUPDR_PUPD12|GPIO_PUPDR_PUPD7);

    // Configuramos el Puerto D (Segmento c)
    GPIOD->ODR    |= (GPIO_ODR_OD2);
    GPIOD->MODER  &= ~(GPIO_MODER_MODE2);
    GPIOD->MODER  |= (GPIO_MODER_MODE2_0);
    GPIOD->OTYPER &= ~(GPIO_OTYPER_OT2);
    GPIOD->OSPEEDR &= ~(GPIO_OSPEEDR_OSPEED2);
    GPIOD->PUPDR  &= ~(GPIO_PUPDR_PUPD2);

    // Configuración de Entradas para las Fotocompuertas

    // PA4: Fotocompuerta Aumenta
    GPIOA->MODER &= ~(GPIO_MODER_MODE4); // Modo Entrada (00)
    GPIOA->PUPDR &= ~(GPIO_PUPDR_PUPD4);
#if SENSOR_INC_ACTIVO_BAJO
    GPIOA->PUPDR |= (GPIO_PUPDR_PUPD4_0); // Pull-up si activa por bajo
#else
    GPIOA->PUPDR |= (GPIO_PUPDR_PUPD4_1); // Pull-down si activa por alto
#endif

    // PC2: Fotocompuerta disminucion
    GPIOC->MODER &= ~(GPIO_MODER_MODE2); // Modo Entrada (00)
    GPIOC->PUPDR &= ~(GPIO_PUPDR_PUPD2);
#if SENSOR_DEC_ACTIVO_BAJO
    GPIOC->PUPDR |= (GPIO_PUPDR_PUPD2_0); // Pull-up
#else
    GPIOC->PUPDR |= (GPIO_PUPDR_PUPD2_1); // Pull-down
#endif
}

/*
 * Configuración de Interrupciones Externas (EXTI) para las Fotocompuertas
 */
void exti_init(void){
    // Habilitar reloj de SYSCFG para poder mapear los pines a las líneas EXTI
    RCC->APB2ENR |= (RCC_APB2ENR_SYSCFGEN);

    // Mapear PA4 a EXTI4 y PC2 a EXTI2
    SYSCFG->EXTICR[1] &= ~(SYSCFG_EXTICR2_EXTI4);
    SYSCFG->EXTICR[1] |= (SYSCFG_EXTICR2_EXTI4_PA);
    SYSCFG->EXTICR[0] &= ~(SYSCFG_EXTICR1_EXTI2);
    SYSCFG->EXTICR[0] |= (SYSCFG_EXTICR1_EXTI2_PC);

    // Limpiar flancos previos
    EXTI->FTSR &= ~(EXTI_FTSR_TR4 | EXTI_FTSR_TR2);
    EXTI->RTSR &= ~(EXTI_RTSR_TR4 | EXTI_RTSR_TR2);

    // Configurar si dispara por flanco de bajada (FTSR) o de subida (RTSR)
#if SENSOR_INC_ACTIVO_BAJO
    EXTI->FTSR |= (EXTI_FTSR_TR4);
#else
    EXTI->RTSR |= (EXTI_RTSR_TR4);
#endif

#if SENSOR_DEC_ACTIVO_BAJO
    EXTI->FTSR |= (EXTI_FTSR_TR2);
#else
    EXTI->RTSR |= (EXTI_RTSR_TR2);
#endif

    // Limpiar interrupciones pendientes y desenmascarar
    EXTI->PR  = (EXTI_PR_PR4 | EXTI_PR_PR2);
    EXTI->IMR |= (EXTI_IMR_MR4 | EXTI_IMR_MR2);

    // Habilitar en el controlador de interrupciones (NVIC)
    NVIC_EnableIRQ(EXTI4_IRQn);
    NVIC_EnableIRQ(EXTI2_IRQn);
}

// ISR: Fotocompuerta PA4 (Aumento)
void EXTI4_IRQHandler(void){
    if (EXTI->PR & EXTI_PR_PR4){
        EXTI->PR = EXTI_PR_PR4;  // Limpiar bandera escribiendo 1

        // Solo se acepta el evento si el sistema esta en REPOSO.
        // Si ya hay un conteo en proceso, este evento se ignora (no se pisa el estado).
        if (Estado == REPOSO) Estado = AUMENTA;
    }
}

// ISR: Fotocompuerta PC2 (Disminuye)
void EXTI2_IRQHandler(void){
    if (EXTI->PR & EXTI_PR_PR2){
        EXTI->PR = EXTI_PR_PR2;  // Limpiar bandera escribiendo 1

        if (Estado == REPOSO) Estado = DISMINUYE;
    }
}

/*
 * TIM9: Generador de tiempo de 1 segundo (Heartbeat LED)
 */
void tim9_init(void){
    RCC->APB2ENR |= (RCC_APB2ENR_TIM9EN);

    TIM9->CNT = 0;
    TIM9->PSC = (16000-1); // 16 MHz / 16000 = 1 kHz (Base de tiempo de 1 ms)
    TIM9->ARR = (1000-1);  // Cuenta hasta 1000 ms = 1 segundo
    TIM9->DIER |= (TIM_DIER_UIE); // Habilitar interrupción por desbordamiento
    TIM9->CR1 &= ~(TIM_CR1_CKD);
    TIM9->CR1 &= ~(TIM_CR1_ARPE);
    TIM9->SR = ~(TIM_SR_UIF);

    NVIC_EnableIRQ(TIM1_BRK_TIM9_IRQn);
    TIM9->CR1 |= (TIM_CR1_CEN); // Arrancar Timer 9
}

// ISR TIM9: Cambia el estado del LED cada 1 segundo
void TIM1_BRK_TIM9_IRQHandler(void){
    if (TIM9->SR & TIM_SR_UIF){
        GPIOH->ODR ^= (GPIO_ODR_OD1); // Commuta el estado de PH1
        TIM9->SR = ~(TIM_SR_UIF);     // Limpia la bandera del timer
    }
}

/*
 * TIM3: Temporizador para la multiplexación del Display (Frecuencia rápida para evitar parpadeo)
 */
void tim3_init(void){
    RCC->APB1ENR |= (RCC_APB1ENR_TIM3EN);

    TIM3->CNT = 0;
    TIM3->PSC = (1600-1); // Prescaler
    TIM3->ARR = (30-1);   // Dispara aproximadamente cada 3ms para refrescar rápido el dígito
    TIM3->DIER |= (TIM_DIER_UIE);
    TIM3->CR1 &= ~(TIM_CR1_CKD);
    TIM3->CR1 &= ~(TIM_CR1_ARPE);
    TIM3->CR1 &= ~(TIM_CR1_DIR);
    TIM3->SR = ~(TIM_SR_UIF);

    NVIC_EnableIRQ(TIM3_IRQn);
    TIM3->CR1 |= (TIM_CR1_CEN); // Arrancar Timer 3
}

// ISR TIM3: Cambia de dígito y avisa al bucle principal
void TIM3_IRQHandler(void){
    if(TIM3->SR & TIM_SR_UIF){
        TIM3->SR = ~(TIM_SR_UIF);

        // Rotamos entre los 4 dígitos (0 -> 1 -> 2 -> 3 -> 0)
        digito_actual = (digito_actual + 1) % 4;
        cambio_digito_flag = 1; // Bandera para procesar el refresco en main()
    }
}

/*
 * Desactiva todos los dígitos y segmentos (se ponen en 1 lógico)
 * Esto es indispensable para evitar "fantasmas" durante el refresco.
 */
void apagar(void){
    GPIOA->ODR |= (GPIO_ODR_OD12|GPIO_ODR_OD11);
    GPIOC->ODR |= (GPIO_ODR_OD10|GPIO_ODR_OD12|GPIO_ODR_OD11|GPIO_ODR_OD5|GPIO_ODR_OD13|GPIO_ODR_OD6);
    GPIOB->ODR |= (GPIO_ODR_OD12|GPIO_ODR_OD7);
    GPIOD->ODR |= (GPIO_ODR_OD2);
}

/*
 * Activa el común del dígito que toca mostrar
 * y calcula el número individual (Miles, Centenas, Decenas o Unidades).
 */
void cambiar_digito(void){
    switch(digito_actual){
        case 0: // Miles
            GPIOC->ODR &= ~(GPIO_ODR_OD5);
            valor[0] = (contador / 1000) % 10;
            break;
        case 1: // Centenas
            GPIOC->ODR &= ~(GPIO_ODR_OD6);
            valor[1] = (contador / 100) % 10;
            break;
        case 2: // Decenas
            GPIOC->ODR &= ~(GPIO_ODR_OD13);
            valor[2] = (contador / 10) % 10;
            break;
        case 3: // Unidades
            GPIOB->ODR &= ~(GPIO_ODR_OD7);
            valor[3] = contador % 10;
            break;
    }
}

/*
 * Toma el número a mostrar (0 a 9), consulta la tabla de segmentos
 * y aplica bit por bit el estado correspondiente a los pines de la tarjeta.
 */
void escribir(void){
    for(uint8_t i = 0; i < 7; i++){

        // Extraemos el bit 'i' correspondiente al segmento (0 = Encender, 1 = Apagar)
        bit = (tabla_7seg[valor[digito_actual]] >> i) & 1;

        if(bit == 0){
            // Enciende el segmento mandando un 0
            mapeo[i].puerto->ODR &= ~(0b1 << mapeo[i].pin);
        }
        else{
            // Apaga el segmento mandando un 1
            mapeo[i].puerto->ODR |= (0b1 << mapeo[i].pin);
        }
    }
}