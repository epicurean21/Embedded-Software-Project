#include "includes.h"

#define F_CPU	16000000UL	// CPU frequency = 16 Mhz
#include <avr/io.h>
#include <util/delay.h>
#include <avr/interrupt.h>

#define  TASK_STK_SIZE  OS_TASK_DEF_STK_SIZE
#define  N_TASKS        3

#define CDS_VALUE 971

// My Notes
#define OFF 0
#define ON 1
#define DO 17
#define RE 43
#define MI 66
#define FA 77
#define SOL 97
#define LA 114
#define TI 129
#define UDO 137
#define EOS 255

const unsigned char FND_DATA[] = { 0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x27, 0x7F, 0x6F, 0x77, 0x7C, 0x39, 0x5E, 0x79, 0x71 };
int LED_DATA[] = { 0, 128, 192, 224, 240, 248, 252, 254, 255 };
const unsigned char FND_sel[] = { 0x01,0x02,0x04,0x08 };
const unsigned char LED_sel[] = { 0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x00 };
const unsigned char FND_OVER[] = { 0x50, 0x79, 0x3E, 0x3F };
OS_STK       TaskStk[N_TASKS][TASK_STK_SIZE];
OS_EVENT* Mbox; // Mailbox
OS_EVENT* Sem; // Semaphore
OS_EVENT* F_grp; // FlagGrp

volatile INT8U	FndNum;
volatile unsigned char alarm[] = { UDO, UDO, UDO, -1 }; // My Alarm
volatile int OVER = OFF; // When the Process is Over
INT8U err;
volatile int sound = -1; // Sound, initial value = -1

volatile int state = OFF;
volatile int index = 0;

volatile int start = ON; // When the First Process begins
volatile int flag = 0;
volatile int maxCnt = 0;

void FND_SET() { // Set FND
    OS_ENTER_CRITICAL();
    DDRC = 0xFF;
    DDRG = 0x0F;
    OS_EXIT_CRITICAL();
}

void LED_SET() { // Set LED
    OS_ENTER_CRITICAL();
    DDRA = 0xFF;
    PORTA = 0x00;
    OS_EXIT_CRITICAL();
}

void BUZZER_SET() { // Set Buzzer
    OS_ENTER_CRITICAL();
    DDRB = 0x10;
    TCCR2 = 0x03;
    TCCR0 = 0x07;
    TIMSK = _BV(TOIE0) | _BV(TOIE2);
    TCNT0 = 256 - (CPU_CLOCK_HZ / OS_TICKS_PER_SEC / 1024);
    OS_EXIT_CRITICAL();
}

void CDS_SET() { // Set Cds
    OS_ENTER_CRITICAL();
    ADMUX = 0x00;
    ADCSRA = 0x87;
    OS_EXIT_CRITICAL();
}

void FND_RESET() { // Clear FND settings
    OS_ENTER_CRITICAL();
    DDRC = 0xFF;
    DDRG = 0x00;
    OS_EXIT_CRITICAL();
}

void LED_RESET() { // Clear LED Settings
    OS_ENTER_CRITICAL();
    DDRA = 0x00;
    PORTA = 0x00;
    OS_EXIT_CRITICAL();
}

unsigned short read_adc() { // For Cds, Read adc return the value
    unsigned char adc_low, adc_high;
    unsigned short value;
    ADCSRA |= 0x40; // ADC start conversion, ADSC = '1'
    while ((ADCSRA & (0x10)) != 0x10); // ADC 변환 완료 검사
    adc_low = ADCL;
    adc_high = ADCH;
    value = (adc_high << 8) | adc_low;
    return value;
}

void Display_FND(int c) { // Display People Cnt in FND.
    unsigned char fnd[2]; // 0 <= c <= 40
    fnd[1] = FND_DATA[(c / 10)];
    fnd[0] = FND_DATA[c % 10];
    int i;
    for (i = 0; i < 2; i++)
    {
        PORTC = fnd[i];
        PORTG = FND_sel[i];
        _delay_us(500);
    }
}

/*
 Display LED는...
 최초 설정한 인원수 (설정 수)에
 현재 카운트 된 입장 사람 수의 비율을 LED 8개로 비율로 나타낸다.
 이렇게하여 LED를 통하여 현재 내가 설정한 인원에 비례해 몇 퍼센트의 인원이 장소에 찼는지
 알 수 있다.
 */
void Display_LED(int c) { // Display LED
    // c = current people cnt;
    double ratio = 8 / maxCnt;
    int v;
    v = (int)c * ratio;
    if (v > 8)
        v = 8;
    unsigned char value = LED_DATA[v];
    PORTA = value;
}

// Display OVER in FND
void Display_OVER() {
    int i;
    for (i = 0; i < 4; i++) {
        PORTC = FND_OVER[i];
        PORTG = FND_sel[i];
        _delay_us(500);
    }
}

ISR(INT4_vect) { // switch 1, Only Operate for increasing Count value;
    if (start) { // state = start
        OSSemPend(Sem, 0, &err); // Using Semaphore. maxCnt = 사용자가 선택한 허용 인원 수
        maxCnt = (maxCnt + 1) % 41; // maximum 40 people allowed
        OSSemPost(Sem);
    }
    else {
        //continue; // if the state is not start, sw1 is for nothing
    }
    _delay_ms(2);
}

ISR(INT5_vect) { //switch 2, set maxCnt if the state is Start, else reset the program.
    if (start) {
        OSSemPend(Sem, 0, &err); // Semaphore 이용, 전역변수 Flag, start의 값을 Toggle.
        flag = 1;
        start = OFF;
        OSSemPost(Sem);
    }
    else {
        //continue;
    }
    _delay_ms(2);
}

// Timer2 Interrupt 발생
ISR(TIMER2_OVF_vect) {
    if (OVER) { // Process Over 시에 작동된다.
        if (sound != -1) {
            if (state == ON) {
                PORTB &= ~(1 << 4);
                state = OFF;
            }
            else {
                PORTB |= (1 << 4);
                state = ON;
            }
            TCNT2 = sound;
        }
    }
}

void BeginTask(void* data) { // First Task.
    while (1) {
        // Task간 순서 동기화는, Priority 뿐 아니라 OS Flag를 활용하였다.
        // The First Task의 Flag Settings = 0x01
        OSFlagPend(F_grp, 0x01, OS_FLAG_WAIT_SET_ALL + OS_FLAG_CONSUME, 0, &err);
        OS_ENTER_CRITICAL();
        maxCnt = 0; // Init maxCnt
        OS_EXIT_CRITICAL();
        int peopleCnt = 0;
        while (!flag) {
            Display_FND(peopleCnt);
            peopleCnt = maxCnt;
        }
        OSMboxPost(Mbox, (void*)&peopleCnt); // Using MailBox Post peopleCnt set value to Task #2
        OSFlagPost(F_grp, 0x02, OS_FLAG_SET, &err);
    }
}

void CountingTask(void* data) {
    while (1) {
        int setCnt = *(int*)OSMboxPend(Mbox, 0, &err); // Operates when the mail received. setCnt = peopleCnt
        // Task 간의 동기화를 위해, MailBox 도착 및 Flag가 0x02로 set 돼있을때 동작한다.
        OSFlagPend(F_grp, 0x02, OS_FLAG_WAIT_SET_ALL + OS_FLAG_CONSUME, 0, &err);
        volatile int currentCnt = 0;
        unsigned short cdsValue;
        while (1) {
            if (currentCnt > setCnt)
                break;
            Display_FND(currentCnt);
            Display_LED(currentCnt);
            cdsValue = read_adc();
            /*
             * 조도 센서에 사람/물건이 지나가며 어두워지면,
             * 현재 들어온 인원수를 하나씩 올린다.
            */
            if (cdsValue < CDS_VALUE) {
                currentCnt++;
            }
            _delay_ms(200);
        }
        OSSemPend(Sem, 0, &err);
        OVER = ON; // When the Second Task Is Over, OVER value = ON
        OSSemPost(Sem);
        OSFlagPost(F_grp, 0x04, OS_FLAG_SET, &err); // OS Flag를 이용해 세번째 Task에게 신호를 준다.
    }
}

void CountingEndTask(void* data) {
    int i;
    while (1) {
        /*
          3번째 Task의 동작 조건.
          OS Flag = 0x04
         */
        OSFlagPend(F_grp, 0x04, OS_FLAG_WAIT_SET_ALL, 0, &err);
        while (1) {
            for (i = 0; i < 200; i++) {
                sound = alarm[i / 50];
                PORTC = FND_OVER[0];
                PORTG = FND_sel[0];
                _delay_us(500);
                PORTC = FND_OVER[1];
                PORTG = FND_sel[1];
                _delay_us(500);
                PORTC = FND_OVER[2];
                PORTG = FND_sel[2];
                _delay_us(500);
                PORTC = FND_OVER[3];
                PORTG = FND_sel[3];
                _delay_us(500);
            }
        }
    }
}

int main(void)
{
    OSInit();

    FND_SET();
    LED_SET();
    BUZZER_SET();
    CDS_SET();

    DDRE = 0xCF;
    EIMSK = 0x30;
    EICRB = 0x0A;
    sei();
    Mbox = OSMboxCreate((void*)0); // MailBox
    Sem = OSSemCreate(1); // Semaphore
    F_grp = OSFlagCreate(0x01, &err); // OS Flag

    /*Task Create */
    OSTaskCreate(BeginTask, (void*)0, (void*)&TaskStk[0][TASK_STK_SIZE - 1], 0);
    OSTaskCreate(CountingTask, (void*)0, (void*)&TaskStk[1][TASK_STK_SIZE - 1], 1);
    OSTaskCreate(CountingEndTask, (void*)0, (void*)&TaskStk[2][TASK_STK_SIZE - 1], 2);
    OSStart();

    return 0;
}
