/* Recording bb_epaper backend for the host equivalence harness.
 *
 * Every primitive the library calls lands here and is written to g_rec as one
 * event line. Nothing reaches hardware. BUSY always reads idle for the active
 * chip family (UC81xx idles high, SSD16xx low), so every wait returns on its
 * first poll and the stream shows exactly where the waits are.
 *
 * Event lines:
 *   C <hex> <cs>     command byte (DC low); cs = 1, 2, 3 (both) or 0 (none)
 *   D <hex...> <cs>  data bytes (DC high)
 *   I <n> <cs>       n frame bytes (written while g_in_image is set)
 *   R <0|1>          RST pin level
 *   S <cs> <0|1>     manual chip-select level (cs = 1 or 2)
 *   W <ms>           delay
 *   B                BUSY pin read
 */
#include <stdio.h>
#include <string.h>

#define pgm_read_byte(a)  (*(const uint8_t *)(a))
#define pgm_read_word(a)  (*(const uint16_t *)(a))
#define pgm_read_dword(a) (*(const uint32_t *)(a))
#define memcpy_P memcpy

#define REC_PIN_DC   1
#define REC_PIN_RST  2
#define REC_PIN_BUSY 3
#define REC_PIN_CS   4
#define REC_PIN_CS2  5

static FILE *g_rec;
static int g_in_image;
static int g_busy_idle_high;

void bbepWakeUp(BBEPDISP *pBBEP);

static int rec_cs(BBEPDISP *p)
{
    switch (p->cs_mode) {
        case 0:
        case CMD_CS1:     return 1;
        case CMD_CS2:     return 2;
        case CMD_CS1_CS2: return 3;
        default:          return 0;
    }
}

void digitalWrite(int pin, int v)
{
    if (pin == REC_PIN_RST) {
        fprintf(g_rec, "R %d\n", v ? 1 : 0);
    } else if (pin == REC_PIN_CS || pin == REC_PIN_CS2) {
        fprintf(g_rec, "S %d %d\n", pin == REC_PIN_CS ? 1 : 2, v ? 1 : 0);
    }
}

void pinMode(int pin, int mode) { (void)pin; (void)mode; }

int digitalRead(int pin)
{
    if (pin == REC_PIN_BUSY) {
        fprintf(g_rec, "B\n");
        return g_busy_idle_high;
    }
    return 0;
}

void delay(REC_DELAY_T ms) { fprintf(g_rec, "W %ld\n", (long)ms); }
void delayMicroseconds(long us) { (void)us; }
long millis(void) { return 0; }

void bbepSetCS2(BBEPDISP *pBBEP, uint8_t cs) { pBBEP->iCS2Pin = cs; }

void bbepInitIO(BBEPDISP *pBBEP, uint8_t u8DC, uint8_t u8RST, uint8_t u8BUSY, uint8_t u8CS,
                uint8_t u8MOSI, uint8_t u8SCK, uint32_t u32Speed)
{
    pBBEP->iDCPin = u8DC;
    pBBEP->iCSPin = u8CS;
    pBBEP->iMOSIPin = u8MOSI;
    pBBEP->iCLKPin = u8SCK;
    pBBEP->iRSTPin = u8RST;
    pBBEP->iBUSYPin = u8BUSY;
    pBBEP->iSpeed = (int)u32Speed;
}

static void rec_bytes(char tag, const uint8_t *d, int n, int cs)
{
    if (g_in_image && tag == 'D') {
        fprintf(g_rec, "I %d %d\n", n, cs);
        return;
    }
    fputc(tag, g_rec);
    fputc(' ', g_rec);
    for (int i = 0; i < n; i++) {
        fprintf(g_rec, "%02x", d[i]);
    }
    fprintf(g_rec, " %d\n", cs);
}

void bbepWriteIT8951Cmd(BBEPDISP *pBBEP, uint16_t cmd) { (void)pBBEP; fprintf(g_rec, "X it8951 %04x\n", cmd); }
void bbepWriteIT8951Data(BBEPDISP *pBBEP, uint8_t *pData, int iLen) { (void)pBBEP; (void)pData; fprintf(g_rec, "X it8951data %d\n", iLen); }
void bbepWriteIT8951CmdArgs(BBEPDISP *pBBEP, uint16_t cmd, uint16_t *pArgs, int iCount)
{
    (void)pArgs; (void)iCount;
    bbepWriteIT8951Cmd(pBBEP, cmd);
}

void bbepWriteCmd(BBEPDISP *pBBEP, uint8_t cmd)
{
    if (!pBBEP->is_awake) {
        bbepWakeUp(pBBEP);
        pBBEP->is_awake = 1;
    }
    rec_bytes('C', &cmd, 1, rec_cs(pBBEP));
}

void bbepWriteData(BBEPDISP *pBBEP, uint8_t *pData, int iLen)
{
    if (iLen > 0) {
        rec_bytes('D', pData, iLen, rec_cs(pBBEP));
    }
}

void bbepWriteCmdData(BBEPDISP *pBBEP, uint8_t cmd, uint8_t *pData, int iLen)
{
    if (!pBBEP->is_awake) {
        bbepWakeUp(pBBEP);
        pBBEP->is_awake = 1;
    }
    rec_bytes('C', &cmd, 1, rec_cs(pBBEP));
    if (pData != NULL && iLen > 0) {
        rec_bytes('D', pData, iLen, rec_cs(pBBEP));
    }
}

void bbepCMD2(BBEPDISP *pBBEP, uint8_t cmd1, uint8_t cmd2)
{
    bbepWriteCmd(pBBEP, cmd1);
    bbepWriteData(pBBEP, &cmd2, 1);
}
