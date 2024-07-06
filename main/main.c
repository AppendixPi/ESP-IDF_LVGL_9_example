#include <stdio.h>
#include <stdbool.h>
#include <unistd.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "st7789.h"
#include "driver/gptimer.h"
#include "utime.h"
#include "esp_timer.h"
#include "esp_log.h"

#define LV_TICK_PERIOD_MS 1

#define SENS_H	2*24
#define SENS_V  2*32

SemaphoreHandle_t xGuiSemaphore;

TaskHandle_t gui_task_Handle;

lv_obj_t * slider1;// = lv_slider_create(lv_screen_active());
lv_obj_t * arc;


static void lv_tick_task(void *arg) {
    (void) arg;

    lv_tick_inc(LV_TICK_PERIOD_MS);
}

static void gui_task(void *arg){

    lv_init();

    lv_display_t *display = lv_display_create(MY_DISP_HOR_RES, MY_DISP_VER_RES);

    static lv_color_t buf1[DISP_BUF_SIZE / 10];                        /*Declare a buffer for 1/10 screen size*/
    static lv_color_t buf2[DISP_BUF_SIZE / 10];                        /*Declare a buffer for 1/10 screen size*/
    lv_display_set_buffers(display, buf1, buf2, sizeof(buf1),LV_DISPLAY_RENDER_MODE_PARTIAL );  /*Initialize the display buffer.*/

    lv_display_set_flush_cb(display, st7789_flush);

    /* Create and start a periodic timer interrupt to call lv_tick_inc */
    const esp_timer_create_args_t periodic_timer_args = {
        .callback = &lv_tick_task,
        .name = "periodic_gui"
    };
    esp_timer_handle_t periodic_timer;
    ESP_ERROR_CHECK(esp_timer_create(&periodic_timer_args, &periodic_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(periodic_timer, LV_TICK_PERIOD_MS * 1000));

    /*Create a slider as example*/
    slider1 = lv_slider_create(lv_screen_active());

    /*Create an Arc*/
    arc = lv_arc_create(lv_screen_active());
    lv_arc_set_range(arc, 0, 100);
    lv_obj_set_size(arc, 200, 200); // Example size: 200x200 pixels
    lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE);  /*To not allow adjusting by click*/
    lv_obj_center(arc);

    lv_obj_t *label = lv_label_create(lv_scr_act());

    // Set the text of the label
    lv_label_set_text(label, "ESP-IDF + LVGL 9.1.0");

    // Align the label to the bottom center of the parent
    lv_obj_align(label, LV_ALIGN_BOTTOM_MID, 0, 0);

    static lv_style_t st;
    lv_style_init(&st);
    lv_style_set_text_font(&st, &lv_font_montserrat_28);

    // Apply the style to the label
    lv_obj_add_style(label, &st, 0);

    lv_obj_set_width(slider1, 150);                          /*Set the width*/
    lv_obj_center(slider1);                                  /*Align to the center of the parent (screen)*/

    /*Create battery indicator*/
    static lv_style_t style_indic;
    lv_style_init(&style_indic);
    lv_style_set_bg_opa(&style_indic, LV_OPA_COVER);
    lv_style_set_bg_color(&style_indic, lv_palette_main(LV_PALETTE_LIGHT_GREEN));

    bar = lv_bar_create(lv_scr_act());
    lv_obj_set_size(bar, 60, 15);
    lv_obj_align(bar,LV_ALIGN_TOP_RIGHT,-5,2);
    lv_bar_set_value(bar, 85, LV_ANIM_ON);
    lv_obj_add_style(bar, &style_indic, LV_PART_INDICATOR);

    label_bat = lv_label_create(lv_scr_act());
    lv_label_set_text(label_bat, "58%");
    lv_obj_align(label_bat, LV_ALIGN_TOP_RIGHT, -15, 2);
    lv_label_set_text(label_bat, "85%");


    while(1){
    	vTaskDelay(10);
    	if (pdTRUE == xSemaphoreTake(xGuiSemaphore, 50)) {
    		lv_task_handler();
    		xSemaphoreGive(xGuiSemaphore);
    	}

    }

}

void app_main(void)
{

    xGuiSemaphore = xSemaphoreCreateMutex();

    spi_display_init();
    st7789_init();

    xTaskCreatePinnedToCore(gui_task, "gui", 18*1024, NULL, 5, &gui_task_Handle,1 );
    vTaskDelay(1000);
	
    int slide_val = 0;
    while (1) {
    	if (pdTRUE == xSemaphoreTake(xGuiSemaphore, 50)) {
    		lv_slider_set_value(slider1, slide_val, LV_ANIM_ON);
    		lv_arc_set_value(arc, slide_val);
    		xSemaphoreGive(xGuiSemaphore);
    	}
    	slide_val ++;
    	if(slide_val > 100){
    		slide_val = 0;
    	}
    	vTaskDelay(10);
    }
}
