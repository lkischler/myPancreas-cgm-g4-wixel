#include <cc2511_map.h>
#include <board.h>
#include <random.h>
#include <time.h>
#include <usb.h>
#include <usb_com.h>
#include <radio_registers.h>
#include <radio_queue.h>
#include <gpio.h>
#include <uart1.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <adc.h>
#include <limits.h>

/* Function prototypes ********************************************************/

void nicedelayMs(int ms);
void usb_printf(const char *format, ...);
void killWithWatchdog();
void loadSettingsFromFlash();

#define myEOF -1

//////////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////////////////////
//                                                                                                  //
//                           SET THESE VARIABLES TO MEET YOUR NEEDS                                 //
//                                   1 = TRUE       0 = FALSE                                       //
//                                                                                                  //

#ifndef CUSTOM_TRANSMITTER_ID
static CODE const char transmitter_id[] = "ABCDE";                                                  //

#define my_webservice_url	"parakeet-receiver.appspot.com/receiver.cgi"
#define my_webservice_reply     "!ACK"
#define my_user_agent 		"xDrip"
#define my_gprs_apn		"apn.check.your.carrier.info"

#define my_udp_server_host	"disabled"
#define my_udp_server_port	"12345"

#define my_control_number       ""

#else
// get user specific configuration from an external untracked file
#include "my_transmitter_id.h"
#endif

//                                                                                                  //
static volatile BIT only_listen_for_my_transmitter = 1;	//
// 1 is recommended                                                                                 //
//	                                                                                                //
static volatile BIT allow_alternate_usb_protocol = 1;
// if set to 1 and plugged in to USB then protocol output is suitable for dexterity and similar
//                                                                                                  //
//..................................................................................................//
//////////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////////////////////
//                                                                                                  //
//                 Advanced Options, dont change unless you know what you are doing                 //
//                                   1 = TRUE       0 = FALSE                                       //
//                                                                                                  //
//                                                                                                  //
static volatile uint8 wake_earlier_for_next_miss = 20;	//
// if a packet is missed, wake this many seconds earlier to try and get the next one                //
// shorter means better bettery life but more likely to miss multiple packets in a row              //
//                                                                                                  //
static volatile uint8 misses_until_failure = 2;	//
// after how many missed packets should we just start a nonstop scan?                               //
// a high value is better for conserving batter life if you go out of wixel range a lot             //
// but it could also mean missing packets for MUCH longer periods of time                           //
// a value of zero is best if you dont care at all about battery life                               //
//                                                                                                  //
//////////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////////////////////

static XDATA volatile int start_channel = 0;
uint32 XDATA asciiToDexcomSrc (char *addr);
uint32 XDATA getSrcValue (char srcVal);
volatile uint32 dex_tx_id;
#define NUM_CHANNELS        (4)
static int8 fOffset[NUM_CHANNELS] = { 0xCE, 0xD5, 0xE6, 0xE5 };
static XDATA int8 defaultfOffset[NUM_CHANNELS] = { 0xCE, 0xD5, 0xE6, 0xE5 };
static uint8 nChannels[NUM_CHANNELS] = { 0, 100, 199, 209 };
static XDATA uint32 waitTimes[NUM_CHANNELS] = { 13500, 500, 500, 500 };
//Now lets try to crank down the channel 1 wait time, if we can 5000 works but it wont catch channel 4 ever
static XDATA uint32 delayedWaitTimes[NUM_CHANNELS] = { 0, 700, 700, 700 };

static uint8 last_catch_channel = 0;
BIT needsTimingCalibration = 1;
BIT usbEnabled = 1;

unsigned char XDATA PM2_BUF[7] = { 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x04 };
unsigned char XDATA dmaDesc[8] =
{ 0x00, 0x00, 0xDF, 0xBE, 0x00, 0x07, 0x20, 0x42 };

static volatile BIT usb_connected = 0;

static uint32 XDATA lastraw = 0;
static uint32 XDATA lastfiltered = 0;
static uint32 XDATA lasttime = 0;
volatile uint8 sequential_missed_packets = 0;

typedef struct _parakeet_settings
{
    uint32 dex_tx_id; 		//4 bytes
    char http_url[56];
    char gsm_lock[16];
    char gsm_apn[32];
    char udp_server[28];
    char udp_port[6];
    int locked:1;
    int deepsleep:1;
    int pin:13;
    uint8 padding3;
    uint32 checksum; // needs to be aligned

} parakeet_settings;

parakeet_settings XDATA settings;
int XDATA loop;
unsigned XDATA char* flash_pointer;
long unsigned int chk;

#define FLASH_SETTINGS 			(0x7760)

typedef struct _Dexcom_packet
{
    uint8 len;
    uint32 dest_addr;
    uint32 src_addr;
    uint8 port;
    uint8 device_info;
    uint8 txId;
    uint16 raw;
    uint16 filtered;
    uint8 battery;
    uint8 unknown;
    uint8 checksum;
    int8 RSSI;
    uint8 LQI;
} Dexcom_packet;

XDATA   int gret = 0;

// Library Routine

uint32 checksum()
{
    chk = 0x12345678;
    flash_pointer = (__xdata unsigned char*)settings;
    for (loop = 0; loop < sizeof(parakeet_settings)-4; loop++)
    {
        chk += (flash_pointer[loop] * (loop + 1));
        chk++;
    }
    return chk;
}

void
strupr (char XDATA *str,char term)
{

    while ((*str) && (*str != term))
    {
        *str = toupper (*str);
        ++str;
    }

}

XDATA char *
xdatstrchr (char XDATA *string, char ch)
{
    while (*string && *string != ch)
        ++string;

    if (*string == ch)
        return string;

    return NULL;
}

XDATA char *
xdatstrstr (char XDATA *str1, char *str2)
{
    XDATA char *cp = str1;
    XDATA char *s1;
    char *s2;


    if (!*str2)
        return str1;

    while (*cp)
    {
        s1 = cp;
        s2 = str2;

        while (*s1 && *s2 && !(*s1-*s2))
            s1++, s2++;

        if (!*s2)
            return cp;

        ++cp;
    }

    return NULL;
}

long
strtol(char *nptr,char **endptr, int base)
__reentrant
{

    char *s = nptr;
    uint32 acc;
    int c;
    unsigned long cutoff;
    int neg = 0, any, cutlim;

    /*
     * Skip white space and pick up leading +/- sign if any.
     * If base is 0, allow 0x for hex and 0 for octal, else
     * assume decimal; if base is already 16, allow 0x.
     */
    do
    {
        c = *s++;
    }
    while (isspace(c));
    if (c == '-')
    {
        neg = 1;
        c = *s++;
    }
    else if (c == '+')
        c = *s++;
    if ((base == 0 || base == 16) &&
    c == '0' && (*s == 'x' || *s == 'X'))
    {
        c = s[1];
        s += 2;
        base = 16;
    }
    else if ((base == 0 || base == 2) &&
    c == '0' && (*s == 'b' || *s == 'B'))
    {
        c = s[1];
        s += 2;
        base = 2;
    }
    if (base == 0)
        base = c == '0' ? 8 : 10;

    cutoff = neg ? -(unsigned long)LONG_MIN : LONG_MAX;
    cutlim = cutoff % (unsigned long)base;
    cutoff /= (unsigned long)base;
    for (acc = 0, any = 0;; c = *s++)
    {
        if (isdigit(c))
            c -= '0';
        else if (isalpha(c))
            c -= isupper(c) ? 'A' - 10 : 'a' - 10;
        else
            break;
        if (c >= base)
            break;
        if (any < 0 || acc > cutoff || acc == cutoff && c > cutlim)
            //if (any < 0)
        {
            any = -1;
        }
        else
        {
            any = 1;
            acc *= base;
            acc += c;
        }
    }


    if (any < 0)
    {
        acc = neg ? LONG_MIN : LONG_MAX;
    }
    else if (neg)
        acc = -acc;
    if (endptr != 0)
        *endptr = (char *)(any ? s - 1 : nptr);
    return (acc);
}

char *scan_string (char *str, XDATA int base)
{
    char *str_ptr = (char*) str;

    switch (base)
    {
    case 10:
        while (!(isdigit(*str_ptr) || *str_ptr == '-' || *str_ptr == 0x0))
        {
            str_ptr++;
        }
        break;
    case 16:
        while (!(isxdigit(*str_ptr) || *str_ptr == 0x0))
        {
            str_ptr++;
        }
        break;
    }

    return str_ptr;
}

int sscanf(char *str, char *fmt, ...)
{
    va_list ap;
    char *format_ptr = (char*)fmt;
    char *str_ptr = (char*)str;

    int8 *p_byte;
    int *p_int;
    long *p_long;

    va_start (ap, fmt);

    while ((*format_ptr != 0x0) && (*str_ptr != 0x0))
    {
        if (*format_ptr == '%')
        {
            format_ptr++;

            if (*format_ptr != 0x0)
            {
                switch (*format_ptr)
                {
                case 'h':       // expect a byte
                    p_byte = va_arg( ap, uint8 *);
                    str_ptr=scan_string(str_ptr, 10);
                    if (*str_ptr==0x0) goto end_parse;
                    *p_byte = (uint8)strtol (str_ptr, &str_ptr, 10);
                    gret ++;
                    break;
                case 'd':       // expect an int
                case 'i':
                    p_int = va_arg( ap, int *);
                    str_ptr=scan_string(str_ptr, 10);
                    if (*str_ptr==0x0) goto end_parse;
                    *p_int = (int)strtol (str_ptr, &str_ptr, 10);
                    gret ++;
                    break;
                case 'D':
                case 'I':       // expect a long
                    p_long = va_arg( ap, long *);
                    str_ptr=scan_string(str_ptr, 10);
                    if (*str_ptr==0x0) goto end_parse;
                    *p_long = strtol (str_ptr, &str_ptr, 10);
                    gret ++;
                    break;
                case 'x':       // expect an int in hexadecimal format
                    p_int = va_arg( ap, int *);
                    str_ptr=scan_string(str_ptr, 16);
                    if (*str_ptr==0x0) goto end_parse;
                    *p_int = (int)strtol (str_ptr, &str_ptr, 16);
                    gret ++;
                    break;
                case 'X':  // expect a long in hexadecimal format
                    p_long = va_arg( ap, long *);
                    str_ptr=scan_string(str_ptr, 16);
                    if (*str_ptr==0x0) goto end_parse;
                    *p_long = strtol (str_ptr, &str_ptr, 16);
                    gret ++;
                    break;
                }
            }
        }

        format_ptr++;
    }

end_parse:
    va_end (ap);

    if (*str_ptr == 0x0) gret = myEOF;
    return gret;
}
void clearSettings()
{
    memset (&settings, 0, sizeof (settings));
    settings.dex_tx_id = asciiToDexcomSrc (transmitter_id);
    dex_tx_id = settings.dex_tx_id;
    sprintf(settings.http_url,my_webservice_url);
    sprintf(settings.gsm_apn,my_gprs_apn);
    sprintf(settings.udp_server,my_udp_server_host);
    sprintf(settings.udp_port,my_udp_server_port);
    sprintf(settings.gsm_lock,my_control_number);
}

void loadSettingsFromFlash()
{
    memcpy(&settings, (uint8 XDATA *)FLASH_SETTINGS, sizeof(settings));
#ifdef DEBUG
    usb_printf("AFTER Setting txid: %lu\r\n",settings.dex_tx_id);
#endif

    checksum();
#ifdef DEBUG
    usb_printf("Load Settings: tx: %lu / chk calcuated: %lu vs stored %lu\r\n",settings.dex_tx_id,chk,settings.checksum);
#endif
    nicedelayMs(500);
    if (chk!=settings.checksum)
    {
        clearSettings();

        // If custom transmitter_id has been compiled in then we don't wait for initial configuration
        if (dex_tx_id != 10858926)   // ABCDE
        {
            settings.deepsleep=1;
        }
    }
    else
    {
        dex_tx_id = settings.dex_tx_id;
    }
}

////////

void sleepInit (void)
{
    WORIRQ |= (1 << 4);
}

ISR (ST, 1)
{
    IRCON &= 0x7F;
    WORIRQ &= 0xFE;
    SLEEP &= 0xFC;
}

void switchToRCOSC (void)
{
    SLEEP &= ~0x04;
    while (!(SLEEP & 0x20));
    CLKCON = (CLKCON & ~0x07) | 0x40 | 0x01;
    while (!(CLKCON & 0x40));
    SLEEP |= 0x04;
}

void uartEnable ()
{
    U1UCR &= ~0x40;		//CTS/RTS Off // always off!
    U1CSR |= 0x40;		// Recevier enable
    delayMs (100);
}

void uartDisable ()
{
    delayMs (100);
    U1UCR &= ~0x40;		//CTS/RTS Off
    U1CSR &= ~0x40;		// Recevier disable
}

void blink_yellow_led ()
{    
        LED_YELLOW (((getMs () / 250) % 2));	//Blink quarter seconds    
}

void blink_red_led ()
{    
        LED_RED (((getMs () / 250) % 2));	//Blink 1/4 seconds    
}

int8 getPacketRSSI(Dexcom_packet * p)
{
    return (p->RSSI / 2) - 73;
}

uint8 getPacketPassedChecksum(Dexcom_packet * p)
{
    return ((p->LQI & 0x80) == 0x80) ? 1 : 0;
}

uint8 bit_reverse_byte (uint8 in)
{
    uint8 XDATA bRet = 0;
    if (in & 0x01)
        bRet |= 0x80;
    if (in & 0x02)
        bRet |= 0x40;
    if (in & 0x04)
        bRet |= 0x20;
    if (in & 0x08)
        bRet |= 0x10;
    if (in & 0x10)
        bRet |= 0x08;
    if (in & 0x20)
        bRet |= 0x04;
    if (in & 0x40)
        bRet |= 0x02;
    if (in & 0x80)
        bRet |= 0x01;
    return bRet;
}

uint8 min8 (uint8 a, uint8 b)
{
    if (a < b)
        return a;
    return b;
}

void bit_reverse_bytes(uint8 * buf, uint8 nLen)
{
    uint8 DATA i = 0;
    for (; i < nLen; i++)
    {
        buf[i] = bit_reverse_byte (buf[i]);
    }
}

uint32 dex_num_decoder(uint16 usShortFloat)
{
    uint16 DATA usReversed = usShortFloat;
    uint8 DATA usExponent = 0;
    uint32 DATA usMantissa = 0;
    bit_reverse_bytes ((uint8 *) & usReversed, 2);
    usExponent = ((usReversed & 0xE000) >> 13);
    usMantissa = (usReversed & 0x1FFF);
    return usMantissa << usExponent;
}

char XDATA SrcNameTable[32] = { '0', '1', '2', '3', '4', '5', '6', '7',
                                '8', '9', 'A', 'B', 'C', 'D', 'E', 'F',
                                'G', 'H', 'J', 'K', 'L', 'M', 'N', 'P',
                                'Q', 'R', 'S', 'T', 'U', 'W', 'X', 'Y'
                              };

void doServices ()
{
    if (usb_connected)
    {
        usbComService ();
        boardService ();
    }
}

void initUart1 ()
{
    uart1Init ();
    uart1SetBaudRate (9600);
}

uint32 asciiToDexcomSrc (char addr[6])
{
    uint32 XDATA src = 0;
    src |= (getSrcValue (addr[0]) << 20);
    src |= (getSrcValue (addr[1]) << 15);
    src |= (getSrcValue (addr[2]) << 10);
    src |= (getSrcValue (addr[3]) << 5);
    src |= getSrcValue (addr[4]);
    return src;
}

uint32 getSrcValue (char srcVal)
{
    uint8 i = 0;
    for (i = 0; i < 32; i++)
    {
        if (SrcNameTable[i] == srcVal)
            break;
    }
    return i & 0xFF;
}

void usb_printf (const char *format, ...)
{
    va_list argx;
    if (usb_connected)
    {
        va_start (argx, format);
        vprintf (format, argx);
    }
}

void print_packet(Dexcom_packet * pPkt)
{
    lasttime = getMs ();
    lastraw = dex_num_decoder (pPkt->raw);
    lastfiltered = dex_num_decoder (pPkt->filtered) * 2;

    uartEnable();

    if ((allow_alternate_usb_protocol == 0) || !usbPowerPresent ())
    {

        // Classic 3 field protocol for serial/bluetooth only
            printf ("%lu %hhu %d", dex_num_decoder (pPkt->raw), pPkt->battery,
                    adcConvertToMillivolts (adcRead (0)));
    }
    else
    {
        // Protocol suitable for dexterity android application or python script when running in USB mode
        usb_printf ("%lu %lu %lu %hhu %d %hhu %d \r\n", pPkt->src_addr,
                    dex_num_decoder (pPkt->raw),
                    dex_num_decoder (pPkt->filtered) * 2, pPkt->battery,
                    getPacketRSSI (pPkt), pPkt->txId,
                    adcConvertToMillivolts (adcRead (0)));
    }
    uartDisable();
}

/////////////////////////////
void makeAllOutputsLow ()
__reentrant
{
    int i;
    for (i = 0; i < 16; i++)
    {
        if (i != 13)
            setDigitalOutput (i, LOW);	// skip P1_3 for dtr line
    }    
        P0INP = 0;  //set pull resistors on pins 0_0 - 0_5 to low    
}

void reset_offsets ()
{
    int i;
    for (i = 0; i < 4; i++)
    {
        fOffset[i] = defaultfOffset[i];
    }
}

void killWithWatchdog ()
{
    WDCTL = (WDCTL & ~0x03) | 0x00;
    WDCTL = (WDCTL & ~0x04) | 0x08;
}

void goToSleep (int32 seconds)
__reentrant
{
    if (seconds < 1)
        return;

    adcSetMillivoltCalibration (adcReadVddMillivolts ());
    makeAllOutputsLow ();

    if (!needsTimingCalibration)
    {
        if (!usbPowerPresent ())
        {
            unsigned char temp;
            unsigned char storedDescHigh, storedDescLow;
            BIT storedDma0Armed;
            unsigned char storedIEN0, storedIEN1, storedIEN2;

            uint8 savedPICTL = PICTL;
            BIT savedP0IE = P0IE;
            uint8 savedP0SEL = P0SEL;
            uint8 savedP0DIR = P0DIR;
            uint8 savedP1SEL = P1SEL;
            uint8 savedP1DIR = P1DIR;

            sleepInit ();

            disableUsbPullup ();
            usbDeviceState = USB_STATE_DETACHED;
            usbEnabled = 0;
            SLEEP &= ~(1 << 7);

            WORCTRL |= 0x03;	// 2^5 periods
            switchToRCOSC ();

            storedDescHigh = DMA0CFGH;
            storedDescLow = DMA0CFGL;
            storedDma0Armed = DMAARM & 0x01;
            DMAARM |= 0x81;
            dmaDesc[0] = ((unsigned int) &PM2_BUF) >> 8;
            dmaDesc[1] = (unsigned int) &PM2_BUF;

            DMA0CFGH = ((unsigned int) &dmaDesc) >> 8;
            DMA0CFGL = (unsigned int) &dmaDesc;
            DMAARM = 0x01;

            // save enabled interrupts
            storedIEN0 = IEN0;
            storedIEN1 = IEN1;
            storedIEN2 = IEN2;

            //enable sleep timer interrupt
            IEN0 |= 0xA0;

            //disable all interrupts except the sleep timer
            IEN0 &= 0xA0;
            IEN1 &= ~0x3F;
            IEN2 &= ~0x3F;

            WORCTRL |= 0x04;	// Reset
            temp = WORTIME0;
            while (temp == WORTIME0)
            {
            };
            WOREVT1 = seconds >> 8;
            WOREVT0 = seconds;

            temp = WORTIME0;
            while (temp == WORTIME0)
            {
            };

            MEMCTR |= 0x02;
            SLEEP = 0x06;
            __asm nop __endasm;
            __asm nop __endasm;
            __asm nop __endasm;
            if (SLEEP & 0x03)
            {
                __asm mov 0xD7, #0x01 __endasm;
                __asm nop __endasm;
                __asm orl 0x87, #0x01 __endasm;
                __asm nop __endasm;
            }
            IEN0 = storedIEN0;
            IEN1 = storedIEN1;
            IEN2 = storedIEN2;
            DMA0CFGH = storedDescHigh;
            DMA0CFGL = storedDescLow;
            if (storedDma0Armed)
            {
                DMAARM |= 0x01;
            }
            // Switch back to high speed
            boardClockInit ();

            PICTL = savedPICTL;
            P0IE = savedP0IE;
            P0SEL = savedP0SEL;
            P0DIR = savedP0DIR;
            P1SEL = savedP1SEL;
            P1DIR = savedP1DIR;
            USBPOW = 1;
            USBCIE = 0b0111;
        }
        else
        {
            uint32 start_waiting = getMs ();
            if (!usbEnabled)
            {
                usbDeviceState = USB_STATE_POWERED;
                enableUsbPullup ();
                usbEnabled = 1;
            }
            delayMs (100);
            while ((getMs () - start_waiting) < (seconds * 1000))
            {
                doServices ();
            }
        }
    }
    makeAllOutputsLow ();
}

void putchar (char c)
{
    uart1TxSendByte (c);
    if (usbPowerPresent ())
    {
        while (usbComTxAvailable()<100)
        {
            usbComService();
        }
        usbComTxSendByte (c);
    }
}

void swap_channel(uint8 channel, uint8 newFSCTRL0)
{
    do
    {
        RFST = 4;			//SIDLE
    }
    while (MARCSTATE != 0x01);

    FSCTRL0 = newFSCTRL0;
    CHANNR = channel;
    RFST = 2;			//RX
}

void strobe_radio(int radio_chan)
{
    radioMacInit ();
    MCSM1 = 0;
    radioMacStrobe ();
    swap_channel (nChannels[radio_chan], fOffset[radio_chan]);
}

int WaitForPacket(uint16 milliseconds, Dexcom_packet * pkt, uint8 channel)
__reentrant
{
    uint32 start = getMs ();
    uint8 *packet = 0;
    uint32 i = 0;
#define six_minutes 360000
    int nRet = 0;
    swap_channel (nChannels[channel], fOffset[channel]);

    while (!milliseconds || (getMs () - start) < milliseconds)
    {
        doServices ();
        blink_yellow_led ();
        i++;
        if (!(i % 40000))
        {
            strobe_radio (channel);
        }
        if ((getMs () - start) > six_minutes)
        {
            killWithWatchdog ();
            delayMs (2000);
        }
        if (packet = radioQueueRxCurrentPacket ())
        {
            uint8 len = packet[0];
            fOffset[channel] += FREQEST;
            memcpy (pkt, packet, min8 (len + 2, sizeof (Dexcom_packet)));
            if (radioCrcPassed ())
            {
                if (pkt->src_addr == dex_tx_id || dex_tx_id == 0
                        || only_listen_for_my_transmitter == 0)
                {
                    pkt->txId -= channel;
                    radioQueueRxDoneWithPacket ();
                    LED_YELLOW (0);
                    last_catch_channel = channel;
                    return 1;
                }
                else
                {
                    radioQueueRxDoneWithPacket ();
                }
            }
            else
            {
                radioQueueRxDoneWithPacket ();
                LED_YELLOW (0);
                return 0;
            }
        }
    }
    LED_YELLOW (0);
    return nRet;
}

uint32 delayFor(int wait_chan)
{
    if (needsTimingCalibration)
    {
        return delayedWaitTimes[wait_chan];
    }
    if (!wait_chan && sequential_missed_packets)
    {
        return waitTimes[wait_chan] +
               (sequential_missed_packets * wake_earlier_for_next_miss * 2 * 1000);
    }
    else
    {
        return waitTimes[wait_chan];
    }
}

BIT get_packet(Dexcom_packet * pPkt)
{
    int nChannel = 0;
    for (nChannel = start_channel; nChannel < NUM_CHANNELS; nChannel++)
    {
        switch (WaitForPacket(delayFor(nChannel), pPkt, nChannel))
        {
			case 1:
				needsTimingCalibration = 0;
				sequential_missed_packets = 0;
				return 1;
			case 0:
				continue;
        }
    }
    needsTimingCalibration = 1;
    sequential_missed_packets++;
    if (sequential_missed_packets > misses_until_failure)
    {
        sequential_missed_packets = 0;
        needsTimingCalibration = 1;
    }
    reset_offsets ();
    last_catch_channel = 0;
    return 0;
}

void nicedelayMs (int ms)
__reentrant
{
    uint32 timeend = getMs() + ms;
    while (getMs() < timeend)
    {
        doServices ();
    }
}

void main()
{
    systemInit();
    LED_YELLOW(1);		// YELLOW LED STARTS DURING POWER ON
    initUart1 ();
    P1DIR |= 0x08;		// RTS
    sleepInit();
    nicedelayMs (3000);		// extra sleep for grumpy sim800 units
    LED_GREEN (1);		// Green shows when connected usb

    usb_connected = usbPowerPresent();

    makeAllOutputsLow ();
    nicedelayMs (1000);
    loadSettingsFromFlash();
    nicedelayMs(1000);
    radioQueueInit();
    radioQueueAllowCrcErrors = 1;

    MCSM1 = 0;
    MCSM0 &= 0x34;			// calibrate every fourth transition to and from IDLE.
    MCSM1 = 0x00;			// after RX go to idle, we don't transmit
    MCSM2 = 0x17;			// terminate receiving on drop of carrier, but keep it up when packet quality is good.

    while (1)
    {
        Dexcom_packet Pkt;
        memset (&Pkt, 0, sizeof (Dexcom_packet));
        if (get_packet(&Pkt))
        {
            print_packet(&Pkt);
        }

        RFST = 4;			// SIDLE = 4
        delayMs (100);

        radioMacSleep ();

        if (usbPowerPresent ())
        {
            sequential_missed_packets++;
        }
        if (sequential_missed_packets > 0)
        {
            int first_square =
                sequential_missed_packets * sequential_missed_packets *
                wake_earlier_for_next_miss;
            int second_square =
                (sequential_missed_packets - 1) * (sequential_missed_packets -
                                                   1) *
                wake_earlier_for_next_miss;
            int sleep_time = (268 - first_square + second_square);
            goToSleep (sleep_time);
        }
        else
        {
            goToSleep (283);
        }
        radioMacResume ();

        MCSM1 = 0;
        radioMacStrobe ();
    }
}