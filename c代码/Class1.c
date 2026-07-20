/*==============================================================================
 *  微机与接口课程设计 —— 基于 8031 的多功能实时钟系统
 *------------------------------------------------------------------------------
 *  硬件平台 : 8031(80C31) + 8255A + LCD1602 + DS1302 + 24C02 + 4×按键
 *  晶振频率 : 6MHz (机器周期 2us，定时器 0.1s 一次到位)
 *  开发环境 : Keil C51 / Proteus 仿真
 *------------------------------------------------------------------------------
 *  功能说明 :
 *    基本  : DS1302 授时，8255 驱动 LCD1602 显示 时:分:秒
 *    扩展1 : 按键清除 / 启停
 *    扩展2 : 按键预置(换挡+加数，当前设置位闪烁提示)
 *    扩展5 : DS1302 显示年月日、星期
 *    扩展7 : 采用 LCD1602 字符液晶显示
 *    扩展10: 24C02(IIC) 备份关键数据，掉电重新上电后从故障时刻状态恢复
 *------------------------------------------------------------------------------
 *  引脚分配 :
 *    8255A : D0-D7<->P0 ; A0<-P2.0 ; A1<-P2.1 ; /CS<-P2.7 ; /WR<-P3.6 ; /RD<-P3.7
 *    LCD   : D0-D7<-8255 PA ; RS<-PC0 ; E<-PC1 ; RW<-GND
 *    DS1302: I/O<-P1.0 ; SCLK<-P1.1 ; CE<-P1.2
 *    24C02 : SCL<-P1.3 ; SDA<-P1.4 (各 4.7k 上拉)
 *    按键  : 清除<-P3.2 ; 启停<-P3.3 ; 换挡<-P3.4 ; 加数<-P3.5
 *============================================================================*/

#include <reg51.h>
#include <absacc.h>     // 提供 XBYTE，用于绝对地址访问外部空间(8255)
#include <intrins.h>    // 提供 _nop_()，产生一个机器周期的短延时

 /*----------------------------- 8255A 端口地址 -------------------------------*/
 /* 采用 P2 高位地址直接译码(/CS=P2.7,A1=P2.1,A0=P2.0)，无需锁存低 8 位地址   */
#define PA8255   XBYTE[0x7C00]   // PA 口：LCD 数据线
#define PC8255   XBYTE[0x7E00]   // PC 口：LCD 控制线 (bit0=RS, bit1=E)
#define CTL8255  XBYTE[0x7F00]   // 控制口：写方式控制字

/*----------------------------- DS1302 寄存器地址 ----------------------------*/
/* 写地址为偶数(bit0=0)，读地址=写地址+1(bit0=1)，数据均为 BCD 码            */
#define DS_SEC_W    0x80        // 秒(写)  bit7=CH 时钟暂停位
#define DS_MIN_W    0x82        // 分(写)
#define DS_HOUR_W   0x84        // 时(写)
#define DS_DATE_W   0x86        // 日(写)
#define DS_MONTH_W  0x88        // 月(写)
#define DS_WEEK_W   0x8A        // 星期(写)
#define DS_YEAR_W   0x8C        // 年(写)
#define DS_WP_W     0x8E        // 写保护(写) bit7=WP

#define DS_SEC_R    0x81        // 秒(读)
#define DS_MIN_R    0x83        // 分(读)
#define DS_HOUR_R   0x85        // 时(读)
#define DS_DATE_R   0x87        // 日(读)
#define DS_MONTH_R  0x89        // 月(读)
#define DS_WEEK_R   0x8B        // 星期(读)
#define DS_YEAR_R   0x8D        // 年(读)

/*----------------------------- 24C02 存储单元分配 ---------------------------*/
#define E2_MAGIC     0x00       // 魔术字：判断 EEPROM 是否已存过有效数据
#define E2_COUNT     0x01       // 启动次数
#define E2_RUNSTATE  0x02       // 运行状态：0=运行 1=暂停
#define E2_HOUR      0x03       // 时备份(BCD)
#define E2_MIN       0x04       // 分备份(BCD)
#define E2_SEC       0x05       // 秒备份(BCD，不含 CH 位)
#define MAGIC_VALUE  0x5A       // 魔术字约定值

/*----------------------------- 引脚定义 -------------------------------------*/
sbit DS_IO = P1 ^ 0;         // DS1302 数据线 I/O
sbit DS_SCLK = P1 ^ 1;         // DS1302 时钟线 SCLK
sbit DS_CE = P1 ^ 2;         // DS1302 使能线 CE

sbit I2C_SCL = P1 ^ 3;         // 24C02 时钟线 SCL
sbit I2C_SDA = P1 ^ 4;         // 24C02 数据线 SDA

sbit KEY_CLR = P3 ^ 2;        // 清除键
sbit KEY_SS = P3 ^ 3;        // 启停键
sbit KEY_MODE = P3 ^ 4;        // 换挡键
sbit KEY_ADD = P3 ^ 5;        // 加数键

/*----------------------------- 全局变量 -------------------------------------*/
unsigned char boot_count = 0;          // 系统启动次数
unsigned char set_mode = 0;          // 预置状态：0正常 1设时 2设分 3设秒
unsigned char ISR_1s = 0;          // 定时中断分频计数(刷新用)
unsigned char blink_cnt = 0;          // 定时中断分频计数(闪烁用)
volatile bit  bRefresh = 0;          // 显示刷新标志(0.2s 置位)
volatile bit  Blink = 1;          // 闪烁标志(0.4s 翻转)
unsigned char sec_top;                 // 设秒时临时变量(保留 CH 位)

/*----------------------------- 函数声明 -------------------------------------*/
void delay_ms(unsigned int n);
void I8255_Init(void);

void Lcd_WriteCmd(unsigned char cmd);
void Lcd_WriteData(unsigned char dat);
void Lcd_Init(void);
void Lcd_Show2(unsigned char bcd, bit hide);
void Lcd_ShowTime(void);
void Lcd_ShowDate(void);

void          DS1302_WByte(unsigned char dat);
unsigned char DS1302_RByte(void);
void          DS1302_Write(unsigned char addr, unsigned char dat);
unsigned char DS1302_Read(unsigned char addr);
void          DS1302_Init(void);
void          Stop_DSTime(void);
void          Start_DSTime(void);

void Key_Scan(void);
unsigned char BCD_To_Dec(unsigned char dat);
unsigned char Dec_To_BCD(unsigned char dat);

void          I2C_Start(void);
void          I2C_Stop(void);
bit           I2C_WriteByte(unsigned char dat);
unsigned char I2C_ReadByte(bit ack);
void          E2_Write(unsigned char addr, unsigned char dat);
unsigned char E2_Read(unsigned char addr);

void Restore_FromE2(void);
void Backup_ToE2(void);

/*==============================================================================
 *  主函数
 *============================================================================*/
void main(void)
{
    I8255_Init();               // 初始化 8255(三口输出，方式0)
    Lcd_Init();                 // 初始化 LCD1602
    Restore_FromE2();           // 从 24C02 判断首次/恢复：首次设初值，否则恢复状态

    /* 定时器 T0 方式1，6MHz 下装 0.1s 初值 */
    TMOD = 0x01;
    EA = 1;                   // 开总中断
    ET0 = 1;                   // 开 T0 中断
    TH0 = 0x3C;                // 0.1s 初值高字节 (65536-50000=0x3CB0)
    TL0 = 0xB0;                // 0.1s 初值低字节
    TR0 = 1;                   // 启动 T0

    /* 第一行行末显示启动次数(十进制两位) */
    Lcd_WriteCmd(0x80 | 0x09);
    Lcd_WriteData((boot_count / 10) + '0');
    Lcd_WriteData((boot_count % 10) + '0');

    while (1)
    {
        Key_Scan();             // 扫描按键

        if (bRefresh)           // 0.2s 到，刷新显示
        {
            bRefresh = 0;
            Lcd_ShowTime();     // 第一行：时:分:秒(读 DS1302)
            Lcd_ShowDate();     // 第二行：年-月-日 星期(读 DS1302)
        }
    }
}

/*==============================================================================
 *  基础功能：延时、8255 初始化、定时中断
 *============================================================================*/

 /* 毫秒级软件延时(6MHz 下约 n ms) */
void delay_ms(unsigned int n)
{
    unsigned int i, j;
    for (i = 0; i < n; i++)
        for (j = 0; j < 120; j++)
            ;
}

/* 8255 初始化：控制字 0x80，PA/PB/PC 均为方式0 输出 */
void I8255_Init(void)
{
    CTL8255 = 0x80;
}

/* 定时器 T0 中断：每 0.1s 一次。仅重装初值 + 维护刷新/闪烁节拍(中断保持极短) */
void T0_ISR(void) interrupt 1
{
    TH0 = 0x3C;
    TL0 = 0xB0;

    if (++ISR_1s >= 2)          // 0.1s × 2 = 0.2s 刷新一次显示
    {
        ISR_1s = 0;
        bRefresh = 1;
    }
    if (++blink_cnt >= 4)       // 0.1s × 4 = 0.4s 翻转一次闪烁标志
    {
        blink_cnt = 0;
        Blink = !Blink;
    }
}

/*==============================================================================
 *  LCD1602 驱动(经 8255，RW 已接地为只写方式)
 *============================================================================*/

 /* 写命令：RS=0；数据放 PA，E 高→低，下降沿锁存 */
void Lcd_WriteCmd(unsigned char cmd)
{
    PA8255 = cmd;
    PC8255 = 0x02;              // RS=0, E=1
    _nop_(); _nop_();
    PC8255 = 0x00;              // RS=0, E=0  下降沿锁存
    delay_ms(1);               // 等待指令执行(清屏/复位类较慢)
}

/* 写数据：RS=1；其余同写命令 */
void Lcd_WriteData(unsigned char dat)
{
    PA8255 = dat;
    PC8255 = 0x03;             // RS=1, E=1
    _nop_(); _nop_();
    PC8255 = 0x01;             // RS=1, E=0  下降沿锁存
    delay_ms(1);
}

/* LCD 上电初始化序列 */
void Lcd_Init(void)
{
    delay_ms(20);              // 上电等待内部复位稳定(>15ms)
    Lcd_WriteCmd(0x38);        // 8位数据/2行/5x8点阵
    Lcd_WriteCmd(0x0C);        // 开显示、关光标
    Lcd_WriteCmd(0x06);        // 写入后地址自增
    Lcd_WriteCmd(0x01);        // 清屏
    delay_ms(2);               // 清屏指令较慢(>1.5ms)
}

/* 显示两位 BCD；hide=1 时显示两个空格(用于预置闪烁的灭阶段) */
void Lcd_Show2(unsigned char bcd, bit hide)
{
    if (hide)
    {
        Lcd_WriteData(' ');
        Lcd_WriteData(' ');
    }
    else
    {
        Lcd_WriteData((bcd >> 4) + '0');   // 高 4 位=十位
        Lcd_WriteData((bcd & 0x0F) + '0');   // 低 4 位=个位
    }
}

/* 第一行显示 时:分:秒(读 DS1302，BCD 直接拆位)。设置中的位闪烁 */
void Lcd_ShowTime(void)
{
    unsigned char h, m, s;
    h = DS1302_Read(DS_HOUR_R);
    m = DS1302_Read(DS_MIN_R);
    s = DS1302_Read(DS_SEC_R) & 0x7F;        // 屏蔽 CH 位再显示

    Lcd_WriteCmd(0x80);                       // 定位第一行开头
    Lcd_Show2(h, (set_mode == 1) && (Blink == 0));   // 设时：灭阶段隐藏
    Lcd_WriteData(':');
    Lcd_Show2(m, (set_mode == 2) && (Blink == 0));   // 设分
    Lcd_WriteData(':');
    Lcd_Show2(s, (set_mode == 3) && (Blink == 0));   // 设秒
}

/* 第二行显示 20年-月-日 星期(读 DS1302，BCD 直接拆位) */
void Lcd_ShowDate(void)
{
    unsigned char d, mo, y, w;
    d = DS1302_Read(DS_DATE_R);
    mo = DS1302_Read(DS_MONTH_R);
    y = DS1302_Read(DS_YEAR_R);
    w = DS1302_Read(DS_WEEK_R) & 0x07;

    Lcd_WriteCmd(0xC0);                        // 定位第二行开头
    Lcd_WriteData('2'); Lcd_WriteData('0');
    Lcd_WriteData((y >> 4) + '0');  Lcd_WriteData((y & 0x0F) + '0');  Lcd_WriteData('-');
    Lcd_WriteData((mo >> 4) + '0');  Lcd_WriteData((mo & 0x0F) + '0');  Lcd_WriteData('-');
    Lcd_WriteData((d >> 4) + '0');  Lcd_WriteData((d & 0x0F) + '0');
    Lcd_WriteData(' '); Lcd_WriteData('W'); Lcd_WriteData(w + '0');     // 星期 W1~W7
}

/*==============================================================================
 *  DS1302 三线串行驱动(LSB 先出)
 *============================================================================*/

 /* 发送一个字节：数据在 SCLK 上升沿被 DS1302 锁存，每位以 SCLK=0 收尾 */
void DS1302_WByte(unsigned char dat)
{
    unsigned char i;
    for (i = 0; i < 8; i++)
    {
        DS_IO = dat & 0x01;    // 取最低位(LSB 先出)
        _nop_();
        DS_SCLK = 1;           // 上升沿：DS1302 读走该位
        _nop_();
        DS_SCLK = 0;           // 复位时钟，保证相位整齐
        dat >>= 1;
    }
}

/* 读取一个字节：读前先释放 I/O 为输入，数据在 SCLK 下降沿后有效(LSB 先进) */
unsigned char DS1302_RByte(void)
{
    unsigned char i, dat = 0;
    DS_IO = 1;                 // 准双向口读前置1，释放为输入
    for (i = 0; i < 8; i++)
    {
        if (DS_IO) dat |= (1 << i);   // 先读当前位，LSB 先进放到第 i 位
        DS_SCLK = 1;
        _nop_();
        DS_SCLK = 0;                   // 下降沿：DS1302 送出下一位
        _nop_();
    }
    return dat;
}

/* 读某寄存器：CE 拉高→发命令(读)→读数据→CE 拉低 */
unsigned char DS1302_Read(unsigned char addr)
{
    unsigned char dat;
    DS_CE = 0; DS_SCLK = 0;    // 复位到空闲
    DS_CE = 1;                 // 开始通信
    DS1302_WByte(addr);        // 命令字节(bit0=1 读)
    dat = DS1302_RByte();      // 读回数据
    DS_CE = 0;                 // 结束
    return dat;
}

/* 写某寄存器：CE 拉高→发命令(写)→发数据→CE 拉低 */
void DS1302_Write(unsigned char addr, unsigned char dat)
{
    DS_CE = 0; DS_SCLK = 0;
    DS_CE = 1;
    DS1302_WByte(addr);        // 命令字节(bit0=0 写)
    DS1302_WByte(dat);         // 数据字节
    DS_CE = 0;
}

/* 设置初始时间(仅首次上电调用一次)。写寄存器前后需解/上写保护 */
void DS1302_Init(void)
{
    DS1302_Write(DS_WP_W, 0x00);   // 解写保护
    DS1302_Write(DS_SEC_W, 0x00);   // 秒=00，同时 CH=0 启动走时
    DS1302_Write(DS_MIN_W, 0x00);   // 分=00
    DS1302_Write(DS_HOUR_W, 0x12);   // 时=12  (BCD)
    DS1302_Write(DS_WEEK_W, 0x03);   // 星期=3 (BCD)
    DS1302_Write(DS_DATE_W, 0x09);   // 日=09  (BCD)
    DS1302_Write(DS_MONTH_W, 0x07);   // 月=07  (BCD)
    DS1302_Write(DS_YEAR_W, 0x26);   // 年=26  (BCD，即 2026)
    DS1302_Write(DS_WP_W, 0x80);   // 上写保护，防误写
}

/* 暂停走时：置秒寄存器 CH 位(bit7)=1，保留秒值 */
void Stop_DSTime(void)
{
    unsigned char sec = DS1302_Read(0x81);   // 读回，bit7=CH
    sec |= 0x80;                             // 置 CH=1
    DS1302_Write(0x8E, 0x00);                // 解写保护
    DS1302_Write(0x80, sec);                 // 写回(暂停)
    DS1302_Write(0x8E, 0x80);                // 上写保护
}

/* 恢复走时：清秒寄存器 CH 位(bit7)=0，保留秒值 */
void Start_DSTime(void)
{
    unsigned char sec = DS1302_Read(0x81);   // 读回，bit7=CH
    sec &= 0x7F;                             // 清 CH=0
    DS1302_Write(0x8E, 0x00);                // 解写保护
    DS1302_Write(0x80, sec);                 // 写回(走时)
    DS1302_Write(0x8E, 0x80);                // 上写保护
}

/*==============================================================================
 *  BCD / 十进制转换(预置时的加减运算需要)
 *============================================================================*/
unsigned char BCD_To_Dec(unsigned char dat)
{
    return (dat >> 4) * 10 + (dat & 0x0F);
}

unsigned char Dec_To_BCD(unsigned char dat)
{
    return (dat / 10 << 4) | (dat % 10);
}

/*==============================================================================
 *  按键扫描(查询方式，含去抖)
 *  去抖三段式：检测低电平 → 延时确认 → 等待松手，保证一次按下只触发一次
 *============================================================================*/
void Key_Scan(void)
{
    /*--------- 清除键：时分秒写 0，回到 00:00:00 并继续走 ---------*/
    if (KEY_CLR == 0)
    {
        delay_ms(10);
        if (KEY_CLR == 0)
        {
            DS1302_Write(0x8E, 0x00);   // 解写保护
            DS1302_Write(0x80, 0x00);   // 秒=0 (CH=0 继续走)
            DS1302_Write(0x82, 0x00);   // 分=0
            DS1302_Write(0x84, 0x00);   // 时=0
            DS1302_Write(0x8E, 0x80);   // 上写保护
            while (KEY_CLR == 0);       // 等松手
        }
        Backup_ToE2();                  // 状态变化，备份到 24C02
    }

    /*--------- 启停键：翻转 CH 位，暂停/恢复走时 ---------*/
    if (KEY_SS == 0)
    {
        delay_ms(10);
        if (KEY_SS == 0)
        {
            unsigned char sec = DS1302_Read(0x81);   // 读回，bit7=CH
            sec ^= 0x80;                             // 异或翻转 CH 位
            DS1302_Write(0x8E, 0x00);                // 解写保护
            DS1302_Write(0x80, sec);                 // 写回
            DS1302_Write(0x8E, 0x80);                // 上写保护
            while (KEY_SS == 0);                     // 等松手
            Backup_ToE2();                           // 备份
        }
    }

    /*--------- 换挡键：切换设置位 0→1→2→3→0，进/出设置时暂停/恢复 ---------*/
    if (KEY_MODE == 0)
    {
        delay_ms(10);
        if (KEY_MODE == 0)
        {
            set_mode = (set_mode + 1) % 4;
            if (set_mode == 1) Stop_DSTime();    // 进入设置：暂停走时
            if (set_mode == 0) Start_DSTime();   // 退出设置：恢复走时
            while (KEY_MODE == 0);               // 等松手
        }
    }

    /*--------- 加数键：给当前选中位 +1 并按上限回绕，写回 DS1302 ---------*/
    if (KEY_ADD == 0)
    {
        delay_ms(10);
        if (KEY_ADD == 0)
        {
            if (set_mode == 1)          // 设时：0~23
            {
                unsigned char hour = BCD_To_Dec(DS1302_Read(DS_HOUR_R));
                hour += 1;
                if (hour >= 24) hour = 0;
                DS1302_Write(0x8E, 0x00);
                DS1302_Write(DS_HOUR_W, Dec_To_BCD(hour));
                DS1302_Write(0x8E, 0x80);
            }
            if (set_mode == 2)          // 设分：0~59
            {
                unsigned char min = BCD_To_Dec(DS1302_Read(DS_MIN_R));
                min += 1;
                if (min >= 60) min = 0;
                DS1302_Write(0x8E, 0x00);
                DS1302_Write(DS_MIN_W, Dec_To_BCD(min));
                DS1302_Write(0x8E, 0x80);
            }
            if (set_mode == 3)          // 设秒：0~59，写回时保留 CH=1(暂停中)
            {
                unsigned char sec = BCD_To_Dec(DS1302_Read(DS_SEC_R) & 0x7F);
                sec += 1;
                if (sec >= 60) sec = 0;
                sec_top = Dec_To_BCD(sec);
                sec_top |= 0x80;        // 保留暂停状态
                DS1302_Write(0x8E, 0x00);
                DS1302_Write(DS_SEC_W, sec_top);
                DS1302_Write(0x8E, 0x80);
            }
            while (KEY_ADD == 0);       // 等松手
            Backup_ToE2();              // 备份
        }
    }
}

/*==============================================================================
 *  24C02 软件 IIC 驱动(MSB 先出，与 DS1302 相反)
 *============================================================================*/

 /* 起始信号：SCL 高电平期间 SDA 由高变低 */
void I2C_Start(void)
{
    I2C_SDA = 1; I2C_SCL = 1; _nop_();
    I2C_SDA = 0; _nop_();
    I2C_SCL = 0;
}

/* 停止信号：SCL 高电平期间 SDA 由低变高 */
void I2C_Stop(void)
{
    I2C_SDA = 0; I2C_SCL = 1; _nop_();
    I2C_SDA = 1; _nop_();
}

/* 发送一个字节(MSB 先出)，返回从机应答位(0=ACK) */
bit I2C_WriteByte(unsigned char dat)
{
    unsigned char i;
    bit ack;
    for (i = 0; i < 8; i++)
    {
        I2C_SDA = (dat & 0x80);    // 取最高位(MSB 先出)
        I2C_SCL = 1; _nop_();      // 拉高，从机采样
        I2C_SCL = 0;
        dat <<= 1;
    }
    I2C_SDA = 1;                   // 释放 SDA，交给从机应答
    I2C_SCL = 1; _nop_();          // 第 9 个时钟
    ack = I2C_SDA;                 // 读应答(0=ACK)
    I2C_SCL = 0;
    return ack;
}

/* 读取一个字节(MSB 先进)。ack=0 发 ACK(续读)，ack=1 发 NACK(结束) */
unsigned char I2C_ReadByte(bit ack)
{
    unsigned char i, dat = 0;
    I2C_SDA = 1;                   // 释放 SDA 为输入
    for (i = 0; i < 8; i++)
    {
        I2C_SCL = 1; _nop_();      // 拉高，数据稳定
        dat <<= 1;                 // 左移(MSB 先进)
        if (I2C_SDA) dat |= 0x01;  // 读一位
        I2C_SCL = 0; _nop_();      // 拉低，从机送下一位
    }
    I2C_SDA = ack;                 // 主机应答：0=ACK 1=NACK
    I2C_SCL = 1; _nop_();
    I2C_SCL = 0;
    I2C_SDA = 1;                   // 释放总线
    return dat;
}

/* 向 24C02 指定单元写一个字节(写后需等内部写入周期) */
void E2_Write(unsigned char addr, unsigned char dat)
{
    I2C_Start();
    I2C_WriteByte(0xA0);           // 器件地址+写
    I2C_WriteByte(addr);           // 单元地址
    I2C_WriteByte(dat);            // 数据
    I2C_Stop();
    delay_ms(5);                   // 等待内部写入周期(约5ms)
}

/* 从 24C02 指定单元读一个字节(复合帧：先写地址，重复起始后转读) */
unsigned char E2_Read(unsigned char addr)
{
    unsigned char dat;
    I2C_Start();
    I2C_WriteByte(0xA0);           // 器件地址+写
    I2C_WriteByte(addr);           // 单元地址
    I2C_Start();                   // 重复起始
    I2C_WriteByte(0xA1);           // 器件地址+读
    dat = I2C_ReadByte(1);         // 读一个字节，发 NACK
    I2C_Stop();
    return dat;
}

/*==============================================================================
 *  掉电备份与恢复
 *============================================================================*/

 /* 备份：把运行状态与当前时间(BCD)存入 24C02。仅在按键改变状态后调用 */
void Backup_ToE2(void)
{
    unsigned char sec_reg = DS1302_Read(DS_SEC_R);        // 含 CH 位

    E2_Write(E2_RUNSTATE, (sec_reg & 0x80) ? 1 : 0);      // CH 位→运行/暂停
    E2_Write(E2_HOUR, DS1302_Read(DS_HOUR_R));            // 时(BCD)
    E2_Write(E2_MIN, DS1302_Read(DS_MIN_R));             // 分(BCD)
    E2_Write(E2_SEC, sec_reg & 0x7F);                    // 秒(去 CH 位)
}

/* 上电恢复：读魔术字判断首次/非首次
 *   非首次：启动次数+1、恢复运行/暂停状态(DS1302 靠电池保持时间，不再重设)
 *   首次  ：设 DS1302 初值、写魔术字、次数置 1
 * (注意 DS1302_Init 只在首次调用，否则每次上电都会抹掉电池保持的时间) */
void Restore_FromE2(void)
{
    unsigned char magic = E2_Read(E2_MAGIC);

    if (magic == MAGIC_VALUE)              // 非首次：曾正常运行过
    {
        boot_count = E2_Read(E2_COUNT) + 1;
        if (E2_Read(E2_RUNSTATE) == 1)     // 上次为暂停 → 恢复暂停
            Stop_DSTime();
        // 上次为运行则无需处理，DS1302 本就在走
    }
    else                                   // 首次上电
    {
        DS1302_Init();                     // 设初始时间(仅此一次)
        E2_Write(E2_MAGIC, MAGIC_VALUE);   // 写入魔术字
        boot_count = 1;
    }
    E2_Write(E2_COUNT, boot_count);        // 更新启动次数
    Backup_ToE2();                         // 存一份当前状态打底
}

