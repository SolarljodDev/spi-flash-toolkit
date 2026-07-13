#include <stdint.h>
#include "stm32f1xx.h"

/* ============================================================================
 * SPI EEPROM/flash dumper/programmer (GD32F103, 24 MHz HSE -> 108 MHz SYSCLK)
 *
 * Wiring (SPI2 default pins) -- standard 25-series SOIC8 pinout, shared by the
 * M95128 EEPROM, the SST25VF032B NOR flash, and most similar parts:
 *   PB12 -> pin1 /CS (CE#)   (manual GPIO chip-select)
 *   PB13 -> pin6 SCK
 *   PB14 <- pin2 SO (MISO)
 *   PB15 -> pin5 SI (MOSI)
 *   PB6  -> CH340 RXD (USART1 TX, remapped)
 *   PB7  <- CH340 TXD (USART1 RX, remapped)
 *   pin3 /WP  -> VCC
 *   pin7 /HOLD -> VCC
 *   pin4 GND, pin8 VCC (3.3V from the STM32 board: M95128 works down to 2.5V,
 *   SST25VF032B needs 2.7-3.6V -- 3.3V is fine for both)
 *
 * Host protocol (USART1) -- all multi-byte integers are little-endian:
 *   'R' addr_bytes(1) start_addr(4) size(4)
 *       -> board streams `size` bytes back, starting at `start_addr`
 *          (READ 0x03 + addr_bytes address bytes). The host uses this to read
 *          the chip in smaller segments with brief pauses between them rather
 *          than one uninterrupted multi-second burst -- some USB-serial
 *          bridges (e.g. CP2104) have been observed to degrade/drop bytes
 *          partway through a long continuous transfer, especially on a
 *          second one back-to-back; breaking it up gives the link recovery
 *          gaps throughout instead of just before a single retry.
 *   'W' addr_bytes(1) page_size(1) batch_size(2) size(4)
 *       -> host then sends exactly `size` bytes, written page_size bytes at a
 *          time (WREN + WRITE 0x02 + address + page_size data bytes, poll
 *          WIP) -- this is the generic 25-series EEPROM write scheme; for
 *          byte-addressable NOR flash (e.g. SST25VF032B) set page_size=1,
 *          which degrades it to plain Byte-Program (target address must
 *          already be erased (0xFF) first, see 'E'). batch_size (a multiple
 *          of page_size) is unrelated to the chip: it's how many bytes the
 *          host sends before waiting for one 0x06 ACK. A big batch_size cuts
 *          the number of round trips -- with page_size=1 (NOR) each physical
 *          write only blocks ~10us, so hundreds/thousands of them can be
 *          done per ACK with no overrun risk, instead of one ACK per byte.
 *          Once done, immediately streams the full contents back (like 'R')
 *          so the host can verify.
 *          Each batch's data bytes are immediately followed by 1 checksum
 *          byte (8-bit wrapping sum of just that batch's data) -- added
 *          after a real multi-minute write over a USB-serial bridge silently
 *          lost a byte partway through, with zero protocol-level symptom (no
 *          NAK, no timeout): everything downstream just permanently shifted
 *          by one position, which only showed up as scattered mismatches in
 *          the post-write full-chip verify. On a checksum mismatch the
 *          firmware replies 0x43 ('C') instead of 0x06/0x15 and expects the
 *          SAME batch (data + checksum) resent, up to WRITE_BATCH_RETRY_MAX
 *          times before giving up with a NAK.
 *   'E' addr_bytes(1)
 *       -> WREN + Chip-Erase (0x60) + poll WIP, then replies 0x06 (ACK) on
 *          success or 0x15 (NAK) if WIP never cleared within WRITE_TIMEOUT_MS
 *          (chip became unresponsive, or is genuinely far slower than spec --
 *          see spi2_wait_write_done). Only meaningful for NOR flash -- EEPROMs
 *          like the M95128 don't need or support this and may treat 0x60 as a
 *          no-op/undefined.
 *   'I' (no params)
 *       -> JEDEC Read-ID (0x9F): sends back 3 bytes (Manufacturer ID, Memory
 *          Type, Capacity) with no address phase. Nearly universal across
 *          SPI NOR flash from every vendor -- use it to positively identify
 *          an unknown chip instead of guessing size from a package marking.
 *          Plain EEPROMs (M95128 and similar) generally don't implement this
 *          and will just answer 0xFF 0xFF 0xFF (or similar noise).
 *   'S' (no params)
 *       -> RDSR (0x05): sends back the raw 1-byte status register. Check this
 *          before a write if it doesn't seem to take effect -- many SPI
 *          EEPROMs/flash power up with the BP0-BP3 block-protection bits set
 *          (whole array write-protected) regardless of the WP# pin, which
 *          only gates whether those bits *can be changed*, not whether
 *          existing protection is enforced.
 *   'U' (no params)
 *       -> WREN + WRSR (0x01) + a single 0x00 data byte, clearing BP0-BP3 and
 *          BPL so the whole array becomes writable, then polls WIP and replies
 *          0x06/0x15 same as 'E'. ("Unprotect".)
 *   'P' (no params)
 *       -> WREN + WRSR (0x01) + a single 0x0C data byte (BP1=1, BP0=1),
 *          protecting the whole array against WRITE, then polls WIP and
 *          replies 0x06/0x15 same as 'E'. ("Protect All" -- mirror of 'U'.)
 *          Leaves SRWD at 0, so 'U' can always undo this later from this
 *          reader regardless of the WP# pin.
 *
 * Note on 0x06/0x15: every operation that polls WIP (page writes within 'W',
 * 'E', 'U') can fail that poll if the chip stops responding mid-operation --
 * without a bound on that loop, the firmware itself would hang forever with
 * no way to ever report back. spi2_wait_write_done() times out after
 * WRITE_TIMEOUT_MS (real elapsed time via SysTick, not a guessed loop count)
 * and the caller sends 0x15 (NAK) instead of 0x06 (ACK) so the host can tell
 * "still working" apart from "genuinely stuck" instead of just seeing a raw
 * link timeout with no information either way.
 *
 * DMA design note: DMA1 channel 4 is shared between SPI2_RX and USART1_TX,
 * and channel 5 between SPI2_TX and USART1_RX (fixed silicon mapping,
 * identical to STM32F103) -- so SPI2 and USART1 cannot both run on DMA at
 * the same time, they would fight over the same channels. USART1 is the
 * actual bottleneck (115200 baud vs. ~1.7 MHz SPI), so only the USART1 TX
 * path (the read-back / dump direction) is put on DMA (channel 4). SPI2
 * reads/writes stay polled, and so does USART1 RX (page writes are paced by
 * the ~5ms EEPROM write cycle anyway, far slower than the UART can deliver).
 * ==========================================================================*/

#define HSE_HZ        24000000UL
#define SYSCLK_HZ     108000000UL
#define UART_BAUD     2000000UL /* CH340/CP2104's practical ceiling; 108MHz/16/baud divides exactly, zero BRR quantization error.
                                    (Earlier timeouts at this speed traced to the web page's progress bar hammering the DOM
                                    on every chunk, not the link itself -- see web/index.html's setProgress throttling.) */
#define CHUNK_SIZE    1024U   /* size requested by the host must be a multiple of this */
#define MAX_BATCH     4096U   /* upper bound on batch_size (see 'W' in the protocol notes above) */

#define CS_HIGH()   (GPIOB->BSRR = (1UL << 12))
#define CS_LOW()    (GPIOB->BSRR = (1UL << (12 + 16)))

static void system_clock_config(void);
static void gpio_config(void);
static void spi2_config(void);
static void usart1_config(uint32_t baud);
static void dma1_ch4_config(void);
static uint8_t spi2_transfer(uint8_t data);
static void dma_tx_start(const uint8_t *buf, uint16_t len);
static void dma_tx_wait_done(void);
static void eeprom_dump(uint8_t addr_bytes, uint32_t start_addr, uint32_t size);
static void eeprom_program(uint8_t addr_bytes, uint8_t page_size, uint16_t batch_size, uint32_t size);
static void eeprom_erase(uint8_t addr_bytes);
static void eeprom_identify(void);
static uint8_t spi2_read_status(void);
static void eeprom_clear_protection(void);
static void eeprom_protect_all(void);
static uint8_t usart1_recv_byte(void);
static void usart1_send_byte(uint8_t b);
static uint16_t usart1_recv_u16le(void);
static uint32_t usart1_recv_u32le(void);
static uint32_t millis(void);

static uint8_t dma_buf[2][CHUNK_SIZE];
static uint8_t write_batch[MAX_BATCH];
static volatile uint32_t g_ms_ticks = 0;

/* Debug aid: last RDSR value spi2_wait_write_done() actually read from the
 * chip, surfaced to the host after a NAK (see send_ack_or_nak()) so a
 * genuinely-stuck-BUSY chip can be told apart from a corrupted/dead SPI read
 * without a second UART. Three sentinels mean the NAK happened before ever
 * touching the chip: 0xEE = the write header itself was rejected
 * (page_size/batch_size mismatch), 0xDD = the host stopped sending batch data
 * for 5s (USB-serial link hiccup, not the chip), 0xCC = a batch's checksum
 * kept failing even after WRITE_BATCH_RETRY_MAX resends (see checksum8()) --
 * the link is corrupting data faster than the retry budget can absorb.
 * Temporary -- remove alongside the diag byte in web/index.html's
 * readAckOrThrow() once the root cause is found. */
static uint8_t g_last_status_reg = 0xFF;

void SysTick_Handler(void)
{
    g_ms_ticks++;
}

static uint32_t millis(void)
{
    return g_ms_ticks;
}

int main(void)
{
    system_clock_config();
    SysTick_Config(SystemCoreClock / 1000U); /* 1ms tick, used only to bound spi2_wait_write_done() */
    gpio_config();
    spi2_config();
    usart1_config(UART_BAUD);
    dma1_ch4_config();

    CS_HIGH();

    while (1) {
        if (USART1->SR & USART_SR_RXNE) {
            uint8_t cmd = (uint8_t)USART1->DR;
            if (cmd == 'R') {
                usart1_send_byte(0x41); /* link ACK: command byte was received -- lets the host tell a dead UART link apart from a downstream SPI/wiring problem */
                uint8_t addr_bytes = usart1_recv_byte();
                uint32_t start_addr = usart1_recv_u32le();
                uint32_t size = usart1_recv_u32le();
                eeprom_dump(addr_bytes, start_addr, size);
            } else if (cmd == 'W') {
                usart1_send_byte(0x41); /* link ACK */
                uint8_t addr_bytes = usart1_recv_byte();
                uint8_t page_size = usart1_recv_byte();
                uint16_t batch_size = usart1_recv_u16le();
                uint32_t size = usart1_recv_u32le();
                eeprom_program(addr_bytes, page_size, batch_size, size);
            } else if (cmd == 'E') {
                usart1_send_byte(0x41); /* link ACK */
                uint8_t addr_bytes = usart1_recv_byte();
                eeprom_erase(addr_bytes);
            } else if (cmd == 'I') {
                usart1_send_byte(0x41); /* link ACK */
                eeprom_identify();
            } else if (cmd == 'S') {
                usart1_send_byte(0x41); /* link ACK */
                usart1_send_byte(spi2_read_status());
            } else if (cmd == 'U') {
                usart1_send_byte(0x41); /* link ACK */
                eeprom_clear_protection();
            } else if (cmd == 'P') {
                usart1_send_byte(0x41); /* link ACK */
                eeprom_protect_all();
            }
        }
    }
}

/* (24 MHz / 2) * 9 = 108 MHz. AHB = 108 MHz, APB1 = 54 MHz, APB2 = 108 MHz. */
static void system_clock_config(void)
{
    RCC->CR |= RCC_CR_HSEON;
    while (!(RCC->CR & RCC_CR_HSERDY)) { }

    /* 3 wait states required above ~90 MHz on GD32F103 */
    FLASH->ACR = (FLASH->ACR & ~FLASH_ACR_LATENCY) | FLASH_ACR_LATENCY_0 | FLASH_ACR_LATENCY_1;
    FLASH->ACR |= FLASH_ACR_PRFTBE;

    RCC->CFGR &= ~(RCC_CFGR_PLLSRC | RCC_CFGR_PLLXTPRE | RCC_CFGR_PLLMULL |
                   RCC_CFGR_HPRE | RCC_CFGR_PPRE1 | RCC_CFGR_PPRE2);
    RCC->CFGR |= RCC_CFGR_PLLSRC | RCC_CFGR_PLLXTPRE_HSE_DIV2 | RCC_CFGR_PLLMULL9;
    RCC->CFGR |= RCC_CFGR_PPRE1_DIV2; /* APB1 max is 54 MHz, must be divided */
    /* HPRE and PPRE2 stay at /1 -> AHB = APB2 = 108 MHz */

    RCC->CR |= RCC_CR_PLLON;
    while (!(RCC->CR & RCC_CR_PLLRDY)) { }

    RCC->CFGR = (RCC->CFGR & ~RCC_CFGR_SW) | RCC_CFGR_SW_PLL;
    while ((RCC->CFGR & RCC_CFGR_SWS) != RCC_CFGR_SWS_PLL) { }

    SystemCoreClock = SYSCLK_HZ;
}

static void gpio_config(void)
{
    RCC->APB2ENR |= RCC_APB2ENR_IOPBEN | RCC_APB2ENR_AFIOEN;
    AFIO->MAPR |= AFIO_MAPR_USART1_REMAP; /* USART1 TX/RX -> PB6/PB7 */

    /* CRL pin6/7: PB6 USART1_TX = AF PP 50MHz (0xB), PB7 USART1_RX = input floating (0x4, reset default). */
    GPIOB->CRL = (GPIOB->CRL & 0x00FFFFFFUL) | 0x4B000000UL;

    /* CRH pin12..15: PB12 CS = out PP 50MHz (0x3), PB13 SCK = AF PP 50MHz (0xB),
       PB14 MISO = input pull-up (0x8), PB15 MOSI = AF PP 50MHz (0xB). Pins 8-11 untouched. */
    GPIOB->CRH = (GPIOB->CRH & 0x0000FFFFUL) | 0xB8B30000UL;
    GPIOB->ODR |= (1UL << 14); /* PB14: ODR=1 selects pull-up (vs pull-down) */
}

static void spi2_config(void)
{
    RCC->APB1ENR |= RCC_APB1ENR_SPI2EN;

    /* Master, software NSS, mode 0 (CPOL=0/CPHA=0), MSB first, 8-bit.
       SPI2 is on APB1 (54 MHz here). BR = /16 -> 54MHz/16 = 3.375 MHz
       (still well under M95128's 5MHz max, with ~35% margin for imperfect
       clip/breadboard wiring; needed to keep up with the 2Mbaud UART -- the
       DMA pipeline only hides SPI behind UART as long as SPI stays faster.
       Go to /32 or /64 if reads become unreliable on long/clip wiring). */
    SPI2->CR1 = SPI_CR1_MSTR | SPI_CR1_SSM | SPI_CR1_SSI | SPI_CR1_BR_1 | SPI_CR1_BR_0;
    SPI2->CR1 |= SPI_CR1_SPE;
}

static void usart1_config(uint32_t baud)
{
    RCC->APB2ENR |= RCC_APB2ENR_USART1EN;

    /* USART1 is on APB2 (108 MHz here). BRR = mantissa:fraction, fraction is 4 bits. */
    uint32_t div16 = (uint32_t)(((uint64_t)SYSCLK_HZ * 1000ULL) / (16ULL * baud));
    uint32_t mantissa = div16 / 1000;
    uint32_t fraction = ((div16 % 1000) * 16 + 500) / 1000;
    if (fraction > 15) { mantissa++; fraction = 0; }
    USART1->BRR = (mantissa << 4) | fraction;

    USART1->CR1 = USART_CR1_UE | USART_CR1_TE | USART_CR1_RE;
}

/* DMA1 channel 4 = USART1_TX, memory -> peripheral, 8-bit, one-shot (not circular). */
static void dma1_ch4_config(void)
{
    RCC->AHBENR |= RCC_AHBENR_DMA1EN;

    DMA1_Channel4->CPAR = (uint32_t)&USART1->DR;
    DMA1_Channel4->CCR = DMA_CCR_DIR | DMA_CCR_MINC; /* PSIZE/MSIZE = 8-bit, PINC=0, CIRC=0 */

    USART1->CR3 |= USART_CR3_DMAT;
}

static void dma_tx_start(const uint8_t *buf, uint16_t len)
{
    DMA1_Channel4->CCR &= ~DMA_CCR_EN;
    DMA1->IFCR = DMA_IFCR_CTCIF4;
    DMA1_Channel4->CMAR = (uint32_t)buf;
    DMA1_Channel4->CNDTR = len;
    DMA1_Channel4->CCR |= DMA_CCR_EN;
}

static void dma_tx_wait_done(void)
{
    while (!(DMA1->ISR & DMA_ISR_TCIF4)) { }
    DMA1->IFCR = DMA_IFCR_CTCIF4;
    DMA1_Channel4->CCR &= ~DMA_CCR_EN;
}

static uint8_t spi2_transfer(uint8_t data)
{
    while (!(SPI2->SR & SPI_SR_TXE)) { }
    SPI2->DR = data;
    while (!(SPI2->SR & SPI_SR_RXNE)) { }
    return (uint8_t)SPI2->DR;
}

static void spi2_fill(uint8_t *dst, uint16_t len)
{
    for (uint16_t i = 0; i < len; i++) {
        dst[i] = spi2_transfer(0xFF); /* clock out dummy byte while clocking in data */
    }
}

/* Sends `addr_bytes` bytes of `addr`, MSB first (e.g. 2 bytes for M95128, 3 for SST25VF032B). */
static void spi2_send_addr(uint8_t addr_bytes, uint32_t addr)
{
    for (int8_t i = (int8_t)addr_bytes - 1; i >= 0; i--) {
        spi2_transfer((uint8_t)(addr >> (8 * i)));
    }
}

static uint8_t usart1_recv_byte(void)
{
    while (!(USART1->SR & USART_SR_RXNE)) { }
    return (uint8_t)USART1->DR;
}

/* Same as usart1_recv_byte() but bounded -- confirmed via live SWD debug
 * (PC parked at the RXNE spin loop below) that a host which stops sending
 * mid-batch (a size/batch_size mismatch, a dropped USB connection, or two
 * commands racing on the same link because the web UI didn't disable other
 * buttons during an in-flight operation) hangs the MCU here forever, with
 * no way to recover short of a physical power-cycle. Used for the one loop
 * that waits on a genuinely large, host-paced amount of data (a write
 * batch, up to MAX_BATCH bytes) -- the header fields elsewhere are a
 * handful of bytes each, immediately following the link ACK the host is
 * already blocking on, so they're far lower risk and left as-is. */
#define USART_RECV_TIMEOUT_MS 5000UL

static int usart1_recv_byte_timeout(uint8_t *out)
{
    uint32_t start = millis();
    while (!(USART1->SR & USART_SR_RXNE)) {
        if ((uint32_t)(millis() - start) >= USART_RECV_TIMEOUT_MS) return 0;
    }
    *out = (uint8_t)USART1->DR;
    return 1;
}

static void usart1_send_byte(uint8_t b)
{
    while (!(USART1->SR & USART_SR_TXE)) { }
    USART1->DR = b;
}

static uint16_t usart1_recv_u16le(void)
{
    uint16_t v = 0;
    for (uint8_t i = 0; i < 2; i++) {
        v |= (uint16_t)usart1_recv_byte() << (8 * i);
    }
    return v;
}

static uint32_t usart1_recv_u32le(void)
{
    uint32_t v = 0;
    for (uint8_t i = 0; i < 4; i++) {
        v |= (uint32_t)usart1_recv_byte() << (8 * i);
    }
    return v;
}

/* 8-bit wrapping sum -- see the 'W' checksum note in the protocol comment
 * above. Doesn't need to be cryptographically strong, just needs a low
 * chance of accepting a batch that's shifted/corrupted by real link noise. */
static uint8_t checksum8(const uint8_t *buf, uint16_t len)
{
    uint8_t sum = 0;
    for (uint16_t i = 0; i < len; i++) {
        sum = (uint8_t)(sum + buf[i]);
    }
    return sum;
}

/* Checksum mismatches tolerated per batch (see g_last_status_reg's 0xCC)
 * before giving up. Each retry is cheap (milliseconds, no 5s timeout in the
 * common case -- a checksum mismatch means the bytes DID arrive, just wrong),
 * so this can afford to be generous: observed in practice needing more than
 * 8 retries on a single batch during an otherwise-clean multi-megabyte write
 * (cause unknown -- root-caused as far as "the link occasionally corrupts a
 * batch", not further than that), so this is deliberately higher than the
 * first cut. */
#define WRITE_BATCH_RETRY_MAX 32U

static void eeprom_dump(uint8_t addr_bytes, uint32_t start_addr, uint32_t size)
{
    const uint32_t num_chunks = size / CHUNK_SIZE;
    uint8_t cur = 0;

    CS_LOW();
    spi2_transfer(0x03); /* READ command */
    spi2_send_addr(addr_bytes, start_addr);

    spi2_fill(dma_buf[cur], CHUNK_SIZE); /* prime the first chunk before the pipeline starts */

    for (uint32_t chunk = 0; chunk < num_chunks; chunk++) {
        uint8_t sending = cur;
        dma_tx_start(dma_buf[sending], CHUNK_SIZE);

        if (chunk + 1 < num_chunks) {
            uint8_t filling = sending ^ 1;
            spi2_fill(dma_buf[filling], CHUNK_SIZE); /* overlaps with the DMA transfer above */
            cur = filling;
        }

        dma_tx_wait_done();
    }

    CS_HIGH();
}

static void spi2_write_enable(void)
{
    CS_LOW();
    spi2_transfer(0x06); /* WREN -- must be its own CS pulse, latch resets after each write cycle */
    CS_HIGH();
}

static uint8_t spi2_read_status(void)
{
    uint8_t sr;
    CS_LOW();
    spi2_transfer(0x05); /* RDSR */
    sr = spi2_transfer(0xFF);
    CS_HIGH();
    return sr;
}

/* Real elapsed time via SysTick (millis()), not a guessed loop-iteration count
 * -- a first attempt at bounding this with a raw iteration counter turned out
 * to be miscalibrated (fired before a genuine, slower-than-typical Chip-Erase
 * had actually finished, even though it was proceeding fine). 20s is well
 * above every documented write/erase time for this chip family (low ms to
 * tens of ms), leaving headroom for a slow/clone part; the host side's wait
 * for this ack is set to match (see WRITE_TIMEOUT_MS in index.html). */
#define WRITE_TIMEOUT_MS 20000UL

static int spi2_wait_write_done(void)
{
    uint32_t start = millis();
    uint8_t sr = spi2_read_status();
    while (sr & 0x01) { /* WIP/BUSY bit, bit0 on both EEPROM and NOR flash status regs */
        g_last_status_reg = sr;
        if ((uint32_t)(millis() - start) >= WRITE_TIMEOUT_MS) return 0; /* gave up -- chip never cleared BUSY */
        sr = spi2_read_status();
    }
    g_last_status_reg = sr;
    return 1;
}

/* Sends 0x06 on success, or 0x15 followed by g_last_status_reg on failure --
 * see that variable's comment for what the diagnostic byte means. Every NAK
 * site must go through this (not a bare usart1_send_byte(0x15)) so the host's
 * readAckOrThrow() always gets the diag byte it's now waiting for. */
static void send_ack_or_nak(int ok)
{
    if (ok) {
        usart1_send_byte(0x06);
    } else {
        usart1_send_byte(0x15);
        usart1_send_byte(g_last_status_reg);
    }
}

static int spi2_write_page(uint8_t addr_bytes, uint32_t addr, const uint8_t *data, uint8_t page_size)
{
    spi2_write_enable();

    CS_LOW();
    spi2_transfer(0x02); /* WRITE / Byte-Program command (same opcode on both EEPROM and NOR flash) */
    spi2_send_addr(addr_bytes, addr);
    for (uint8_t i = 0; i < page_size; i++) {
        spi2_transfer(data[i]);
    }
    CS_HIGH();

    return spi2_wait_write_done();
}

/* Receiving stalls USART1 (no RX FIFO) whenever we're not actively reading it,
 * so the host must never blast ahead of what we can consume. batch_size is how
 * many bytes we accept and physically write (in page_size-sized SPI writes)
 * before sending one 0x06 ACK; the host waits for that ACK before sending the
 * next batch, so no overrun is possible regardless of how big a batch is --
 * only page_size is dictated by the chip, batch_size is purely a round-trip/
 * performance knob (see the 'W' protocol note above). */
static void eeprom_program(uint8_t addr_bytes, uint8_t page_size, uint16_t batch_size, uint32_t size)
{
    /* page_size is a single byte on the wire, so a chip with a real 256-byte
     * page (e.g. P25Q32SH) must send page_size=128 or smaller from the host
     * side -- 256 wraps to 0 in a uint8_t. Guard here too: page_size==0 (or
     * batch_size not an exact multiple of it) would otherwise divide by zero
     * below and hang the MCU solid until power-cycled instead of just
     * NAK-ing the command. */
    if (page_size == 0 || batch_size == 0 || batch_size % page_size != 0 || batch_size > MAX_BATCH) {
        g_last_status_reg = 0xEE; /* header itself rejected -- never touched the chip */
        send_ack_or_nak(0);
        return;
    }
    const uint32_t num_batches = size / batch_size;
    const uint16_t pages_per_batch = batch_size / page_size;
    uint32_t addr = 0;

    for (uint32_t b = 0; b < num_batches; b++) {
        uint8_t retries = 0;
        for (;;) {
            for (uint16_t i = 0; i < batch_size; i++) {
                if (!usart1_recv_byte_timeout(&write_batch[i])) {
                    g_last_status_reg = 0xDD; /* host stopped sending mid-batch -- never touched the chip either */
                    send_ack_or_nak(0); /* NAK: host went quiet mid-batch -- give up instead of hanging forever */
                    return;
                }
            }
            uint8_t checksum;
            if (!usart1_recv_byte_timeout(&checksum)) {
                g_last_status_reg = 0xDD;
                send_ack_or_nak(0);
                return;
            }
            if (checksum8(write_batch, batch_size) == checksum) break; /* good batch -- program it below */
            if (++retries > WRITE_BATCH_RETRY_MAX) {
                g_last_status_reg = 0xCC; /* checksum kept failing -- link too unreliable, giving up */
                send_ack_or_nak(0);
                return;
            }
            usart1_send_byte(0x43); /* 'C': checksum mismatch, ask host to resend this exact batch */
        }
        for (uint16_t pg = 0; pg < pages_per_batch; pg++) {
            if (!spi2_write_page(addr_bytes, addr, &write_batch[(uint32_t)pg * page_size], page_size)) {
                send_ack_or_nak(0); /* NAK: this page never finished, chip stopped responding -- abort, skip verify */
                return;
            }
            addr += page_size;
        }
        send_ack_or_nak(1); /* ACK: this batch is committed and checksum-verified, host may send the next one */
    }

    eeprom_dump(addr_bytes, 0, size); /* stream the result back immediately so the host can verify */
}

static void eeprom_erase(uint8_t addr_bytes)
{
    (void)addr_bytes; /* Chip-Erase takes no address */
    spi2_write_enable();
    CS_LOW();
    spi2_transfer(0x60); /* Chip-Erase -- NOR flash only, sets every byte to 0xFF */
    CS_HIGH();
    send_ack_or_nak(spi2_wait_write_done()); /* ACK or NAK if BUSY never cleared */
}

static void eeprom_identify(void)
{
    CS_LOW();
    spi2_transfer(0x9F); /* JEDEC Read-ID -- no address phase */
    uint8_t mfg_id = spi2_transfer(0xFF);
    uint8_t mem_type = spi2_transfer(0xFF);
    uint8_t capacity = spi2_transfer(0xFF);
    CS_HIGH();

    usart1_send_byte(mfg_id);
    usart1_send_byte(mem_type);
    usart1_send_byte(capacity);
}

/* WREN also arms WRSR on chips that document "EWSR or WREN" (e.g. SST25VF032B),
 * so the existing write-enable helper works here without a separate EWSR (0x50)
 * path. Writing 0x00 clears BP0-BP3 (block protection) and BPL in one shot. */
static void eeprom_clear_protection(void)
{
    spi2_write_enable();
    CS_LOW();
    spi2_transfer(0x01); /* WRSR */
    spi2_transfer(0x00); /* new status: no protection */
    CS_HIGH();
    send_ack_or_nak(spi2_wait_write_done()); /* ACK or NAK if BUSY never cleared */
}

/* Mirror of eeprom_clear_protection(): sets BP1:BP0 = 11 (whole array
 * write-protected). Deliberately leaves SRWD (bit7) at 0 -- this keeps the
 * status register itself always writable via WRSR regardless of the WP# pin
 * (which this board ties straight to VCC, see the wiring notes above, so
 * WRSR never gets hardware-locked here anyway), so this is always reversible
 * with 'U' from this same reader without touching any pin. */
static void eeprom_protect_all(void)
{
    spi2_write_enable();
    CS_LOW();
    spi2_transfer(0x01); /* WRSR */
    spi2_transfer(0x0C); /* new status: BP1=1, BP0=1 -- whole array protected */
    CS_HIGH();
    send_ack_or_nak(spi2_wait_write_done()); /* ACK or NAK if BUSY never cleared */
}
