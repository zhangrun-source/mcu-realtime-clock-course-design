#include <reg51.h>
#include <absacc.h>
#include <intrins.h>    // ??? _nop_()

#define PA8255   XBYTE[0x7C00]   // LCD ???
#define PC8255   XBYTE[0x7E00]   // LCD ??? (bit0=RS, bit1=E)
#define CTL8255  XBYTE[0x7F00]   // ???

#define DS_SEC_W    0x80
#define DS_MIN_W    0x82
#define DS_HOUR_W   0x84
#define DS_DATE_W   0x86
#define DS_MONTH_W  0x88
#define DS_WEEK_W    0x8A
#define DS_YEAR_W   0x8C
#define DS_WP_W     0x8E

#define DS_SEC_R    0x81
#define DS_MIN_R    0x83
#define DS_HOUR_R   0x85
#define DS_DATE_R   0x87
#define DS_MONTH_R  0x89
#define DS_WEEK_R    0x8B
#define DS_YEAR_R   0x8D

#define E2_MAGIC     0x00    // ???
#define E2_COUNT     0x01    // ????
#define E2_RUNSTATE  0x02    // ??(0)/??(1)
#define E2_HOUR      0x03    // ?(BCD)
#define E2_MIN       0x04    // ?(BCD)
#define E2_SEC       0x05    // ?(BCD,??CH)
#define MAGIC_VALUE  0x5A

unsigned char boot_count = 0;   // ??



unsigned char ISR_1s = 0;
volatile bit bRefresh = 0;    // ? 0.5s ??,?????????
unsigned char blink_cnt = 0;
volatile bit Blink = 1;

sbit DS_IO = P1 ^ 0;   //I/O?P1.0 
sbit DS_SCLK = P1 ^ 1;   // SCLK?P1.1 
sbit DS_CE = P1 ^ 2;   //  CE?P1.2 

sbit KEY_CLR = P3 ^ 2;   // ??
sbit KEY_SS = P3 ^ 3;   // ??
sbit KEY_MODE = P3 ^ 4;   // ???
sbit KEY_ADD = P3 ^ 5;   // ???

sbit I2C_SCL = P1 ^ 3;
sbit I2C_SDA = P1 ^ 4;

unsigned char set_mode = 0;

volatile unsigned char g_sec;
volatile unsigned char g_min;
volatile unsigned char g_hour;

// ???? //
void Lcd_Init(void);
void delay_ms(unsigned int n);
void I8255_Init(void);

void Lcd_WriteCmd(unsigned char cmd);
void Lcd_WriteData(unsigned char dat);
void Lcd_ShowTime(void);
void Lcd_ShowDate(void);

void DS1302_WByte(unsigned char dat);
unsigned char DS1302_RByte(void);
void DS1302_Init(void);
void DS1302_Write(unsigned char addr, unsigned char dat);
unsigned char DS1302_Read(unsigned char addr);

void Key_Scan(void);

unsigned char BCD_To_Dec(unsigned char dat);
unsigned char Dec_To_BCD(unsigned char dat);

void Stop_DSTime(void);
void Start_DSTime(void);

void Lcd_Show2(unsigned char bcd, bit hide);

bit I2C_WriteByte(unsigned char dat);
unsigned char I2C_ReadByte(bit ack);
void E2_Write(unsigned char addr, unsigned char dat);
unsigned char E2_Read(unsigned char addr);

void Restore_FromE2(void);
void Backup_ToE2(void);


// ??? //
void main (void)
{
    I8255_Init();
    Lcd_Init();
    Restore_FromE2();


    TMOD = 0x01;
    EA = 1;
    ET0 = 1;
    TH0 = 0x3C; // 6MHZ,2um,0.1s????
    TL0 = 0xB0;
    TR0 = 1;

  
    Lcd_WriteCmd(0x80 | 0x09);
    Lcd_WriteData((boot_count / 10) + '0');
    Lcd_WriteData((boot_count % 10) + '0');

    while (1)
    {
        Key_Scan();

        if (bRefresh) {            // 0.2s ????(ISR ???)
            bRefresh = 0;
            Lcd_ShowTime();        // ??? DS1302 ??(????)
            Lcd_ShowDate();        // ??? DS1302 ??(?????)
        }
    }
}

void delay_ms(unsigned int n)
{          // ? 2ms @6MHz(??)
    unsigned int i, j;
    for (i = 0; i < n; i++) for (j = 0; j < 120; j++) ;
}

void I8255_Init(void) { CTL8255 = 0x80; }  // PA/PB/PC ???,??0

//?????0.1s????//
void T0_ISR(void) interrupt 1
{
    TH0 = 0x3C;
    TL0 = 0xB0;
    if (++ISR_1s >= 2) {        // 0.2s ??
        ISR_1s = 0;
        bRefresh = 1;
    }
    if (++blink_cnt >= 4) {     // 0.4s ????
        blink_cnt = 0;
        Blink = !Blink;         // ??
    }
 }

//LCD????//
void Lcd_WriteCmd(unsigned char cmd)
{
    PA8255 = cmd;
    PC8255 = 0x02;      // RS=0, E=1
    _nop_(); _nop_();
    PC8255 = 0x00;      // RS=0, E=0  ? ??????
    delay_ms(1);        // ???????(??/??????)
}

//LCD??//
void Lcd_WriteData(unsigned char dat)
{
    PA8255 = dat;
    PC8255 = 0x03;      // RS=1, E=1
    _nop_(); _nop_();
    PC8255 = 0x01;      // RS=1, E=0
    delay_ms(1);
}


//LCD???//
void Lcd_Init(void)
{
    delay_ms(20);          // ??? LCD ?????? (>15ms)
    Lcd_WriteCmd(0x38);    // 8?/2?/5x8??
    Lcd_WriteCmd(0x0C);    // ???, ???
    Lcd_WriteCmd(0x06);    // ???????
    Lcd_WriteCmd(0x01);    // ??
    delay_ms(2);           // ?????, ???? (>1.5ms)
}


//DS1302
void DS1302_WByte(unsigned char dat)
{
    unsigned char i;
    for (i = 0; i < 8; i++)
    {
        DS_IO = dat & 0x01;
        _nop_();
        DS_SCLK = 1;          // ?????
        _nop_();
        DS_SCLK = 0;          // ? ??0,????/????
        dat >>= 1;
    }
}

unsigned char DS1302_RByte(void)
{
    unsigned char i, dat = 0;
    DS_IO = 1;                // ? ????I/O???
    for (i = 0; i < 8; i++)
    {
        if (DS_IO) dat |= (1 << i);   // ??(LSB??,?i?)
        DS_SCLK = 1;
        _nop_();
        DS_SCLK = 0;                   // ???????
        _nop_();
    }
    return dat;
}

unsigned char DS1302_Read(unsigned char addr)
{
    unsigned char dat;
    DS_CE = 0; DS_SCLK = 0;    // ?????
    DS_CE = 1;                 // ????
    DS1302_WByte(addr);        // ???(?"?"??)
    dat = DS1302_RByte();      // ????
    DS_CE = 0;                 // ??
    return dat;
}

void DS1302_Write(unsigned char addr, unsigned char dat)
{
    DS_CE = 0; DS_SCLK = 0;
    DS_CE = 1;
    DS1302_WByte(addr);    // addr ? bit0=0 ??"?"
    DS1302_WByte(dat);     // ??????
    DS_CE = 0;
}

void DS1302_Init(void)
{
    DS1302_Write(DS_WP_W, 0x00);     // ????? (WP=0)
    DS1302_Write(DS_SEC_W, 0x00);    // ?????0:CH?=0 ? ????
    DS1302_Write(DS_MIN_W, 0x00);    // ?(BCD)
    DS1302_Write(DS_HOUR_W, 0x12);   // ?(BCD,?? 12)
    DS1302_Write(DS_WEEK_W, 0x03);
    DS1302_Write(DS_DATE_W, 0x09);   // ?(BCD)
    DS1302_Write(DS_MONTH_W, 0x07);  // ?(BCD)
    DS1302_Write(DS_YEAR_W, 0x26);   // ?(BCD,?? 2026 -> 0x26)
    DS1302_Write(DS_WP_W, 0x80);     // ??????,???
}

// ? DS1302 ???????,??????? Mon..Sun
void Lcd_ShowDate(void)
{
    unsigned char d, mo, y, w;
    d = DS1302_Read(DS_DATE_R);
    mo = DS1302_Read(DS_MONTH_R);
    y = DS1302_Read(DS_YEAR_R);
    w = DS1302_Read(DS_WEEK_R) & 0x07;

    Lcd_WriteCmd(0xC0);
    Lcd_WriteData('2'); Lcd_WriteData('0');
    Lcd_WriteData((y >> 4) + '0');   Lcd_WriteData((y & 0x0F) + '0');   Lcd_WriteData('-');
    Lcd_WriteData((mo >> 4) + '0');  Lcd_WriteData((mo & 0x0F) + '0');  Lcd_WriteData('-');
    Lcd_WriteData((d >> 4) + '0');   Lcd_WriteData((d & 0x0F) + '0');
    Lcd_WriteData(' '); Lcd_WriteData('W'); Lcd_WriteData(w + '0');
}

void Lcd_ShowTime(void)
{
    unsigned char h, m, s;
    h = DS1302_Read(DS_HOUR_R);
    m = DS1302_Read(DS_MIN_R);
    s = DS1302_Read(DS_SEC_R) & 0x7F;

    Lcd_WriteCmd(0x80);
    Lcd_Show2(h, (set_mode == 1) && (Blink == 0));   // ?????????
    Lcd_WriteData(':');
    Lcd_Show2(m, (set_mode == 2) && (Blink == 0));   // ??
    Lcd_WriteData(':');
    Lcd_Show2(s, (set_mode == 3) && (Blink == 0));   // ??
}

unsigned char sec_top;

void Key_Scan(void)
{
    if (KEY_CLR == 0)              // ????????(???)
    {
        delay_ms(10);             // ??:?20ms
        if (KEY_CLR == 0)         // ?????,???????
        {
            // TODO: ??"??"——? 00:00:00
            // ??/?/???0(???????)
            DS1302_Write(0x8E, 0x00);   // ????
            DS1302_Write(0x80, 0x00);   // ?=0 (CH=0???)
            DS1302_Write(0x82, 0x00);   // ?=0
            DS1302_Write(0x84, 0x00);   // ?=0
            DS1302_Write(0x8E, 0x80);   // ????
            while (KEY_CLR == 0); // ???(??????????)
        }
        Backup_ToE2();//??
    }

    if (KEY_SS == 0)              // ???
    {
        delay_ms(10);
        if (KEY_SS == 0)
        {
            // TODO: ??"??"——??CH?
            // ????? ? ??bit7 ? ??
            unsigned char sec = DS1302_Read(0x81);   // ???bit7??CH
            sec ^= 0x80;                             // ??0x80 = ?????
            DS1302_Write(0x8E, 0x00);                // ????
            DS1302_Write(0x80, sec);                 // ??(CH????)
            DS1302_Write(0x8E, 0x80);                // ????
            while (KEY_SS == 0);  // ???
            Backup_ToE2();//??
        }        
    }

    // —— ???:????? ——
    if (KEY_MODE == 0)
    {
        delay_ms(10);
        if (KEY_MODE == 0)
        {
            set_mode = (set_mode + 1) % 4;   // 0?1?2?3?0

            if (set_mode == 1) 
            {               
                // ?????:???? CH=1
                // TODO: ????bit7???
                Stop_DSTime();
            }
            if (set_mode == 0)
            {                
                // ????:???? CH=0
                // TODO: ????bit7???
                Start_DSTime();  
            }
            while (KEY_MODE == 0);
        }
    }

    // —— ???:????+1 ——
    if (KEY_ADD == 0)
    {
        delay_ms(10);
        if (KEY_ADD == 0)
        {
            if (set_mode == 1) 
            { /* TODO: ?+1, ?24?0, ?? */ 
                unsigned char hour = BCD_To_Dec(DS1302_Read(DS_HOUR_R));
                hour += 1;
                if (hour >= 24)hour = 0;
                DS1302_Write(0x8E, 0x00);                // ????
                DS1302_Write(DS_HOUR_W, Dec_To_BCD(hour));                 // ??
                DS1302_Write(0x8E, 0x80);                // ????
            }
            if (set_mode == 2) 
            { /* TODO: ?+1, ?60?0, ?? */
                unsigned char min = BCD_To_Dec(DS1302_Read(DS_MIN_R));
                min += 1;
                if (min >= 60)min = 0;
                DS1302_Write(0x8E, 0x00);                // ????
                DS1302_Write(DS_MIN_W, Dec_To_BCD(min));                 // ??
                DS1302_Write(0x8E, 0x80);
            }
            if (set_mode == 3)
            { /* TODO: ?+1, ?60?0, ?? */ 
                unsigned char sec = BCD_To_Dec(DS1302_Read(DS_SEC_R) & 0x7F);
                sec += 1;
                if (sec >= 60)sec = 0;
                sec_top = Dec_To_BCD(sec);
                sec_top |= 0x80;
                DS1302_Write(0x8E, 0x00);                // ????
                DS1302_Write(DS_SEC_W, sec_top);                 // ??
                DS1302_Write(0x8E, 0x80);
            }
            while (KEY_ADD == 0);
            Backup_ToE2();//??
        }
    }
}


unsigned char BCD_To_Dec(unsigned char dat)
{
    return (dat >> 4) * 10 + (dat & 0x0F);
}

unsigned char Dec_To_BCD(unsigned char dat)
{
    return (dat / 10 << 4) | (dat % 10);
}



void Stop_DSTime (void)
{
    // ?????:???? CH=1
    // TODO: ????bit7???
    unsigned char sec = DS1302_Read(0x81);   // ???bit7??CH
    sec |= 0x80;                             // ?0x80 = ???1
    DS1302_Write(0x8E, 0x00);                // ????
    DS1302_Write(0x80, sec);                 // ??(CH???)

    DS1302_Write(0x8E, 0x80);
}

void Start_DSTime(void) {
    // ????:???? CH=0
    // TODO: ????bit7???
    unsigned char sec = DS1302_Read(0x81);   // ???bit7??CH
    sec &= 0x7F;                             // ?0x7F = ???0
    DS1302_Write(0x8E, 0x00);                // ????
    DS1302_Write(0x80, sec);                 // ??(CH?????)
    DS1302_Write(0x8E, 0x80);
}

// hide=1 ? ??????;????BCD??
void Lcd_Show2(unsigned char bcd, bit hide)
{
    if (hide) { Lcd_WriteData(' '); Lcd_WriteData(' '); }
    else { Lcd_WriteData((bcd >> 4) + '0'); Lcd_WriteData((bcd & 0x0F) + '0'); }
}

void I2C_Start(void)
{
    I2C_SDA = 1; I2C_SCL = 1; _nop_();
    I2C_SDA = 0; _nop_();          // SCL??SDA??=??
    I2C_SCL = 0;
}

void I2C_Stop(void)
{
    I2C_SDA = 0; I2C_SCL = 1; _nop_();
    I2C_SDA = 1; _nop_();          // SCL??SDA??=??
}

// ??????,????ACK(0=??)
bit I2C_WriteByte(unsigned char dat)
{
    unsigned char i;
    bit ack;
    for (i = 0; i < 8; i++) {
        I2C_SDA = (dat & 0x80);    // I2C?MSB??(?DS1302??!)
        I2C_SCL = 1; _nop_();
        I2C_SCL = 0;
        dat <<= 1;
    }
    I2C_SDA = 1;                    // ??SDA?????
    I2C_SCL = 1; _nop_();           //???9???,??ACK
    ack = I2C_SDA;                  // ?ACK
    I2C_SCL = 0;
    return ack;
}

// ?????,??ack=?????
unsigned char I2C_ReadByte(bit ack)   // ack=0?ACK(???), ack=1?NACK(???)
{
    unsigned char i, dat = 0;

    I2C_SDA = 1;                 // ? ??SDA???(??!)

    for (i = 0; i < 8; i++)
    {
        I2C_SCL = 1;  _nop_();   // ? ??,??????
        dat <<= 1;               // ? ??(MSB??)
        if (I2C_SDA) dat |= 0x01;// ? ???
        I2C_SCL = 0;  _nop_();   // ? ??,???????
    }

    // ?????(?9???)
  
    I2C_SDA = ack;  // (ack=1?NACK, ack=0?ACK)  ??????ack
    I2C_SCL = 1; _nop_();
    I2C_SCL = 0;
    I2C_SDA = 1;    // (????)

    return dat;
}

void E2_Write(unsigned char addr, unsigned char dat)
{
    I2C_Start();
    I2C_WriteByte(0xA0);    // ??:???0xA0??,???
    I2C_WriteByte(addr);    // ???:???addr???
    I2C_WriteByte(dat);     // ??
    I2C_Stop();
    delay_ms(5);            // ????????
}

unsigned char E2_Read(unsigned char addr)
{
    unsigned char dat;
    I2C_Start();
    I2C_WriteByte(0xA0);      // ???
    I2C_WriteByte(addr);      // ????
    I2C_Start();              // ????
    I2C_WriteByte(0xA1);      // ???
    dat = I2C_ReadByte(1);    // ?1???,?NACK(1)
    I2C_Stop();
    return dat;
}





//????//
void Backup_ToE2(void)
{
    unsigned char sec_reg = DS1302_Read(DS_SEC_R);   // ?CH?

    E2_Write(E2_RUNSTATE, (sec_reg & 0x80) ? 1 : 0); // CH????/??
    E2_Write(E2_HOUR, DS1302_Read(DS_HOUR_R));       // ?(BCD???)
    E2_Write(E2_MIN, DS1302_Read(DS_MIN_R));        // ?
    E2_Write(E2_SEC, sec_reg & 0x7F);               // ?(??CH?)
}

// ????//
void Restore_FromE2(void)
{
    unsigned char magic = E2_Read(E2_MAGIC);

    if (magic == MAGIC_VALUE)        // ?? ? ??
    {
        boot_count = E2_Read(E2_COUNT) + 1;

        // ????/????? DS1302 ? CH ?
        if (E2_Read(E2_RUNSTATE) == 1) {
            Stop_DSTime();
            // TODO: ????? ? ? DS1302 ???? CH=1
            //       ??(0x81) ? |0x80 ? ?????0x80????
        }
        // ????(0)????,DS1302????
    }
    else                             // ??
    {
        DS1302_Init();
        E2_Write(E2_MAGIC, MAGIC_VALUE);
        boot_count = 1;
    }
    E2_Write(E2_COUNT, boot_count);
    Backup_ToE2();                   // ?????????
}