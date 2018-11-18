
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

// LIB: EasyQ
#include "easyq.h" 
#include "debug.h"


#define STATUS_INTERVAL 3000


LOCAL EasyQSession eq;
ETSTimer status_timer;
LOCAL bool remote_enabled;


void ICACHE_FLASH_ATTR
status_timer_func() {
	char str[50];
	float vdd = system_get_vdd33() / 1024.0;

	uint32_t userbin_addr = system_get_userbin_addr();
	uint8_t image = system_upgrade_userbin_check();
	os_sprintf(str, "Image: %s:0x%05X, VDD: %d.%03d Remote: %s", 
			(UPGRADE_FW_BIN1 == image)? "user1": "user2",
			userbin_addr,
			(int)vdd, 
			(int)(vdd*1000)%1000, remote_enabled? "ON": "OFF");
	easyq_push(&eq, STATUS_QUEUE, str);
}


void ICACHE_FLASH_ATTR
update_relay(uint32_t num, const char* msg) {
	bool on = strcmp(msg, "on") == 0;
	GPIO_OUTPUT_SET(GPIO_ID_PIN(num), !on);
}


void ICACHE_FLASH_ATTR
easyq_message_cb(void *arg, const char *queue, const char *msg, 
		uint16_t message_len) {
	//INFO("EASYQ: Message: %s From: %s\r\n", msg, queue);

	if (strcmp(queue, RELAY1_QUEUE) == 0) { 
		update_relay(RELAY1_NUM, msg);
	}
	else if (strcmp(queue, RELAY2_QUEUE) == 0) { 
		update_relay(RELAY2_NUM, msg);
	}	
	else if (strcmp(queue, FOTA_QUEUE) == 0 && msg[0] == 'S') {
		os_timer_disarm(&status_timer);
		char *server = (char *)(&msg[0]+1);
		char *colon = strrchr(server, ':');;
		uint8_t hostname_len = (uint8_t)(colon - server);
		uint16_t port = atoi(colon+1);
		colon[0] = 0;	
		
		INFO("INIT FOTA: %s %d\r\n", server, port);
		fota_init(server, hostname_len, port);

		// TODO: decide about delete easyq ?
		easyq_disconnect(&eq);
	}
}


void ICACHE_FLASH_ATTR
easyq_connect_cb(void *arg) {
	INFO("EASYQ: Connected to %s:%d\r\n", eq.hostname, eq.port);
	INFO("\r\n***** OTA ****\r\n");
    os_timer_disarm(&status_timer);
    os_timer_setfn(&status_timer, (os_timer_func_t *)status_timer_func, NULL);
    os_timer_arm(&status_timer, STATUS_INTERVAL, 1);
	
	const char * queues[] = {RELAY1_QUEUE, RELAY2_QUEUE, FOTA_QUEUE};
	easyq_pull_all(&eq, queues, 3);
}


void ICACHE_FLASH_ATTR
easyq_connection_error_cb(void *arg) {
	EasyQSession *e = (EasyQSession*) arg;
    os_timer_disarm(&status_timer);
	INFO("EASYQ: Connection error: %s:%d\r\n", e->hostname, e->port);
	INFO("EASYQ: Reconnecting to %s:%d\r\n", e->hostname, e->port);
}


void easyq_disconnect_cb(void *arg)
{
	EasyQSession *e = (EasyQSession*) arg;
    os_timer_disarm(&status_timer);
	INFO("EASYQ: Disconnected from %s:%d\r\n", e->hostname, e->port);
	easyq_delete(&eq);
	fota_start();
}


void wifi_connect_cb(uint8_t status) {
    if(status == STATION_GOT_IP) {
        easyq_connect(&eq);
    } else {
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

