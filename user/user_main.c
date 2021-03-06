
// Internal 
#include "partition.h"
#include "wifi.h"
#include "config.h"

// SDK
#include <ets_sys.h>
#include <osapi.h>
#include <gpio.h>
#include <mem.h>
#include <user_interface.h>
#include <driver/uart.h>
#include <upgrade.h>

// LIB: EasyQ
#include "easyq.h" 
#include "debug.h"


#define STATUS_INTERVAL		200	
#define VERSION				"2.2.1"

LOCAL EasyQSession eq;
ETSTimer status_timer;
LOCAL bool remote_enabled;
LOCAL bool relay1_remote_status;
LOCAL bool relay2_remote_status;
LOCAL bool led_status;
LOCAL uint32_t ticks;


enum blink_speed {
	BLINK_SLOW = 1,
	BLINK_MEDIUM = 2,
	BLINK_FAST = 4
};


void
fota_report_status(const char *q) {
	char str[50];
	float vdd = system_get_vdd33() / 1024.0;

	uint8_t image = system_upgrade_userbin_check();
	os_sprintf(str, "Image: %s Version: "VERSION" VDD: %d.%03d Remote: %s", 
			(UPGRADE_FW_BIN1 == image)? "FOTA": "APP",
			(int)vdd, 
			(int)(vdd*1000)%1000, 
			remote_enabled? "ON": "OFF");
	easyq_push(&eq, q, str);
}


void
update_led(bool on) {
	led_status = on;
	GPIO_OUTPUT_SET(GPIO_ID_PIN(LED_NUM), !on);
}


void
blink_led() {
	update_led(!remote_enabled || !led_status);
}


void ICACHE_FLASH_ATTR
status_timer_func() {
	ticks++;
	blink_led();
	if (eq.status == EASYQ_CONNECTED && ticks % 20 == 0) {
		fota_report_status(STATUS_QUEUE);
	}
}


void blink_speed(enum blink_speed n) {
    os_timer_disarm(&status_timer);
    os_timer_setfn(&status_timer, (os_timer_func_t *)status_timer_func, NULL);
    os_timer_arm(&status_timer, STATUS_INTERVAL/n, 1);
}


void ICACHE_FLASH_ATTR
update_relay(uint32_t num, bool on) {
	GPIO_OUTPUT_SET(GPIO_ID_PIN(num), !on);
}


void ICACHE_FLASH_ATTR
update_relays_by_remote_status() {
	update_relay(RELAY1_NUM, relay1_remote_status);	
	update_relay(RELAY2_NUM, relay2_remote_status);	
}


void ICACHE_FLASH_ATTR
update_relay_by_message(uint32_t num, const char* msg) {
	bool on = strcmp(msg, "on") == 0;
	switch (num) {
		case RELAY1_NUM:
			relay1_remote_status = on;
			break;
		case RELAY2_NUM:
			relay2_remote_status = on;
			break;
	}
	if (remote_enabled) {
		update_relays_by_remote_status();
	}
}


void ICACHE_FLASH_ATTR
easyq_message_cb(void *arg, const char *queue, const char *msg, 
		uint16_t message_len) {
	//INFO("EASYQ: Message: %s From: %s\r\n", msg, queue);

	if (strcmp(queue, RELAY1_QUEUE) == 0) { 
		update_relay_by_message(RELAY1_NUM, msg);
	}
	else if (strcmp(queue, RELAY2_QUEUE) == 0) { 
		update_relay_by_message(RELAY2_NUM, msg);
	}	
	else if (strcmp(queue, FOTA_QUEUE) == 0) {
		if (msg[0] == 'R') {
			os_timer_disarm(&status_timer);
			INFO("Rebooting to FOTA ROM\r\n");
			system_upgrade_flag_set(UPGRADE_FLAG_FINISH);
			system_upgrade_reboot();
		}
		else if (msg[0] == 'I') {
			fota_report_status(FOTA_STATUS_QUEUE);
		}

	}
}


void ICACHE_FLASH_ATTR
easyq_connect_cb(void *arg) {
	INFO("EASYQ: Connected to %s:%d\r\n", eq.hostname, eq.port);
	INFO("\r\n***** Smart Outlet "VERSION"****\r\n");
	blink_speed(BLINK_SLOW);
	const char * queues[] = {RELAY1_QUEUE, RELAY2_QUEUE, FOTA_QUEUE};
	easyq_pull_all(&eq, queues, 3);
}


void ICACHE_FLASH_ATTR
easyq_connection_error_cb(void *arg) {
	EasyQSession *e = (EasyQSession*) arg;
	INFO("EASYQ: Connection error: %s:%d\r\n", e->hostname, e->port);
	INFO("EASYQ: Reconnecting to %s:%d\r\n", e->hostname, e->port);
}


void easyq_disconnect_cb(void *arg)
{
	EasyQSession *e = (EasyQSession*) arg;
	INFO("EASYQ: Disconnected from %s:%d\r\n", e->hostname, e->port);
	easyq_delete(&eq);
}


void wifi_connect_cb(uint8_t status) {
    if(status == STATION_GOT_IP) {
		blink_speed(BLINK_MEDIUM);
        easyq_connect(&eq);
    } else {
		blink_speed(BLINK_FAST);
        easyq_disconnect(&eq);
    }
}



void ICACHE_FLASH_ATTR
sw2_interrupt() {
	uint16_t status;
	ETS_GPIO_INTR_DISABLE();
	
	// Clear the interrupt
	status = GPIO_REG_READ(GPIO_STATUS_ADDRESS);
	GPIO_REG_WRITE(GPIO_STATUS_W1TC_ADDRESS, status);

	bool enabled = !GPIO_INPUT_GET(GPIO_ID_PIN(SW2_NUM));
	if (enabled ^ remote_enabled) {
		remote_enabled = enabled;
		if (eq.status >= EASYQ_SENDING) {
			char str[16];
			os_sprintf(str, "Remote: %d", enabled);
			easyq_push(&eq, STATUS_QUEUE, str);
		}
		INFO("REMOTE: %s\r\n", enabled? "ON": "OFF"); 
	}
	os_delay_us(50000);
	if (remote_enabled) {
		update_relays_by_remote_status();
	}
	else {
		update_relay(RELAY1_NUM, true);	
		update_relay(RELAY2_NUM, true);	
	}
	ETS_GPIO_INTR_ENABLE();
}


void user_init(void) {
    uart_init(BIT_RATE_115200, BIT_RATE_115200);
    os_delay_us(60000);

	// SW2
	PIN_FUNC_SELECT(SW2_MUX, SW2_FUNC);
	PIN_PULLUP_EN(SW2_MUX);
	GPIO_DIS_OUTPUT(GPIO_ID_PIN(SW2_NUM));

	// Relays 
	PIN_FUNC_SELECT(RELAY1_MUX, RELAY1_FUNC);
	PIN_FUNC_SELECT(RELAY2_MUX, RELAY2_FUNC);
	GPIO_OUTPUT_SET(GPIO_ID_PIN(RELAY1_NUM), 1);
	GPIO_OUTPUT_SET(GPIO_ID_PIN(RELAY2_NUM), 1);
	relay1_remote_status = false;
	relay2_remote_status = false;

	// LED
	PIN_FUNC_SELECT(LED_MUX, LED_FUNC);
	PIN_PULLUP_DIS(LED_MUX);
	GPIO_OUTPUT_SET(GPIO_ID_PIN(LED_NUM), 1);

	EasyQError err = easyq_init(&eq, EASYQ_HOSTNAME, EASYQ_PORT, EASYQ_LOGIN);
	if (err != EASYQ_OK) {
		ERROR("EASYQ INIT ERROR: %d\r\n", err);
		return;
	}
	eq.onconnect = easyq_connect_cb;
	eq.ondisconnect = easyq_disconnect_cb;
	eq.onconnectionerror = easyq_connection_error_cb;
	eq.onmessage = easyq_message_cb;

	ETS_GPIO_INTR_DISABLE();
	ETS_GPIO_INTR_ATTACH(sw2_interrupt, NULL);
	gpio_pin_intr_state_set(GPIO_ID_PIN(SW2_NUM), GPIO_PIN_INTR_ANYEDGE);
	ETS_GPIO_INTR_ENABLE();
	sw2_interrupt();
	
	blink_speed(BLINK_FAST);
    WIFI_Connect(WIFI_SSID, WIFI_PSK, wifi_connect_cb);
    INFO("System started ...\r\n");
}


void ICACHE_FLASH_ATTR user_pre_init(void)
{
    if(!system_partition_table_regist(at_partition_table, 
				sizeof(at_partition_table)/sizeof(at_partition_table[0]),
				SPI_FLASH_SIZE_MAP)) {
		FATAL("system_partition_table_regist fail\r\n");
		while(1);
	}
}

