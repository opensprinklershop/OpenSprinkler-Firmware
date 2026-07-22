/* OpenSprinkler Unified Firmware
 *
 * ESP32-C5 hardware self-test (board bring-up / QC)
 *
 * This module runs an on-device hardware self-test on the ESP32-C5-WROOM-1
 * OpenSprinkler board. It is intended for boards that are hand-soldered:
 * it is launched by HOLDING B1+B2 together during boot (from
 * OpenSprinkler::begin(), BEFORE the external-flash filesystem is mounted),
 * exercises the on-board peripherals and GPIOs, and reports faulty devices/pins
 * on the OLED display. It performs NO filesystem access, so a board with a
 * faulty external flash can still be diagnosed.
 *
 * The result is shown on the display; failed items auto-cycle. The test always
 * exits by resetting the device (any button press triggers the reset).
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
#define HWT_MAX_PAIRS 8   // capacity for adjacent-pad pairs (>= hwt_adj_pairs count)

struct HwTestResult {
	const char *name;   // short display name (<= 16 chars)
	bool ok;            // true = pass
	bool required;      // true = a failure counts as a hardware fault
	bool present;       // for optional devices: was the device detected?
};

// All self-test state lives in this context. It is allocated on the stack for
// the duration of the test only, so the self-test reserves NO permanent RAM
// (no file-scope static variables).
struct HwtContext {
	HwTestResult results[HWT_MAX_TESTS];
	char pair_name[HWT_MAX_PAIRS][17]; // storage for generated bridge-test names
	uint8_t count;
};

static void hwt_add(HwtContext &ctx, const char *name, bool ok, bool required, bool present = true) {
	if (ctx.count >= HWT_MAX_TESTS) return;
	ctx.results[ctx.count].name = name;
	ctx.results[ctx.count].ok = ok;
	ctx.results[ctx.count].required = required;
	ctx.results[ctx.count].present = present;
	ctx.count++;
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
// Adjacent-pad short (solder bridge) detection
// ---------------------------------------------------------------------------
// The ESP32-C5-WROOM-1 module is hand-soldered, so a solder bridge can short
// two PHYSICALLY adjacent castellated pads. The pads are NOT wired in GPIO-number
// order, so adjacency must follow the real module footprint. Physical pinout
// (pin = module pad, IOxx = ESP32 GPIO):
//
//   Row 1: 01 GND  02 3V3  03 EN  04 IO2  05 IO3  06 IO0  07 IO1  08 IO6
//          09 IO7  10 IO8(tiedHi) 11 IO9  12 IO10 13 IO13 14 IO14
//   Row 2: 15 IO28 16 IO5  17 IO4  18 IO27 19 NC  20 NC  21 IO23 22 NC
//          23 IO24 24 IO12 25 IO11 26 IO25 27 IO26 28 GND
//
// We only DRIVE pads that are safe to toggle: not the external-flash QIO bus
// (IO4/IO5/IO6/IO13/IO14/IO23), not I2C (IO0/IO1), not the ETH_CS/ADC/IRQ pins,
// not IO8 (externally tied HIGH), and not IO11/IO12 (ETX/ERX = the USB serial
// data port; driving them would break the console). A bridge to a fixed pad
// (GND/3V3/IO8) is still caught by the per-pin short-to-rail check. Here we test
// bridges between two adjacent pads that are BOTH safe to drive.
struct HwtAdjPair { uint8_t a; uint8_t b; };
static const HwtAdjPair hwt_adj_pairs[] = {
	// Left
	{ 9, 10 },   // pads 9-10: IO7  <-> IO8
	{ 10,13}, // pads 12-13: IO10 <-> IO13
	{ 13,14}, // pads 13-14: IO13 <-> IO14

	//Right:
	{ 5, 4 },   // pads 16-17: IO5  <-> IO4
	//{ 23, 24},   // pads 21-23: IO23 <-> IO24
	//{ 24,12}, // pads 23-24: IO24 <-> IO12
	//{ 12,11}, // pads 24-25: IO12 <-> IO11
	//{ 11,25}, // pads 25-26: IO11 <-> IO25
	{ 25, 26 }, // pads 26-27: IO25 <-> IO26
	// pads 23-25 (IO24 <-> IO12 <-> IO11) skipped: IO11/IO12 are the USB serial
	// ETX/ERX lines and are excluded from the test.
};
#define HWT_NADJ (sizeof(hwt_adj_pairs) / sizeof(hwt_adj_pairs[0]))

// Drive 'drv' to 'level' and verify the (undriven, weakly pulled) neighbour
// 'nbr' does NOT follow it. Returns true if no bridge is detected.
static bool hwt_drive_check(uint8_t drv, uint8_t nbr, uint8_t level) {
	// Pull the neighbour to the OPPOSITE rail so that a bridge (strong push-pull
	// driver) overrides the weak internal pull and is detected.
	pinMode(nbr, level ? INPUT_PULLDOWN : INPUT_PULLUP);
	pinMode(drv, OUTPUT);
	digitalWrite(drv, level ? HIGH : LOW);
	delayMicroseconds(300);
	int nbr_v = digitalRead(nbr);
	int drv_v = digitalRead(drv);
	// no bridge  => neighbour keeps its pulled level (opposite of drv)
	// driver must also actually reach its level (else it is shorted to a rail)
	bool ok = (nbr_v == (level ? LOW : HIGH)) && (drv_v == (level ? HIGH : LOW));
	pinMode(drv, INPUT);
	pinMode(nbr, INPUT);
	return ok;
}

// Test one adjacent pad pair for a solder bridge, driving from both sides.
static bool hwt_pair_no_short(uint8_t a, uint8_t b) {
	bool ok = true;
	ok &= hwt_drive_check(a, b, 1); // a HIGH, b must stay LOW
	ok &= hwt_drive_check(a, b, 0); // a LOW,  b must stay HIGH
	ok &= hwt_drive_check(b, a, 1); // b HIGH, a must stay LOW
	ok &= hwt_drive_check(b, a, 0); // b LOW,  a must stay HIGH
	return ok;
}

// ---------------------------------------------------------------------------
// Test runners
// ---------------------------------------------------------------------------
static void hwt_run_all(HwtContext &ctx) {
	ctx.count = 0;

	// 1) OLED display present on the I2C bus
	hwt_add(ctx, "Display OLED", hwt_i2c_present(LCD_I2CADDR), true);

	// 2) Main IO-expander present
	bool ioexp_present = (os.mainio != NULL) && hwt_i2c_present(os.mainio->address);
	hwt_add(ctx, "IO-Expander", ioexp_present, true);

	// 3) IO-expander I2C read/write (re-write config reg with its own value)
	bool ioexp_rw = false;
	if (ioexp_present) {
		os.mainio->i2c_write(NXP_CONFIG_REG, IO_CONFIG);
		uint16_t rb = os.mainio->i2c_read(NXP_CONFIG_REG);
		ioexp_rw = (rb == IO_CONFIG);
	}
	hwt_add(ctx, "IOexp I2C R/W", ioexp_rw, true);

	// 4) RTC (optional: DS1307 / PCF8563 / MCP7940)
	bool rtc_present = hwt_i2c_present(DS1307_ADDR) ||
	                   hwt_i2c_present(PCF8563_ADDR) ||
	                   hwt_i2c_present(MCP7940_ADDR);
	hwt_add(ctx, "RTC clock", rtc_present, false, rtc_present);

	// 5) EEPROM (optional, 24Cxx @ 0x50)
	bool eeprom_present = hwt_i2c_present(EEPROM_I2CADDR);
	hwt_add(ctx, "EEPROM", eeprom_present, false, eeprom_present);

	// External flash / LittleFS is intentionally NOT tested here: the self-test
	// runs BEFORE the filesystem is mounted (in begin()), so a faulty flash
	// cannot block it and no filesystem access is performed.

	// Buttons must read released (HIGH) at rest
	hwt_add(ctx, "Button B1", hwt_input_released(PIN_BUTTON_1), true);
	hwt_add(ctx, "Button B2", hwt_input_released(PIN_BUTTON_2), true);
	hwt_add(ctx, "Button B3", hwt_input_released(PIN_BUTTON_3), true);

	// 10-11) Sensor inputs pulled up at rest (open = HIGH)
	hwt_add(ctx, "Sensor1 in", hwt_input_released(PIN_SENSOR1), true);
	hwt_add(ctx, "Sensor2 in", hwt_input_released(PIN_SENSOR2), true);

	// 12-14) Free GPIOs: short-to-rail check
	hwt_add(ctx, "GPIO25", hwt_gpio_short_ok(PIN_FREE1), true);
	hwt_add(ctx, "GPIO26", hwt_gpio_short_ok(PIN_FREE2), true);
	hwt_add(ctx, "GPIO7",  hwt_gpio_short_ok(PIN_FREE3), true);

	// 15) Shift register / station latch drive: clock an all-OFF pattern
	//     (safe: no station is energised) and verify the expander still ACKs.
	bool sr_ok = false;
	if (ioexp_present) {
		os.mainio->shift_out(V2_PIN_SRLAT, V2_PIN_SRCLK, V2_PIN_SRDAT, 0x00);
		sr_ok = hwt_i2c_present(os.mainio->address);
	}
	hwt_add(ctx, "ShiftReg/Stn", sr_ok, true);

	// 16..) Adjacent-pad solder-bridge check: switching one pad must not switch
	//       a physically neighbouring pad (short between module contacts).
	for (uint8_t i = 0; i < HWT_NADJ && i < HWT_MAX_PAIRS; i++) {
		uint8_t a = hwt_adj_pairs[i].a;
		uint8_t b = hwt_adj_pairs[i].b;
		snprintf(ctx.pair_name[i], sizeof(ctx.pair_name[i]), "Brdg IO%u-%u",
		         (unsigned)a, (unsigned)b);
		hwt_add(ctx, ctx.pair_name[i], hwt_pair_no_short(a, b), true);
	}

	// restore pins touched by the tests to their normal operating mode
	pinModeExt(PIN_SENSOR1, INPUT_PULLUP);
	pinModeExt(PIN_SENSOR2, INPUT_PULLUP);
	pinMode(PIN_BUTTON_1, INPUT_PULLUP); // GPIO10 (B1) is a native pin
	if (PIN_RFTX != 255) {               // GPIO24 (RFTX) idles as a low output
		pinMode(PIN_RFTX, OUTPUT);
		digitalWrite(PIN_RFTX, LOW);
	}

}

// ---------------------------------------------------------------------------
// Result screens / interaction
// ---------------------------------------------------------------------------

// Collect the names of failed required tests into the given array.
static uint8_t hwt_collect_failures(HwtContext &ctx, const char *out[], uint8_t maxn) {
	uint8_t n = 0;
	for (uint8_t i = 0; i < ctx.count && n < maxn; i++) {
		if (ctx.results[i].required && !ctx.results[i].ok) {
			out[n++] = ctx.results[i].name;
		}
	}
	return n;
}

// Screen shown when all required tests passed. Any button resets the device.
static void hwt_screen_pass(HwtContext &ctx) {
	char line[17];
	uint8_t npass = 0;
	uint8_t ntest = 0;
	for (uint8_t i = 0; i < ctx.count; i++){ 
		if (ctx.results[i].ok) npass++;
		if (ctx.results[i].required || ctx.results[i].ok) {
			ntest++;
		}
	}
	snprintf(line, sizeof(line), "%u/%u checks OK", npass, ntest);

	os.lcd.clear();
	hwt_lcd_line(-1, "HW SELF-TEST");
	hwt_lcd_line(0, "ALL PASSED");
	hwt_lcd_line(1, line);
	hwt_lcd_line(2, "Press=Reset");

	// The self-test always exits via a device reset: any button press returns
	// (the caller then reboots).
	while (true) {
		unsigned char b = os.button_read(BUTTON_WAIT_NONE);
		if (b & BUTTON_FLAG_DOWN) return;
		delay(10);
	}
}

// Screen shown when at least one required test failed. Failed items auto-cycle;
// any button press resets the device.
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
		hwt_lcd_line(2, "Press=Reset");
	};

	render(page);

	// Failed items auto-cycle so they can all be read; any button press exits
	// the test via a device reset (the caller reboots).
	while (true) {
		unsigned char b = os.button_read(BUTTON_WAIT_NONE);
		if (b & BUTTON_FLAG_DOWN) return;
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

	// All test state lives on the stack for the duration of the test only,
	// so the self-test reserves no permanent (static) RAM.
	HwtContext ctx;
	ctx.count = 0;
	hwt_run_all(ctx);

	// count failed required tests
	const char *fails[HWT_MAX_TESTS];
	uint8_t nfail = hwt_collect_failures(ctx, fails, HWT_MAX_TESTS);

	DEBUG_PRINTF("[HWTEST] complete: %u check(s), %u failure(s)\n", ctx.count, nfail);

	if (nfail == 0) {
		hwt_screen_pass(ctx);
	} else {
		hwt_screen_fail(fails, nfail);
	}

	// The hardware self-test always exits via a device reset.
	os.lcd.clear();
	hwt_lcd_line(0, "Rebooting...");
	delay(400);
	DEBUG_PRINTLN(F("[HWTEST] ===== self-test finished, resetting ====="));
	os.reboot_dev(REBOOT_CAUSE_BUTTON);
}

#endif // ESP32C5
