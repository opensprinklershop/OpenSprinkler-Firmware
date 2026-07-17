/* OpenSprinkler Unified Firmware
 *
 * ESP32-C5 hardware self-test (board bring-up / QC)
 *
 * This module runs a one-shot hardware self-test on the ESP32-C5-WROOM-1
 * OpenSprinkler board. It is intended for boards that are hand-soldered:
 * on the very first boot (i.e. whenever the completion marker DONE_FILENAME
 * is missing) it automatically exercises the on-board peripherals and GPIOs
 * and reports faulty devices/pins on the OLED display.
 *
 * User interaction at the end of the test:
 *   - If errors were found:  B1 = reboot (re-run test),  B3 = continue
 *   - If no errors:          B3 = continue
 *
 * All checks are non-destructive:
 *   - I2C peripherals are only probed / read back
 *   - the IO-expander config register is re-written with its own value
 *   - the shift register is clocked with an all-OFF pattern (no station on)
 *   - free GPIOs are tested with internal pull-up/pull-down only (short check)
 *
 * This file is only compiled for ESP32-C5 targets.
 */

#include "defines.h"

#if defined(ESP32C5)

#include <Arduino.h>
#include "OpenSprinkler.h"
#include "gpio.h"
#include "utils.h"
#include "I2CRTC.h"

extern OpenSprinkler os;

// ---------------------------------------------------------------------------
// Result bookkeeping
// ---------------------------------------------------------------------------
#define HWT_MAX_TESTS 24

struct HwTestResult {
	const char *name;   // short display name (<= 16 chars)
	bool ok;            // true = pass
	bool required;      // true = a failure counts as a hardware fault
	bool present;       // for optional devices: was the device detected?
};

static HwTestResult hwt_results[HWT_MAX_TESTS];
static uint8_t hwt_count = 0;

static void hwt_add(const char *name, bool ok, bool required, bool present = true) {
	if (hwt_count >= HWT_MAX_TESTS) return;
	hwt_results[hwt_count].name = name;
	hwt_results[hwt_count].ok = ok;
	hwt_results[hwt_count].required = required;
	hwt_results[hwt_count].present = present;
	hwt_count++;
	DEBUG_PRINTF("[HWTEST] %-14s : %s%s\n", name,
		ok ? "PASS" : "FAIL",
		required ? "" : (present ? " (opt)" : " (opt, absent)"));
}

// ---------------------------------------------------------------------------
// Display helpers (SSD1306 wrapper: 4 rows -1..2, 16 columns)
// ---------------------------------------------------------------------------
static void hwt_lcd_line(int8_t row, const char *s) {
	char buf[17];
	uint8_t n = 0;
	while (s[n] && n < 16) { buf[n] = s[n]; n++; }
	for (; n < 16; n++) buf[n] = ' ';
	buf[16] = 0;
	os.lcd.setCursor(0, row);
	os.lcd.print(buf);
}

// ---------------------------------------------------------------------------
// Individual test helpers
// ---------------------------------------------------------------------------

// Probe an I2C address (returns true if a device ACKs).
static bool hwt_i2c_present(int addr) {
	return detect_i2c(addr);
}

// Non-destructive short check on a GPIO using the internal pull resistors.
// Returns true if the pin can be pulled both HIGH and LOW (i.e. not shorted).
static bool hwt_gpio_short_ok(uint8_t pin) {
	pinMode(pin, INPUT_PULLUP);
	delayMicroseconds(200);
	int hi = digitalRead(pin);   // expect HIGH; LOW => short to GND
	pinMode(pin, INPUT_PULLDOWN);
	delayMicroseconds(200);
	int lo = digitalRead(pin);   // expect LOW; HIGH => short to VCC
	pinMode(pin, INPUT);         // restore high-impedance
	return (hi == HIGH && lo == LOW);
}

// Read a (possibly IO-expander) input pin, expecting it released/HIGH.
static bool hwt_input_released(uint8_t pin) {
	pinModeExt(pin, INPUT_PULLUP);
	delay(2);
	for (uint8_t i = 0; i < 5; i++) {
		if (digitalReadExt(pin) == 0) return false; // stuck low / shorted
		delay(3);
	}
	return true;
}

// ---------------------------------------------------------------------------
// Test runners
// ---------------------------------------------------------------------------
static void hwt_run_all() {
	hwt_count = 0;

	// 1) OLED display present on the I2C bus
	hwt_add("Display OLED", hwt_i2c_present(LCD_I2CADDR), true);

	// 2) Main IO-expander present
	bool ioexp_present = (os.mainio != NULL) && hwt_i2c_present(os.mainio->address);
	hwt_add("IO-Expander", ioexp_present, true);

	// 3) IO-expander I2C read/write (re-write config reg with its own value)
	bool ioexp_rw = false;
	if (ioexp_present) {
		os.mainio->i2c_write(NXP_CONFIG_REG, IO_CONFIG);
		uint16_t rb = os.mainio->i2c_read(NXP_CONFIG_REG);
		ioexp_rw = (rb == IO_CONFIG);
	}
	hwt_add("IOexp I2C R/W", ioexp_rw, true);

	// 4) RTC (optional: DS1307 / PCF8563 / MCP7940)
	bool rtc_present = hwt_i2c_present(DS1307_ADDR) ||
	                   hwt_i2c_present(PCF8563_ADDR) ||
	                   hwt_i2c_present(MCP7940_ADDR);
	hwt_add("RTC clock", rtc_present, false, rtc_present);

	// 5) EEPROM (optional, 24Cxx @ 0x50)
	bool eeprom_present = hwt_i2c_present(EEPROM_I2CADDR);
	hwt_add("EEPROM", eeprom_present, false, eeprom_present);

	// 6) External QIO flash / LittleFS: write+read back a temp file
	const char *tf = "hwtst.tmp";
	remove_file(tf);
	file_write_byte(tf, 0, 0xA5);
	bool flash_ok = file_exists(tf) && (file_read_byte(tf, 0) == 0xA5);
	remove_file(tf);
	hwt_add("Ext.Flash FS", flash_ok, true);

	// 7-9) Buttons must read released (HIGH) at rest
	hwt_add("Button B1", hwt_input_released(PIN_BUTTON_1), true);
	hwt_add("Button B2", hwt_input_released(PIN_BUTTON_2), true);
	hwt_add("Button B3", hwt_input_released(PIN_BUTTON_3), true);

	// 10-11) Sensor inputs pulled up at rest (open = HIGH)
	hwt_add("Sensor1 in", hwt_input_released(PIN_SENSOR1), true);
	hwt_add("Sensor2 in", hwt_input_released(PIN_SENSOR2), true);

	// 12-14) Free GPIOs: short-to-rail check
	hwt_add("GPIO25", hwt_gpio_short_ok(PIN_FREE1), true);
	hwt_add("GPIO26", hwt_gpio_short_ok(PIN_FREE2), true);
	hwt_add("GPIO7",  hwt_gpio_short_ok(PIN_FREE3), true);

	// 15) Shift register / station latch drive: clock an all-OFF pattern
	//     (safe: no station is energised) and verify the expander still ACKs.
	bool sr_ok = false;
	if (ioexp_present) {
		os.mainio->shift_out(V2_PIN_SRLAT, V2_PIN_SRCLK, V2_PIN_SRDAT, 0x00);
		sr_ok = hwt_i2c_present(os.mainio->address);
	}
	hwt_add("ShiftReg/Stn", sr_ok, true);

	// restore sensor pins to their normal input mode
	pinModeExt(PIN_SENSOR1, INPUT_PULLUP);
	pinModeExt(PIN_SENSOR2, INPUT_PULLUP);
}

// ---------------------------------------------------------------------------
// Result screens / interaction
// ---------------------------------------------------------------------------

// Collect the names of failed required tests into the given array.
static uint8_t hwt_collect_failures(const char *out[], uint8_t maxn) {
	uint8_t n = 0;
	for (uint8_t i = 0; i < hwt_count && n < maxn; i++) {
		if (hwt_results[i].required && !hwt_results[i].ok) {
			out[n++] = hwt_results[i].name;
		}
	}
	return n;
}

static bool hwt_button_down(unsigned char mask) {
	unsigned char b = os.button_read(BUTTON_WAIT_NONE);
	return ((b & BUTTON_MASK) == mask) && (b & BUTTON_FLAG_DOWN);
}

// Screen shown when all required tests passed. B3 continues.
static void hwt_screen_pass() {
	char line[17];
	uint8_t npass = 0;
	for (uint8_t i = 0; i < hwt_count; i++)
		if (hwt_results[i].ok) npass++;
	snprintf(line, sizeof(line), "%u/%u checks OK", npass, hwt_count);

	os.lcd.clear();
	hwt_lcd_line(-1, "HW SELF-TEST");
	hwt_lcd_line(0, "ALL PASSED");
	hwt_lcd_line(1, line);
	hwt_lcd_line(2, "B3: continue");

	while (true) {
		if (hwt_button_down(BUTTON_3)) return;
		delay(10);
	}
}

// Screen shown when at least one required test failed.
// Pages through the failed items; B1 reboots, B3 continues.
static void hwt_screen_fail(const char *fails[], uint8_t nfail) {
	char title[17];
	snprintf(title, sizeof(title), "HW FAIL: %u", nfail);

	uint8_t npages = (nfail + 1) / 2; // 2 items per page
	if (npages == 0) npages = 1;
	uint8_t page = 0;
	ulong last_flip = millis();

	auto render = [&](uint8_t pg) {
		os.lcd.clear();
		hwt_lcd_line(-1, title);
		uint8_t base = pg * 2;
		hwt_lcd_line(0, base < nfail ? fails[base] : "");
		hwt_lcd_line(1, (uint8_t)(base + 1) < nfail ? fails[base + 1] : "");
		hwt_lcd_line(2, "B1:Reboot B3:Go");
	};

	render(page);

	while (true) {
		if (hwt_button_down(BUTTON_1)) {
			os.lcd.clear();
			hwt_lcd_line(0, "Rebooting...");
			delay(400);
			os.reboot_dev(REBOOT_CAUSE_BUTTON);
			return; // not reached
		}
		if (hwt_button_down(BUTTON_3)) {
			return;
		}
		if (hwt_button_down(BUTTON_2) && npages > 1) {
			page = (page + 1) % npages;
			render(page);
			last_flip = millis();
		}
		// auto-advance pages so all failures are visible without pressing B2
		if (npages > 1 && (millis() - last_flip) > 2500) {
			page = (page + 1) % npages;
			render(page);
			last_flip = millis();
		}
		delay(10);
	}
}

// ---------------------------------------------------------------------------
// Public entry point
// ---------------------------------------------------------------------------
void OpenSprinkler::hardware_selftest() {
	DEBUG_PRINTLN(F("[HWTEST] ===== ESP32-C5 hardware self-test ====="));

	os.lcd.clear();
	hwt_lcd_line(-1, "HW SELF-TEST");
	hwt_lcd_line(0, "Testing...");
	hwt_lcd_line(1, "Please wait");
	hwt_lcd_line(2, "");

	hwt_run_all();

	// count failed required tests
	const char *fails[HWT_MAX_TESTS];
	uint8_t nfail = hwt_collect_failures(fails, HWT_MAX_TESTS);

	DEBUG_PRINTF("[HWTEST] complete: %u check(s), %u failure(s)\n", hwt_count, nfail);

	if (nfail == 0) {
		hwt_screen_pass();
	} else {
		hwt_screen_fail(fails, nfail);
	}

	os.lcd.clear();
	DEBUG_PRINTLN(F("[HWTEST] ===== self-test finished ====="));
}

#endif // ESP32C5
