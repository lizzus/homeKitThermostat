/*
 * Copyright 2018 David B Brown (@maccoylton)
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * 
 * http://www.apache.org/licenses/LICENSE-2.0
 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * 
 * EPS8266 HomeKit WIFI Thermostat 
 *
 * Uses a DHT22 (temperature sensor)
 *
 *
 */

#define DEVICE_MANUFACTURER "Kristian Dimitrov"
#define DEVICE_NAME "Wifi-Thermostat"
#define DEVICE_MODEL "Basic"
#define DEVICE_SERIAL "12345678"
#define FW_VERSION "1.0.1"

#include <stdio.h>
#include <espressif/esp_wifi.h>
#include <espressif/esp_sta.h>
#include <espressif/esp_system.h>
#include <esp/uart.h>
#include <esp8266.h>
#include <etstimer.h>
#include <esplibs/libmain.h>
#include <FreeRTOS.h>
#include <task.h>
#include <ssd1306/ssd1306.h>
#include <homekit/homekit.h>
#include <homekit/characteristics.h>
#include <wifi_config.h>
#include <dht/dht.h>

#include "wifi_thermostat.h"
//#include "button.h"

#define TEMPERATURE_SENSOR_PIN 4
#define TEMPERATURE_POLL_PERIOD 10000
#define BUTTON_UP_GPIO 12
#define BUTTON_DOWN_GPIO 13
#define BUTTON_MODE_GPIO 0
#define RELAY_GPIO 16
#define RECIVE_GPIO 15
const int LED_GPIO = 2;


static ETSTimer thermostat_timer;
static TaskHandle_t  xHandle ;
bool fire = false, could = false;




// add this section to make your device OTA capable
// create the extra characteristic &ota_trigger, at the end of the primary service (before the NULL)
// it can be used in Eve, which will show it, where Home does not
// and apply the four other parameters in the accessories_information section

void led_write(bool on)
{
    gpio_write(LED_GPIO, on ? 0 : 1);
}

void relay_write(bool on)
{
    gpio_write(RELAY_GPIO, on ? 1 : 0);
}

/// I2C

/* Remove this line if your display connected by SPI */
#define I2C_CONNECTION

#ifdef I2C_CONNECTION
#include <i2c/i2c.h>
#endif

#include "fonts/fonts.h"

/* Change this according to you schematics and display size */
#define DISPLAY_WIDTH 128
#define DISPLAY_HEIGHT 64

#ifdef I2C_CONNECTION
#define PROTOCOL SSD1306_PROTO_I2C
#define ADDR SSD1306_I2C_ADDR_0
#define I2C_BUS 0
#define SCL_PIN 14
#define SDA_PIN 5
#else
#define PROTOCOL SSD1306_PROTO_SPI4
#define CS_PIN 5
#define DC_PIN 4
#endif

#define DEFAULT_FONT FONT_FACE_TERMINUS_16X32_ISO8859_1

/* Declare device descriptor */
static const ssd1306_t dev = {
    .protocol = PROTOCOL,
#ifdef I2C_CONNECTION
    .i2c_dev.bus = I2C_BUS,
    .i2c_dev.addr = ADDR,
#else
    .cs_pin = CS_PIN,
    .dc_pin = DC_PIN,
#endif
    .width = DISPLAY_WIDTH,
    .height = DISPLAY_HEIGHT};

/* Local frame buffer */
static uint8_t buffer[DISPLAY_WIDTH * DISPLAY_HEIGHT / 8];

#define SECOND (1000 / portTICK_PERIOD_MS)

// I2C

#include "ota-api.h"
homekit_characteristic_t ota_trigger = API_OTA_TRIGGER;
homekit_characteristic_t name = HOMEKIT_CHARACTERISTIC_(NAME, DEVICE_NAME);
homekit_characteristic_t manufacturer = HOMEKIT_CHARACTERISTIC_(MANUFACTURER, DEVICE_MANUFACTURER);
homekit_characteristic_t serial = HOMEKIT_CHARACTERISTIC_(SERIAL_NUMBER, DEVICE_SERIAL);
homekit_characteristic_t model = HOMEKIT_CHARACTERISTIC_(MODEL, DEVICE_MODEL);
homekit_characteristic_t revision = HOMEKIT_CHARACTERISTIC_(FIRMWARE_REVISION, FW_VERSION);

void thermostat_identify_task(void *_args)
{
    for (int i = 0; i < 3; i++)
    {
        for (int i = 0; i < 3; i++)
        {
            led_write(true);
            vTaskDelay(150 / portTICK_PERIOD_MS);
            led_write(false);
            vTaskDelay(150 / portTICK_PERIOD_MS);
        }

        vTaskDelay(350 / portTICK_PERIOD_MS);
    }
    led_write(false);

    vTaskDelete(NULL);
}


void thermostat_identify(homekit_value_t _value)
{
    printf("Thermostat identify\n");
    xTaskCreate(thermostat_identify_task, "Thermostat identify", 128, NULL, 2, NULL);
}

void process_setting_update();
void on_update(homekit_characteristic_t *ch, homekit_value_t value, void *context)
{
    process_setting_update();
}

homekit_characteristic_t current_temperature = HOMEKIT_CHARACTERISTIC_(CURRENT_TEMPERATURE, 0);
homekit_characteristic_t target_temperature = HOMEKIT_CHARACTERISTIC_(TARGET_TEMPERATURE, 22, .callback = HOMEKIT_CHARACTERISTIC_CALLBACK(on_update));
homekit_characteristic_t units = HOMEKIT_CHARACTERISTIC_(TEMPERATURE_DISPLAY_UNITS, 0);
homekit_characteristic_t current_state = HOMEKIT_CHARACTERISTIC_(CURRENT_HEATING_COOLING_STATE, 0);
homekit_characteristic_t target_state = HOMEKIT_CHARACTERISTIC_(TARGET_HEATING_COOLING_STATE, 1, .callback = HOMEKIT_CHARACTERISTIC_CALLBACK(on_update));
homekit_characteristic_t cooling_threshold = HOMEKIT_CHARACTERISTIC_(COOLING_THRESHOLD_TEMPERATURE, 25, .callback = HOMEKIT_CHARACTERISTIC_CALLBACK(on_update));
homekit_characteristic_t heating_threshold = HOMEKIT_CHARACTERISTIC_(HEATING_THRESHOLD_TEMPERATURE, 15, .callback = HOMEKIT_CHARACTERISTIC_CALLBACK(on_update));
homekit_characteristic_t current_humidity = HOMEKIT_CHARACTERISTIC_(CURRENT_RELATIVE_HUMIDITY, 0);

void wifi_led()
{
    led_write(true);
    vTaskDelay(4000 / portTICK_PERIOD_MS);
    led_write(false);
    vTaskDelete(NULL);
    
    
}
void wifi_init_led()
{
    xTaskCreate(wifi_led, "Reset configuration", 256, NULL, 2, NULL);
}

void reset_configuration_task()
{
 sdk_os_timer_disarm(&thermostat_timer);
    //Flash the LED first before we start the reset
    for (int i = 0; i < 3; i++)
    {
        for (int i = 0; i < 5; i++)
        {
            led_write(true);
            vTaskDelay(150 / portTICK_PERIOD_MS);
            led_write(false);
            vTaskDelay(150 / portTICK_PERIOD_MS);
        }
        vTaskDelay(350 / portTICK_PERIOD_MS);
    }

    printf("Resetting Wifi Config\n");

    wifi_config_reset();

    vTaskDelay(1000 / portTICK_PERIOD_MS);

    printf("Resetting HomeKit Config\n");

    homekit_server_reset();

    vTaskDelay(1000 / portTICK_PERIOD_MS);

    printf("Restarting\n");

    sdk_system_restart();

    vTaskDelete(NULL);
}

void reset_configuration()
{
    printf("Resetting configuration\n");
    xTaskCreate(reset_configuration_task, "Reset configuration", 256, NULL, 2, NULL);
}

// LCD ssd1306

static void ssd1306_task(void *pvParameters)
{
    char target_temp_string[20];
    char mode_string[20];
    char temperature_string[20];
    char humidity_string[20];

    int xT = 5, yT = 2, xM = 83, yM = 37, xH = 90, yH = 2;

    vTaskDelay(SECOND);
    
    
    ssd1306_set_whole_display_lighting(&dev, false);


    if (ssd1306_load_xbm(&dev, homekit_logo, buffer))
        goto error_loop;

    if (ssd1306_load_frame_buffer(&dev, buffer))
        goto error_loop;

    vTaskDelay(SECOND * 5);

    ssd1306_clear_screen(&dev);
    
     

    while (1)
    {
    
   
        uint8_t hum = (uint8_t)current_humidity.value.float_value;
        sprintf(temperature_string, "%g", (float)current_temperature.value.float_value);
        sprintf(humidity_string, "%i", hum /*(float)current_humidity.value.float_value */);
        sprintf(target_temp_string, "%g", (float)target_temperature.value.float_value);

        uint8_t i = 0;

        for (i = 0; temperature_string[i] != '\0'; i++)
            ;

        if (i > 2)
        {
            i = 70;
        }
        else
        {
            i = 42;
        }

        switch ((int)target_state.value.int_value)
        {
        
        case 0:
            sprintf(mode_string, "OFF ");
            break;
        case 1:
            sprintf(mode_string, "HEAT");
            break;
        case 2:
            sprintf(mode_string, "COOL");
            break;
        case 3:
            sprintf(mode_string, "AUTO");
            break;
        default:
            sprintf(mode_string, "?   ");
        }

        if (ssd1306_fill_rectangle(&dev, buffer, 0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT, OLED_COLOR_BLACK))
        {
            printf("Error printing rectangle\bn");
        }

        if (ssd1306_draw_string(&dev, buffer, font_builtin_fonts[FONT_FACE_TERMINUS_BOLD_11X22_ISO8859_1], 2, 37, target_temp_string, OLED_COLOR_WHITE, OLED_COLOR_BLACK) < 1)
        {
            printf("Error printing target temp\n");
        }

        if (ssd1306_draw_string(&dev, buffer, font_builtin_fonts[FONT_FACE_TERMINUS_BOLD_11X22_ISO8859_1], xM, yM, mode_string, OLED_COLOR_WHITE, OLED_COLOR_BLACK) < 1)
        {
            printf("Error printing mode\n");
        }

        if (ssd1306_draw_string(&dev, buffer, font_builtin_fonts[FONT_FACE_TERMINUS_BOLD_14X28_ISO8859_1], xT, yT, temperature_string, OLED_COLOR_WHITE, OLED_COLOR_BLACK) < 1)
        {
            printf("Error printing temperature\n");
        }
        if (ssd1306_draw_string(&dev, buffer, font_builtin_fonts[FONT_FACE_TERMINUS_BOLD_11X22_ISO8859_1], xH, yH, humidity_string, OLED_COLOR_WHITE, OLED_COLOR_BLACK) < 1)
        {
            printf("Error printing humidity\n");
        }

       ssd1306_draw_circle(&dev, buffer, i, 7, 3, OLED_COLOR_WHITE);
     //   {
     //       printf("Error printing celsius temp\n");
      //  }

        if (ssd1306_draw_string(&dev, buffer, font_builtin_fonts[FONT_FACE_TERMINUS_BOLD_11X22_ISO8859_1], 117, 2, "%", OLED_COLOR_WHITE, OLED_COLOR_BLACK) < 1)
        {
            printf("Error printing mode\n");
        }

		if(fire){
		ssd1306_load_xbm(&dev, thermostat_xbm, buffer);
		}

		if(could){
		ssd1306_load_xbm(&dev, thermostat_could_xbm, buffer);
		}
		
        if (ssd1306_load_frame_buffer(&dev, buffer))
            goto error_loop;		

      //  vTaskDelay(SECOND / 2);
      vTaskSuspend(xHandle);
    }

error_loop:
    printf("%s: error while loading framebuffer into SSD1306\n", __func__);
    for (;;)
    {
        vTaskDelay(2 * SECOND);
        printf("%s: error loop\n", __FUNCTION__);
    }
}

void screen_init(void)
{
    //uncomment to test with CPU overclocked
    //sdk_system_update_cpu_freq(160);

    printf("Screen Init SDK version:%s\n", sdk_system_get_sdk_version());

#ifdef I2C_CONNECTION
    i2c_init(I2C_BUS, SCL_PIN, SDA_PIN, I2C_FREQ_400K);
#endif

    while (ssd1306_init(&dev) != 0)
    {
        printf("%s: failed to init SSD1306 lcd\n", __func__);
        vTaskDelay(SECOND);
    }
    ssd1306_set_whole_display_lighting(&dev, true);

    xTaskCreate(ssd1306_task, "ssd1306_task", 512, NULL, 2, &xHandle);
}

// LCD ssd1306



//Button

void button_up_callback(uint8_t event)
{
    switch (event)
    {
    case 1:
        printf("Button UP\n");
       if((target_temperature.value.float_value + 0.5) <= 38){
         target_temperature.value.float_value += 0.5;
         homekit_characteristic_notify(&target_temperature, target_temperature.value);
        
        }
        break;
    case 2:
        printf("Button UP\n");
      if((target_temperature.value.float_value + 1) <= 38){
         target_temperature.value.float_value += 1;
         homekit_characteristic_notify(&target_temperature, target_temperature.value);

        } 
        break;
    default:
        printf("Unknown button event: %d\n", event);
        
    }
    process_setting_update();
}

void button_down_callback(uint8_t event)
{
    switch (event)
    {    
    case 1:
        printf("Button DOWN\n");
   if((target_temperature.value.float_value - 0.5) >= 10){
         target_temperature.value.float_value -= 0.5;
         homekit_characteristic_notify(&target_temperature, target_temperature.value);

        }     
        
        break;
    case 2:
        printf("Button DOWN\n");
        if((target_temperature.value.float_value - 1) >= 10){
         target_temperature.value.float_value -= 1;
         homekit_characteristic_notify(&target_temperature, target_temperature.value);

        }  
        
        break;
    default:
        printf("Unknown button event: %d\n", event);
        
    }
    process_setting_update();
}

void button_mode_callback(uint8_t event)
{
uint8_t state = target_state.value.int_value + 1;
    switch (event)
    {
    case 1:
        
    switch (state)
        {
        case 1:
            //heat
            state = 1;
            break;
            //cool
        case 2:
            state = 2;

            break;
            //auto
        case 3:
            state = 3;
            break;

        default:
            //off

            state = 0;
            break;
        }
        target_state.value = HOMEKIT_UINT8(state);
        homekit_characteristic_notify(&target_state, target_state.value);
       process_setting_update();
        break;
    case 2:
        reset_configuration();
        break;
    default:
        printf("Unknown button event: %d\n", event);
    }
}

//Button

void buttonUp(void *pvParameters)
{
    printf("Polling for button press on gpio %d...\r\n", BUTTON_UP_GPIO);
   bool prest;
    bool press_time;
    uint32_t now, last;
    
    while(1) {
    press_time = true;
    prest = true;
    
        while(prest)
        {
        	if(gpio_read(BUTTON_UP_GPIO) == 0){
        		if(press_time){
        			press_time = false;
        		    now = xTaskGetTickCountFromISR()*portTICK_PERIOD_MS;
        		}
        	}else{
        		if(press_time == false){
        			prest = false;
        		}
        	}
        	
           vTaskDelay(50 / portTICK_PERIOD_MS);
        }
        last = xTaskGetTickCountFromISR()*portTICK_PERIOD_MS;
        last -= now;
        
        printf("Polled for button press at %dms\r\n", last);
        
        if(last > 50){
     	    if(last > 500){
               button_up_callback(2);
     	    
        }else{
        
      		button_up_callback(1);
        }
    }
       vTaskDelay(200 / portTICK_PERIOD_MS);
    
 }  
       
    
}
void buttonDown(void *pvParameters)
{
    printf("Polling for button press on gpio %d...\r\n", BUTTON_DOWN_GPIO);
   bool prest;
    bool press_time;
    uint32_t now, last;
    
    while(1) {
    press_time = true;
    prest = true;
    
        while(prest)
        {
        	if(gpio_read(BUTTON_DOWN_GPIO) == 0){
        		if(press_time){
        			press_time = false;
        		    now = xTaskGetTickCountFromISR()*portTICK_PERIOD_MS;
        		}
        	}else{
        		if(press_time == false){
        			prest = false;
        		}
        	}
        	
            vTaskDelay(50 / portTICK_PERIOD_MS);
        }
        last = xTaskGetTickCountFromISR()*portTICK_PERIOD_MS;
        last -= now;
        
        printf("Polled for button press at %dms\r\n", last);
        
        if(last > 50){
     	    if(last > 500){
               button_down_callback(2);
     	    
        }else{
        
      		button_down_callback(1);
        }
     }
       vTaskDelay(200 / portTICK_PERIOD_MS);
    
	}  
       
    
}
void buttonMode(void *pvParameters)
{
    printf("Polling for button press on gpio %d...\r\n", BUTTON_MODE_GPIO);
   bool prest;
    bool press_time;
    uint32_t now, last;
    
    while(1) {
    press_time = true;
    prest = true;
    
        while(prest)
        {
        	if(gpio_read(BUTTON_MODE_GPIO) == 0){
        		if(press_time){
        			press_time = false;
        		    now = xTaskGetTickCountFromISR()*portTICK_PERIOD_MS;
        		}
        	}else{
        		if(press_time == false){
        			prest = false;
        		}
        	}
        	
        	if(gpio_read(RECIVE_GPIO) == 1){
        		fire = true;
        		vTaskResume(xHandle);
        	}else{
        		fire = false;
        		vTaskResume(xHandle);
        	}
        	
           vTaskDelay(50 / portTICK_PERIOD_MS);
        }
        last = xTaskGetTickCountFromISR()*portTICK_PERIOD_MS;
        last -= now;
        
        printf("Polled for button press at %dms\r\n", last);
        
        if(last > 600){
     	    if(last > 10000){
               button_mode_callback(2);
     	    
        }else{
        
      		button_mode_callback(1);
        }
    }
       vTaskDelay(200 / portTICK_PERIOD_MS);
    
 }  
       
    
}

void process_setting_update()
{

    uint8_t state = target_state.value.int_value;
    if ((state == 1 && current_temperature.value.float_value < target_temperature.value.float_value) ||
        (state == 3 && current_temperature.value.float_value < heating_threshold.value.float_value))
    {
        if (current_state.value.int_value != 1)
        {
            current_state.value = HOMEKIT_UINT8(1);
            homekit_characteristic_notify(&current_state, current_state.value);
			
            relay_write(true);
           // fire = true;
            could = false;           

        }
    }
    else if ((state == 2 && current_temperature.value.float_value > target_temperature.value.float_value) ||
             (state == 3 && current_temperature.value.float_value > cooling_threshold.value.float_value))
    {
        if (current_state.value.int_value != 2)
        {
            current_state.value = HOMEKIT_UINT8(2);
            homekit_characteristic_notify(&current_state, current_state.value);

            relay_write(false);
         //   fire = false;
            could = true;
        }
    }
    else
    {
        if (current_state.value.int_value != 0)
        {
            current_state.value = HOMEKIT_UINT8(0);
            homekit_characteristic_notify(&current_state, current_state.value);

            relay_write(false);
          //  fire = false;
            could = false;
        }
    }
    vTaskResume(xHandle);
}

void temperature_sensor_task()
{


    float humidity_value, temperature_value;
 //   while (1)
  //  {
        bool success = dht_read_float_data( DHT_TYPE_DHT22, TEMPERATURE_SENSOR_PIN, &humidity_value, &temperature_value);

        if (success)
        {
            printf("Got readings: temperature %g, humidity %g\n", temperature_value, humidity_value);
            current_temperature.value = HOMEKIT_FLOAT(temperature_value);
            current_humidity.value = HOMEKIT_FLOAT(humidity_value);

            homekit_characteristic_notify(&current_temperature, current_temperature.value);
            homekit_characteristic_notify(&current_humidity, current_humidity.value);

            process_setting_update();
        }
        else
        {
            printf("Couldnt read data from sensor\n");
        }

   //     homekit_characteristic_notify(&current_state, current_state.value);
    //    vTaskDelay(TEMPERATURE_POLL_PERIOD / portTICK_PERIOD_MS);
   // }
   
 //  vTaskResume(xHandle);


}

void thermostat_init()
{

    gpio_enable(RECIVE_GPIO, GPIO_INPUT);
    gpio_set_pullup(RECIVE_GPIO, false, false);

    gpio_enable(BUTTON_UP_GPIO, GPIO_INPUT);
 //     gpio_set_interrupt(BUTTON_UP_GPIO, GPIO_INTTYPE_EDGE_ANY, NULL);
    gpio_set_pullup(BUTTON_UP_GPIO, false, false);
      
    gpio_enable(BUTTON_DOWN_GPIO, GPIO_INPUT);
 //   gpio_set_interrupt(BUTTON_DOWN_GPIO, GPIO_INTTYPE_EDGE_ANY, NULL);
    gpio_set_pullup(BUTTON_DOWN_GPIO, false, false);
    
    gpio_enable(BUTTON_MODE_GPIO, GPIO_INPUT);
//    gpio_set_interrupt(BUTTON_MODE_GPIO, GPIO_INTTYPE_EDGE_ANY, NULL);
    gpio_set_pullup(BUTTON_MODE_GPIO, false, false);

    gpio_enable(LED_GPIO, GPIO_OUTPUT);
    led_write(false);

    gpio_enable(RELAY_GPIO, GPIO_OUTPUT);
    relay_write(false);
 
    gpio_set_pullup(TEMPERATURE_SENSOR_PIN, false, false);
 
 	xTaskCreate(buttonUp, "buttonUp", 256, NULL, 2, NULL);
	xTaskCreate(buttonDown, "buttonDown", 256, NULL, 2, NULL);
	xTaskCreate(buttonMode, "buttonMode", 256, NULL, 2, NULL);

  sdk_os_timer_setfn(&thermostat_timer, temperature_sensor_task, NULL);
    sdk_os_timer_arm(&thermostat_timer, TEMPERATURE_POLL_PERIOD, 1);
}




homekit_accessory_t *accessories[] = {
    HOMEKIT_ACCESSORY(.id=1, .category=homekit_accessory_category_thermostat, .services=(homekit_service_t*[]) {
        HOMEKIT_SERVICE(ACCESSORY_INFORMATION, .characteristics=(homekit_characteristic_t*[]) {
            &name,
            &manufacturer,
            &serial,
            &model,
            &revision,
            HOMEKIT_CHARACTERISTIC(IDENTIFY, thermostat_identify),
            NULL
        }),
        HOMEKIT_SERVICE(THERMOSTAT, .primary=true, .characteristics=(homekit_characteristic_t*[]) {
            HOMEKIT_CHARACTERISTIC(NAME, "Thermostat"),
            &current_temperature,
            &target_temperature,
            &current_state,
            &target_state,
            &cooling_threshold,
            &heating_threshold,
            &units,
            &current_humidity,
            &ota_trigger,
            NULL
        }),
        NULL
    }),
    NULL
};



void create_accessory_name() {

    int serialLength = snprintf(NULL, 0, "%d", sdk_system_get_chip_id());

    char *serialNumberValue = malloc(serialLength + 1);

    snprintf(serialNumberValue, serialLength + 1, "%d", sdk_system_get_chip_id());
    
    int name_len = snprintf(NULL, 0, "%s-%s-%s",
				DEVICE_NAME,
				DEVICE_MODEL,
				serialNumberValue);

    if (name_len > 63) {
        name_len = 63;
    }

    char *name_value = malloc(name_len + 1);

    snprintf(name_value, name_len + 1, "%s-%s-%s",
		 DEVICE_NAME, DEVICE_MODEL, serialNumberValue);

   
    name.value = HOMEKIT_STRING(name_value);
    serial.value = name.value;
}


homekit_server_config_t config = {
    .accessories = accessories,
    .password = "111-11-111"
};

void on_wifi_ready() {
	wifi_init_led();
	
	create_accessory_name(); 
	
    homekit_server_init(&config);
    
}

void user_init(void) {

    uart_set_baud(0, 115200);

    wifi_config_init("HomeKit-Thermostat", NULL, on_wifi_ready);

    thermostat_init();

//    printf ("Calling screen init\n");
 //   printf ("fonts count %i\n", font_builtin_fonts_count);
    screen_init();
 //   printf ("Screen init called\n");
    int c_hash=ota_read_sysparam(&manufacturer.value.string_value,&serial.value.string_value,
                                      &model.value.string_value,&revision.value.string_value);
    if (c_hash==0) c_hash=1;
        config.accessories[0]->config_number=c_hash;

  //  homekit_server_init(&config);
}
