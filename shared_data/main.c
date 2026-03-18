/* src/main.c
 *
 * nRF54L15 + DHT22 + TFT(ST7735, Soft-SPI GPIO) + BLE NUS (RX/TX) + UART stall detector
 *
 * TFT wiring (Port 2):
 *   CS   P2.00
 *   SCK  P2.02
 *   MOSI P2.04
 *   RST  P2.06
 *   RS   P2.08   (D/C)
 *
 * DHT22: overlay defines DT_NODELABEL(dht22) on P0.04
 *
 * CHANGE in this version:
 * - Add global TEXT_XOFF/TEXT_YOFF
 * - Apply offset to ALL tft_draw_text() calls via TX()/TY() macros
 * - No panel offset, no XY swap, keep your working display orientation
 *
 * ADD:
 * - Active buzzer signal on P2.10 (GPIO, direct control; NO devicetree alias needed)
 *   connected   : short beep x2
 *   disconnected: long beep 1s
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/util.h>
#include <zephyr/sys/atomic.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <bluetooth/services/nus.h>

#include <dk_buttons_and_leds.h>
#include <stdio.h>
#include <string.h>

#include <cmsis_core.h>   /* NVIC_SystemReset() */

/* =========================
 * DHT22
 * ========================= */
static const struct device *dht_dev;

/* =========================
 * TFT GPIO (Port 2)
 * ========================= */
#define GPIO_TFT_NODE DT_NODELABEL(gpio2)
static const struct device *gpio_tft;

#define PIN_CS    0   /* P2.00 */
#define PIN_SCK   2   /* P2.02 */
#define PIN_MOSI  4   /* P2.04 */
#define PIN_RST   6   /* P2.06 */
#define PIN_RS    8   /* P2.08 (D/C) */

#define TFT_W 128
#define TFT_H 160

/* ===== Global text offset (ONLY text) ===== */
#define TEXT_XOFF 10  /* -> right */
#define TEXT_YOFF 12  /* -> down  */
#define TX(x) ((uint16_t)((x) + TEXT_XOFF))
#define TY(y) ((uint16_t)((y) + TEXT_YOFF))

/* RGB565 */
#define BLACK   0x0000
#define WHITE   0xFFFF
#define RED     0xF800
#define GREEN   0x07E0
#define BLUE    0x001F
#define YELLOW  0xFFE0
#define CYAN    0x07FF

/* =========================
 * BLE state
 * ========================= */
static atomic_t bt_connected = ATOMIC_INIT(0);
static atomic_t nus_notification_enabled = ATOMIC_INIT(0);

static struct bt_conn *current_conn;
static struct k_mutex conn_lock;

static struct k_work_delayable adv_restart_work;
static int bt_start_advertising(void);

/* =========================
 * Active buzzer (GPIO direct on P2.10)
 * ========================= */
#define PIN_BUZZ 10 /* P2.10 */
static atomic_t buzzer_ready = ATOMIC_INIT(0);

enum buzzer_pattern {
	BUZZ_NONE = 0,
	BUZZ_CONN_2SHORT,
	BUZZ_DISC_1S,
};

static struct k_work_delayable buzzer_work;
static enum buzzer_pattern buzz_pat = BUZZ_NONE;
static uint8_t buzz_step;

static inline void buzzer_set(bool on)
{
	if (!atomic_get(&buzzer_ready)) return;
	/* If your module is active-low, flip 1/0 here */
	gpio_pin_set(gpio_tft, PIN_BUZZ, on ? 1:0);
}

static void buzzer_start(enum buzzer_pattern pat)
{
	if (!atomic_get(&buzzer_ready)) return;

	buzz_pat = pat;
	buzz_step = 0;
	k_work_cancel_delayable(&buzzer_work);
	k_work_schedule(&buzzer_work, K_NO_WAIT);
}

static void buzzer_work_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	if (!atomic_get(&buzzer_ready) || buzz_pat == BUZZ_NONE) {
		buzzer_set(false);
		return;
	}

	switch (buzz_pat) {
	case BUZZ_CONN_2SHORT:
		/* step 0: ON 200ms, 1: OFF 120ms, 2: ON 200ms, 3: OFF end */
		if (buzz_step == 0) {
			buzzer_set(true);
			buzz_step = 1;
			k_work_schedule(&buzzer_work, K_MSEC(200));
		} else if (buzz_step == 1) {
			buzzer_set(false);
			buzz_step = 2;
			k_work_schedule(&buzzer_work, K_MSEC(120));
		} else if (buzz_step == 2) {
			buzzer_set(true);
			buzz_step = 3;
			k_work_schedule(&buzzer_work, K_MSEC(200));
		} else {
			buzzer_set(false);
			buzz_pat = BUZZ_NONE;
		}
		break;

	case BUZZ_DISC_1S:
		/* step 0: ON 1000ms, 1: OFF end */
		if (buzz_step == 0) {
			buzzer_set(true);
			buzz_step = 1;
			k_work_schedule(&buzzer_work, K_MSEC(1000));
		} else {
			buzzer_set(false);
			buzz_pat = BUZZ_NONE;
		}
		break;

	default:
		buzzer_set(false);
		buzz_pat = BUZZ_NONE;
		break;
	}
}

/* =========================
 * Data
 * ========================= */
static double last_t = 0.0;
static double last_h = 0.0;
static atomic_t last_valid = ATOMIC_INIT(0);

/* =========================
 * UART stall detector
 * ========================= */
static atomic_t uart_heartbeat_ms = ATOMIC_INIT(0); /* k_uptime_get_32() */
static atomic_t last_dbg_code = ATOMIC_INIT(0);

enum {
	DBG_BOOT = 10,
	DBG_BT_READY = 20,
	DBG_BT_ADV = 21,
	DBG_BT_CONN = 22,
	DBG_BT_DISC = 23,
	DBG_DHT_OK = 30,
	DBG_DHT_FAIL = 31,
	DBG_NUS_TX = 40,
	DBG_NUS_RX = 41,
	DBG_TFT_OK = 50,
	DBG_TFT_DRAW = 51,
	DBG_STALL_REBOOT = 90,
};

static inline void uart_feed(int code)
{
	atomic_set(&last_dbg_code, code);
	atomic_set(&uart_heartbeat_ms, (int32_t)k_uptime_get_32());
	printk("[DBG] CODE=%d UPT=%u\n", code, (uint32_t)k_uptime_get_32());
}

static void uart_stall_thread(void)
{
	const uint32_t STALL_MS = 5000;
	while (1) {
		uint32_t now = (uint32_t)k_uptime_get_32();
		uint32_t last = (uint32_t)atomic_get(&uart_heartbeat_ms);

		if (last != 0 && (now - last) > STALL_MS) {
			printk("[STALL] > %u ms, LAST_CODE=%ld -> reboot\n",
			       STALL_MS, (long)atomic_get(&last_dbg_code));
			uart_feed(DBG_STALL_REBOOT);
			k_sleep(K_MSEC(50));
			NVIC_SystemReset();
		}
		k_sleep(K_MSEC(250));
	}
}

K_THREAD_DEFINE(uart_stall_tid, 1024, uart_stall_thread, NULL, NULL, NULL,
		K_PRIO_PREEMPT(8), 0, 0);

/* =========================
 * TFT Soft-SPI low-level
 * ========================= */
static inline void tft_cs(int v)  { gpio_pin_set(gpio_tft, PIN_CS, v); }
static inline void tft_sck(int v) { gpio_pin_set(gpio_tft, PIN_SCK, v); }
static inline void tft_mosi(int v){ gpio_pin_set(gpio_tft, PIN_MOSI, v); }
static inline void tft_rs(int v)  { gpio_pin_set(gpio_tft, PIN_RS, v); }
static inline void tft_rst(int v) { gpio_pin_set(gpio_tft, PIN_RST, v); }

static void tft_spi_write8(uint8_t b)
{
	for (int i = 0; i < 8; i++) {
		tft_sck(0);
		tft_mosi((b & 0x80) ? 1 : 0);
		tft_sck(1);
		b <<= 1;
	}
	tft_sck(0);
}

static void tft_write_cmd(uint8_t cmd)
{
	tft_cs(0);
	tft_rs(0);
	tft_spi_write8(cmd);
	tft_cs(1);
}

static void tft_write_data8(uint8_t d)
{
	tft_cs(0);
	tft_rs(1);
	tft_spi_write8(d);
	tft_cs(1);
}

static void tft_write_data16(uint16_t d)
{
	tft_cs(0);
	tft_rs(1);
	tft_spi_write8((uint8_t)(d >> 8));
	tft_spi_write8((uint8_t)(d & 0xFF));
	tft_cs(1);
}

/* ST7735 commands */
#define ST7735_SWRESET 0x01
#define ST7735_SLPOUT  0x11
#define ST7735_COLMOD  0x3A
#define ST7735_MADCTL  0x36
#define ST7735_CASET   0x2A
#define ST7735_RASET   0x2B
#define ST7735_RAMWR   0x2C
#define ST7735_DISPON  0x29
#define ST7735_INVON   0x21
#define ST7735_NORON   0x13

static void tft_set_addr_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
	tft_write_cmd(ST7735_CASET);
	tft_write_data8(0x00); tft_write_data8(x0);
	tft_write_data8(0x00); tft_write_data8(x1);

	tft_write_cmd(ST7735_RASET);
	tft_write_data8(0x00); tft_write_data8(y0);
	tft_write_data8(0x00); tft_write_data8(y1);

	tft_write_cmd(ST7735_RAMWR);
}

static void tft_fill(uint16_t color)
{
	tft_set_addr_window(0, 0, TFT_W - 1, TFT_H - 1);
	tft_cs(0);
	tft_rs(1);
	for (int i = 0; i < TFT_W * TFT_H; i++) {
		tft_spi_write8((uint8_t)(color >> 8));
		tft_spi_write8((uint8_t)(color & 0xFF));
	}
	tft_cs(1);
}

/* Small font glyphs */
typedef struct { char c; uint8_t col[5]; } glyph5x7_t;
static const glyph5x7_t font_small[] = {
	{' ', {0x00,0x00,0x00,0x00,0x00}},
	{':', {0x00,0x36,0x36,0x00,0x00}},
	{'.', {0x00,0x60,0x60,0x00,0x00}},
	{'=', {0x14,0x14,0x14,0x14,0x14}},
	{'%', {0x62,0x64,0x08,0x13,0x23}},
	{'C', {0x3E,0x41,0x41,0x41,0x22}},
	{'B', {0x7F,0x49,0x49,0x49,0x36}},
	{'T', {0x01,0x01,0x7F,0x01,0x01}},
	{'H', {0x7F,0x08,0x08,0x08,0x7F}},
	{'O', {0x3E,0x41,0x41,0x41,0x3E}},
	{'K', {0x7F,0x08,0x14,0x22,0x41}},
	{'R', {0x7F,0x08,0x14,0x22,0x41}},
	{'X', {0x63,0x14,0x08,0x14,0x63}},
	{'0', {0x3E,0x51,0x49,0x45,0x3E}},
	{'1', {0x00,0x42,0x7F,0x40,0x00}},
	{'2', {0x42,0x61,0x51,0x49,0x46}},
	{'3', {0x21,0x41,0x45,0x4B,0x31}},
	{'4', {0x18,0x14,0x12,0x7F,0x10}},
	{'5', {0x27,0x45,0x45,0x45,0x39}},
	{'6', {0x3C,0x4A,0x49,0x49,0x30}},
	{'7', {0x01,0x71,0x09,0x05,0x03}},
	{'8', {0x36,0x49,0x49,0x49,0x36}},
	{'9', {0x06,0x49,0x49,0x29,0x1E}},
	{'-', {0x08,0x08,0x08,0x08,0x08}},
};

static const uint8_t *glyph_lookup(char c)
{
	for (size_t i = 0; i < ARRAY_SIZE(font_small); i++) {
		if (font_small[i].c == c) return font_small[i].col;
	}
	return font_small[0].col;
}

static void tft_draw_pixel(uint16_t x, uint16_t y, uint16_t color)
{
	if (x >= TFT_W || y >= TFT_H) return;
	tft_set_addr_window(x, y, x, y);
	tft_write_data16(color);
}

static void tft_draw_char(uint16_t x, uint16_t y, char c, uint16_t fg, uint16_t bg, uint8_t scale)
{
	const uint8_t *col = glyph_lookup(c);
	for (int cx = 0; cx < 5; cx++) {
		uint8_t bits = col[cx];
		for (int cy = 0; cy < 7; cy++) {
			uint16_t color = (bits & (1u << cy)) ? fg : bg;
			for (int sx = 0; sx < scale; sx++) {
				for (int sy = 0; sy < scale; sy++) {
					tft_draw_pixel(x + cx*scale + sx, y + cy*scale + sy, color);
				}
			}
		}
	}
	for (int cy = 0; cy < 7*scale; cy++) {
		for (int sx = 0; sx < scale; sx++) {
			tft_draw_pixel(x + 5*scale + sx, y + cy, bg);
		}
	}
}

static void tft_draw_text(uint16_t x, uint16_t y, const char *s, uint16_t fg, uint16_t bg, uint8_t scale)
{
	while (*s) {
		tft_draw_char(x, y, *s, fg, bg, scale);
		x += (6 * scale);
		s++;
	}
}

static int tft_init_softspi_st7735(void)
{
	gpio_tft = DEVICE_DT_GET(GPIO_TFT_NODE);
	if (!device_is_ready(gpio_tft)) {
		printk("[TFT] gpio2 not ready\n");
		return -ENODEV;
	}

	gpio_pin_configure(gpio_tft, PIN_CS,   GPIO_OUTPUT_ACTIVE);
	gpio_pin_configure(gpio_tft, PIN_SCK,  GPIO_OUTPUT_INACTIVE);
	gpio_pin_configure(gpio_tft, PIN_MOSI, GPIO_OUTPUT_INACTIVE);
	gpio_pin_configure(gpio_tft, PIN_RST,  GPIO_OUTPUT_ACTIVE);
	gpio_pin_configure(gpio_tft, PIN_RS,   GPIO_OUTPUT_INACTIVE);

	/* Buzzer pin init (ACTIVE buzzer, active-high) */
        if (gpio_pin_configure(gpio_tft, PIN_BUZZ, GPIO_OUTPUT) == 0) {
	        atomic_set(&buzzer_ready, 1);
	        gpio_pin_set(gpio_tft, PIN_BUZZ, 0); /* force OFF */
        }       

	tft_cs(1);
	tft_sck(0);
	tft_mosi(0);

	tft_rst(0);
	k_sleep(K_MSEC(50));
	tft_rst(1);
	k_sleep(K_MSEC(120));

	tft_write_cmd(ST7735_SWRESET);
	k_sleep(K_MSEC(150));

	tft_write_cmd(ST7735_SLPOUT);
	k_sleep(K_MSEC(120));

	tft_write_cmd(ST7735_COLMOD);
	tft_write_data8(0x05);
	k_sleep(K_MSEC(10));

	tft_write_cmd(ST7735_MADCTL);
	tft_write_data8(0xC8);

	tft_write_cmd(ST7735_INVON);
	k_sleep(K_MSEC(10));

	tft_write_cmd(ST7735_NORON);
	k_sleep(K_MSEC(10));

	tft_write_cmd(ST7735_DISPON);
	k_sleep(K_MSEC(120));

	tft_fill(BLACK);
	return 0;
}

/* =========================
 * Advertising restart work
 * ========================= */
static void adv_restart_work_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	int s = bt_le_adv_stop();
	if (s && s != -EALREADY && s != -EINVAL) {
		/* ignore */
	}

	int err = bt_start_advertising();
	if (err) {
		printk("[BT] ⚠️ 重新廣播失敗 (err=%d)\n", err);
		if (err == -ENOMEM) {
			k_work_schedule(&adv_restart_work, K_MSEC(500));
		}
	} else {
		printk("[BT] 📡 已重新開始廣播\n");
		uart_feed(DBG_BT_ADV);
	}
}

/* =========================
 * NUS callbacks (RX/TX)
 * ========================= */
static void nus_rx_cb(struct bt_conn *conn, const uint8_t *const data, uint16_t len)
{
	ARG_UNUSED(conn);

	uart_feed(DBG_NUS_RX);

	char buf[32];
	uint16_t n = MIN(len, (uint16_t)(sizeof(buf) - 1));
	memcpy(buf, data, n);
	buf[n] = '\0';

	printk("[BT][NUS RX] %s\n", buf);

	if (gpio_tft && device_is_ready(gpio_tft)) {
		tft_draw_text(TX(0),  TY(70), "RX:", CYAN, BLACK, 2);
		tft_draw_text(TX(36), TY(70), buf,  CYAN, BLACK, 2);
	}

	if (atomic_get(&bt_connected) && atomic_get(&nus_notification_enabled)) {
		struct bt_conn *conn_ref = NULL;

		k_mutex_lock(&conn_lock, K_FOREVER);
		if (current_conn) conn_ref = bt_conn_ref(current_conn);
		k_mutex_unlock(&conn_lock);

		if (conn_ref) {
			bt_nus_send(conn_ref, buf, strlen(buf));
			bt_conn_unref(conn_ref);
		}
	}
}

static void nus_send_enabled_cb(enum bt_nus_send_status status)
{
	if (status == BT_NUS_SEND_STATUS_ENABLED) {
		printk("[BT] 🔔 Notify enabled\n");
		atomic_set(&nus_notification_enabled, 1);
	} else {
		printk("[BT] 🔕 Notify disabled\n");
		atomic_set(&nus_notification_enabled, 0);
	}
}

static struct bt_nus_cb nus_cb = {
	.received = nus_rx_cb,
	.send_enabled = nus_send_enabled_cb,
};

/* =========================
 * Connection callbacks
 * ========================= */
static void connected_cb(struct bt_conn *conn, uint8_t err)
{
	if (err) {
		printk("[BT] 連線失敗 (err=%u)\n", err);
		return;
	}

	printk("[BT] ✅ 已連線\n");
	buzzer_start(BUZZ_CONN_2SHORT);
	uart_feed(DBG_BT_CONN);

	k_mutex_lock(&conn_lock, K_FOREVER);
	if (current_conn) {
		bt_conn_unref(current_conn);
		current_conn = NULL;
	}
	current_conn = bt_conn_ref(conn);
	k_mutex_unlock(&conn_lock);

	atomic_set(&bt_connected, 1);
	atomic_set(&nus_notification_enabled, 0);

	dk_set_led(DK_LED2, 1);

	if (gpio_tft && device_is_ready(gpio_tft)) {
		tft_draw_text(TX(0), TY(0), "BT:OK", GREEN, BLACK, 2);
	}
}

static void disconnected_cb(struct bt_conn *conn, uint8_t reason)
{
	ARG_UNUSED(conn);

	buzzer_start(BUZZ_DISC_1S);

	printk("[BT] ❌ 已斷線 (reason=0x%02X)\n", reason);
	uart_feed(DBG_BT_DISC);

	atomic_set(&bt_connected, 0);
	atomic_set(&nus_notification_enabled, 0);
	dk_set_led(DK_LED2, 0);

	k_mutex_lock(&conn_lock, K_FOREVER);
	if (current_conn) {
		bt_conn_unref(current_conn);
		current_conn = NULL;
	}
	k_mutex_unlock(&conn_lock);

	if (gpio_tft && device_is_ready(gpio_tft)) {
		tft_draw_text(TX(0), TY(0), "BT:--", YELLOW, BLACK, 2);
	}

	k_work_schedule(&adv_restart_work, K_MSEC(200));
}

static struct bt_conn_cb conn_callbacks = {
	.connected = connected_cb,
	.disconnected = disconnected_cb,
};

/* =========================
 * Advertising
 * ========================= */
static int bt_start_advertising(void)
{
	static const uint8_t ad_flags = (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR);
	static const uint8_t nus_uuid128[16] = {
		0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
		0x93, 0xf3, 0xa3, 0xb5, 0x01, 0x00, 0x40, 0x6e
	};
	static const struct bt_data ad[] = {
		BT_DATA(BT_DATA_FLAGS, &ad_flags, sizeof(ad_flags)),
		BT_DATA(BT_DATA_UUID128_ALL, nus_uuid128, sizeof(nus_uuid128)),
	};
	static const struct bt_data sd[] = {
		BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME, sizeof(CONFIG_BT_DEVICE_NAME) - 1),
	};

	int err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_2, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
	if (!err) {
		printk("[BT] 📡 Advertising\n");
	}
	return err;
}

/* =========================
 * Main
 * ========================= */
int main(void)
{
	int err;

	uart_feed(DBG_BOOT);

	dk_leds_init();
	dk_set_led(DK_LED1, 1);

	k_mutex_init(&conn_lock);
	k_work_init_delayable(&adv_restart_work, adv_restart_work_fn);
	k_work_init_delayable(&buzzer_work, buzzer_work_fn);

	printk("nRF54L15 DHT22 + TFT(SoftSPI) + NUS(RX/TX) + UART Watchdog\n");

	/* DHT22 device (overlay: DT_NODELABEL(dht22)) */
	dht_dev = DEVICE_DT_GET(DT_NODELABEL(dht22));
	if (!device_is_ready(dht_dev)) {
		printk("[DHT] device not ready\n");
		return 0;
	}

	/* TFT init (also config buzzer P2.10 on gpio2) */
	err = tft_init_softspi_st7735();
	if (err) {
		printk("[TFT] init failed (err=%d)\n", err);
	} else {
		uart_feed(DBG_TFT_OK);
		tft_draw_text(TX(0), TY(0),  "BT:--",  YELLOW, BLACK, 2);
		tft_draw_text(TX(0), TY(20), "T:--.-C", WHITE,  BLACK, 2);
		tft_draw_text(TX(0), TY(45), "H:--.-%", WHITE,  BLACK, 2);
		tft_draw_text(TX(0), TY(70), "RX:",    CYAN,   BLACK, 2);
	}

	/* BLE init */
	bt_conn_cb_register(&conn_callbacks);

	err = bt_nus_init(&nus_cb);
	if (err) {
		printk("[BT] NUS init failed (err=%d)\n", err);
		return 0;
	}

	err = bt_enable(NULL);
	if (err) {
		printk("[BT] bt_enable failed (err=%d)\n", err);
		return 0;
	}
	uart_feed(DBG_BT_READY);

	err = bt_start_advertising();
	if (err) {
		printk("[BT] adv start failed (err=%d)\n", err);
	} else {
		uart_feed(DBG_BT_ADV);
	}

	while (1) {
		uart_feed(DBG_TFT_DRAW);

		/* Read DHT22 */
		struct sensor_value temp, hum;
		int rc = sensor_sample_fetch(dht_dev);
		if (rc == 0) {
			sensor_channel_get(dht_dev, SENSOR_CHAN_AMBIENT_TEMP, &temp);
			sensor_channel_get(dht_dev, SENSOR_CHAN_HUMIDITY, &hum);

			last_t = sensor_value_to_double(&temp);
			last_h = sensor_value_to_double(&hum);
			atomic_set(&last_valid, 1);

			printk("[DHT] T=%.1f H=%.1f\n", last_t, last_h);
			uart_feed(DBG_DHT_OK);
		} else {
			printk("[DHT] fetch failed (rc=%d)\n", rc);
			uart_feed(DBG_DHT_FAIL);
		}

		/* TFT update */
		if (gpio_tft && device_is_ready(gpio_tft) && atomic_get(&last_valid)) {
			char line1[16], line2[16];
			snprintk(line1, sizeof(line1), "T:%.1fC", last_t);
			snprintk(line2, sizeof(line2), "H:%.1f%%", last_h);

			tft_draw_text(TX(0), TY(20), "T:--.-C", BLACK, BLACK, 2);
			tft_draw_text(TX(0), TY(45), "H:--.-%", BLACK, BLACK, 2);

			tft_draw_text(TX(0), TY(20), line1, WHITE, BLACK, 2);
			tft_draw_text(TX(0), TY(45), line2, WHITE, BLACK, 2);

			if (atomic_get(&bt_connected)) {
				tft_draw_text(TX(0), TY(0), "BT:OK", GREEN, BLACK, 2);
			} else {
				tft_draw_text(TX(0), TY(0), "BT:--", YELLOW, BLACK, 2);
			}
		}

		/* NUS TX */
		if (atomic_get(&bt_connected) && atomic_get(&nus_notification_enabled) && atomic_get(&last_valid)) {
			struct bt_conn *conn_ref = NULL;

			k_mutex_lock(&conn_lock, K_FOREVER);
			if (current_conn) conn_ref = bt_conn_ref(current_conn);
			k_mutex_unlock(&conn_lock);

			if (conn_ref) {
				char msg[64];
				int n = snprintk(msg, sizeof(msg), "T=%.1fC H=%.1f%%\r\n", last_t, last_h);

				int s_err = bt_nus_send(conn_ref, msg, n);
				if (s_err) {
					printk("[NUS] send failed (err=%d)\n", s_err);
				} else {
					printk("[NUS] TX: %s", msg);
					uart_feed(DBG_NUS_TX);
				}
				bt_conn_unref(conn_ref);
			}
		}

		/* Heartbeat LED */
		dk_set_led(DK_LED1, 1);
		k_sleep(K_MSEC(120));
		dk_set_led(DK_LED1, 0);

		k_sleep(K_SECONDS(2));
	}
}
