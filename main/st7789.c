/**
 * @file st7789.c
 *
 * Mostly taken from lbthomsen/esp-idf-littlevgl github.
 */


#include "sdkconfig.h"

#include "esp_log.h"

#include "st7789.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "rom/gpio.h"

/*********************
 *      DEFINES
 *********************/
#define TAG "st7789"
/**********************
 *      TYPEDEFS
 **********************/

/*The LCD needs a bunch of command/argument values to be initialized. They are stored in this struct. */
typedef struct {
    uint8_t cmd;
    uint8_t data[16];
    uint8_t databytes; //No of data in data; bit 7 = delay after set; 0xFF = end of cmds.
} lcd_init_cmd_t;

/**********************
 *  STATIC PROTOTYPES
 **********************/
static void st7789_set_orientation(uint8_t orientation);

static void st7789_send_color(void *data, size_t length);

static void disp_wait_for_pending_transactions();
void disp_spi_transaction(const uint8_t *data, size_t length,
    disp_spi_send_flag_t flags, uint8_t *out,
    uint64_t addr, uint8_t dummy_bits);
static void disp_spi_send_colors(uint8_t *data, size_t length);
static void disp_spi_send_data(uint8_t *data, size_t length);

/**********************
 *  STATIC VARIABLES
 **********************/
static QueueHandle_t TransactionPool = NULL;
static spi_device_handle_t spi;
static spi_host_device_t spi_host;

/**********************
 *      MACROS
 **********************/

/**********************
 *   GLOBAL FUNCTIONS
 **********************/
void st7789_init(void)
{
    lcd_init_cmd_t st7789_init_cmds[] = {
        {0xCF, {0x00, 0x83, 0X30}, 3},
        {0xED, {0x64, 0x03, 0X12, 0X81}, 4},
        {ST7789_PWCTRL2, {0x85, 0x01, 0x79}, 3},
        {0xCB, {0x39, 0x2C, 0x00, 0x34, 0x02}, 5},
        {0xF7, {0x20}, 1},
        {0xEA, {0x00, 0x00}, 2},
        {ST7789_LCMCTRL, {0x26}, 1},
        {ST7789_IDSET, {0x11}, 1},
        {ST7789_VCMOFSET, {0x35, 0x3E}, 2},
        {ST7789_CABCCTRL, {0xBE}, 1},
        {ST7789_MADCTL, {0x00}, 1}, // Set to 0x28 if your display is flipped
        {ST7789_COLMOD, {0x55}, 1},

#if ST7789_INVERT_COLORS == 1
		{ST7789_INVON, {0}, 0}, // set inverted mode
#else
 		{ST7789_INVOFF, {0}, 0}, // set non-inverted mode
#endif

        {ST7789_RGBCTRL, {0x00, 0x1B}, 2},
        {0xF2, {0x08}, 1},
        {ST7789_GAMSET, {0x01}, 1},
        {ST7789_PVGAMCTRL, {0xD0, 0x00, 0x02, 0x07, 0x0A, 0x28, 0x32, 0x44, 0x42, 0x06, 0x0E, 0x12, 0x14, 0x17}, 14},
        {ST7789_NVGAMCTRL, {0xD0, 0x00, 0x02, 0x07, 0x0A, 0x28, 0x31, 0x54, 0x47, 0x0E, 0x1C, 0x17, 0x1B, 0x1E}, 14},
        {ST7789_CASET, {0x00, 0x00, 0x00, 0xEF}, 4},
        {ST7789_RASET, {0x00, 0x00, 0x01, 0x3f}, 4},
        {ST7789_RAMWR, {0}, 0},
        {ST7789_GCTRL, {0x07}, 1},
        {0xB6, {0x0A, 0x82, 0x27, 0x00}, 4},
        {ST7789_SLPOUT, {0}, 0x80},
        {ST7789_DISPON, {0}, 0x80},
        {0, {0}, 0xff},
    };

    //Initialize non-SPI GPIOs
    gpio_pad_select_gpio(ST7789_DC);
    gpio_set_direction(ST7789_DC, GPIO_MODE_OUTPUT);

#if !defined(ST7789_SOFT_RST)
    gpio_pad_select_gpio(ST7789_RST);
    gpio_set_direction(ST7789_RST, GPIO_MODE_OUTPUT);
#endif

    //Reset the display
#if !defined(ST7789_SOFT_RST)
    gpio_set_level(ST7789_RST, 0);
    vTaskDelay(100 / portTICK_RATE_MS);
    gpio_set_level(ST7789_RST, 1);
    vTaskDelay(100 / portTICK_RATE_MS);
#else
    st7789_send_cmd(ST7789_SWRESET);
#endif

    printf("ST7789 initialization.\n");

    //Send all the commands
    uint16_t cmd = 0;
    while (st7789_init_cmds[cmd].databytes!=0xff) {
        st7789_send_cmd(st7789_init_cmds[cmd].cmd);
        st7789_send_data(st7789_init_cmds[cmd].data, st7789_init_cmds[cmd].databytes&0x1F);
        if (st7789_init_cmds[cmd].databytes & 0x80) {
                vTaskDelay(100 / portTICK_PERIOD_MS);
        }
        cmd++;
    }

    st7789_set_orientation(CONFIG_LV_DISPLAY_ORIENTATION);


}

void spi_display_init(void){
	esp_err_t ret;

	ESP_LOGI(TAG, "GPIO_CS=%d",CONFIG_CS_GPIO);
	if ( CONFIG_CS_GPIO >= 0 ) {
		//gpio_pad_select_gpio( GPIO_CS );
		gpio_reset_pin( CONFIG_CS_GPIO );
		gpio_set_direction( CONFIG_CS_GPIO, GPIO_MODE_OUTPUT );
		gpio_set_level( CONFIG_CS_GPIO, 0 );
	}

	ESP_LOGI(TAG, "GPIO_DC=%d",CONFIG_DC_GPIO);
	//gpio_pad_select_gpio( GPIO_DC );
	gpio_reset_pin( CONFIG_DC_GPIO );
	gpio_set_direction( CONFIG_DC_GPIO, GPIO_MODE_OUTPUT );
	gpio_set_level( CONFIG_DC_GPIO, 0 );

	ESP_LOGI(TAG, "GPIO_RESET=%d",CONFIG_RESET_GPIO);
	if ( CONFIG_RESET_GPIO >= 0 ) {
		//gpio_pad_select_gpio( GPIO_RESET );
		gpio_reset_pin( CONFIG_RESET_GPIO );
		gpio_set_direction( CONFIG_RESET_GPIO, GPIO_MODE_OUTPUT );
		gpio_set_level( CONFIG_RESET_GPIO, 1 );
		vTaskDelay(50 / portTICK_PERIOD_MS);
		gpio_set_level( CONFIG_RESET_GPIO, 0 );
		vTaskDelay(50 / portTICK_PERIOD_MS);
		gpio_set_level( CONFIG_RESET_GPIO, 1 );
		vTaskDelay(50 / portTICK_PERIOD_MS);
	}

	ESP_LOGI(TAG, "GPIO_BL=%d",CONFIG_BL_GPIO);
	if ( CONFIG_BL_GPIO >= 0 ) {
		//gpio_pad_select_gpio(GPIO_BL);
		gpio_reset_pin(CONFIG_BL_GPIO);
		gpio_set_direction( CONFIG_BL_GPIO, GPIO_MODE_OUTPUT );
		gpio_set_level( CONFIG_BL_GPIO, 1 );
	}

	ESP_LOGI(TAG, "GPIO_MOSI=%d",CONFIG_MOSI_GPIO);
	ESP_LOGI(TAG, "GPIO_SCLK=%d",CONFIG_SCLK_GPIO);
	spi_bus_config_t buscfg = {
		.sclk_io_num = CONFIG_SCLK_GPIO,
		.mosi_io_num = CONFIG_MOSI_GPIO,
		.miso_io_num = -1,
		.quadwp_io_num = -1,
		.quadhd_io_num = -1,
		.max_transfer_sz = SPI_BUS_MAX_TRANSFER_SZ
	};

	spi_host = SPI2_HOST;
	ret = spi_bus_initialize( SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO );
	ESP_LOGD(TAG, "spi_bus_initialize=%d",ret);
	assert(ret==ESP_OK);

	spi_device_interface_config_t devcfg={
		.clock_speed_hz = SPI_MASTER_FREQ_20M,
		.queue_size = SPI_TRANSACTION_POOL_SIZE,
		.mode = 2,
		.flags = SPI_DEVICE_NO_DUMMY,
	};

	if ( CONFIG_CS_GPIO >= 0 ) {
		devcfg.spics_io_num = CONFIG_CS_GPIO;
	} else {
		devcfg.spics_io_num = -1;
	}

	//spi_device_handle_t handle;
	ret = spi_bus_add_device( SPI2_HOST, &devcfg, &spi);
	ESP_LOGD(TAG, "spi_bus_add_device=%d",ret);
	assert(ret==ESP_OK);

	/* create the transaction pool and fill it with ptrs to spi_transaction_ext_t to reuse */
	if(TransactionPool == NULL) {
		TransactionPool = xQueueCreate(SPI_TRANSACTION_POOL_SIZE, sizeof(spi_transaction_ext_t*));
		assert(TransactionPool != NULL);
		for (size_t i = 0; i < SPI_TRANSACTION_POOL_SIZE; i++)
		{
			spi_transaction_ext_t* pTransaction = (spi_transaction_ext_t*)heap_caps_malloc(sizeof(spi_transaction_ext_t), MALLOC_CAP_DMA);
			assert(pTransaction != NULL);
			memset(pTransaction, 0, sizeof(spi_transaction_ext_t));
			xQueueSend(TransactionPool, &pTransaction, portMAX_DELAY);
		}
	}
}

/* The ST7789 display controller can drive up to 320*240 displays, when using a 240*240 or 240*135
 * displays there's a gap of 80px or 40/52/53px respectively. 52px or 53x offset depends on display orientation.
 * We need to edit the coordinates to take into account those gaps, this is not necessary in all orientations. */
void st7789_flush(lv_display_t * disp, const lv_area_t * area, uint8_t * pixmap)
{
	//ESP_LOGI("Debug_a", "flush");
	//ESP_LOGI("COLOR:","%d",(int)color_map[0].full);

    uint8_t data[4] = {0};

    uint16_t offsetx1 = area->x1;
    uint16_t offsetx2 = area->x2;
    uint16_t offsety1 = area->y1;
    uint16_t offsety2 = area->y2;

#if (CONFIG_LV_TFT_DISPLAY_OFFSETS)
    offsetx1 += CONFIG_LV_TFT_DISPLAY_X_OFFSET;
    offsetx2 += CONFIG_LV_TFT_DISPLAY_X_OFFSET;
    offsety1 += CONFIG_LV_TFT_DISPLAY_Y_OFFSET;
    offsety2 += CONFIG_LV_TFT_DISPLAY_Y_OFFSET;

#elif (LV_HOR_RES_MAX == 240) && (LV_VER_RES_MAX == 240)
    #if (CONFIG_LV_DISPLAY_ORIENTATION_PORTRAIT)
        offsetx1 += 80;
        offsetx2 += 80;
    #elif (CONFIG_LV_DISPLAY_ORIENTATION_LANDSCAPE_INVERTED)
        offsety1 += 80;
        offsety2 += 80;
    #endif
#elif (LV_HOR_RES_MAX == 240) && (LV_VER_RES_MAX == 135)
    #if (CONFIG_LV_DISPLAY_ORIENTATION_PORTRAIT) || \
        (CONFIG_LV_DISPLAY_ORIENTATION_PORTRAIT_INVERTED)
        offsetx1 += 40;
        offsetx2 += 40;
        offsety1 += 53;
        offsety2 += 53;
    #endif
#elif (LV_HOR_RES_MAX == 135) && (LV_VER_RES_MAX == 240)
    #if (CONFIG_LV_DISPLAY_ORIENTATION_LANDSCAPE) || \
        (CONFIG_LV_DISPLAY_ORIENTATION_LANDSCAPE_INVERTED)
        offsetx1 += 52;
        offsetx2 += 52;
        offsety1 += 40;
        offsety2 += 40;
    #endif
#endif

    /*Column addresses*/
    st7789_send_cmd(ST7789_CASET);
    data[0] = (offsetx1 >> 8) & 0xFF;
    data[1] = offsetx1 & 0xFF;
    data[2] = (offsetx2 >> 8) & 0xFF;
    data[3] = offsetx2 & 0xFF;
    st7789_send_data(data, 4);

    /*Page addresses*/
    st7789_send_cmd(ST7789_RASET);
    data[0] = (offsety1 >> 8) & 0xFF;
    data[1] = offsety1 & 0xFF;
    data[2] = (offsety2 >> 8) & 0xFF;
    data[3] = offsety2 & 0xFF;
    st7789_send_data(data, 4);

    /*Memory write*/
    st7789_send_cmd(ST7789_RAMWR);

    size_t size = (size_t)lv_area_get_width(area) * (size_t)lv_area_get_height(area);

    st7789_send_color((void*)pixmap, size * 2);

    lv_display_flush_ready(disp);         /* Indicate you are ready with the flushing*/
}

/**********************
 *   STATIC FUNCTIONS
 **********************/
void disp_spi_transaction(const uint8_t *data, size_t length,
    disp_spi_send_flag_t flags, uint8_t *out,
    uint64_t addr, uint8_t dummy_bits)
{
    if (0 == length) {
        return;
    }

    spi_transaction_ext_t t = {0};

    /* transaction length is in bits */
    t.base.length = length * 8;

    if (length <= 4 && data != NULL) {
        t.base.flags = SPI_TRANS_USE_TXDATA;
        memcpy(t.base.tx_data, data, length);
    } else {
        t.base.tx_buffer = data;
    }

    if (flags & DISP_SPI_RECEIVE) {
        assert(out != NULL && (flags & (DISP_SPI_SEND_POLLING | DISP_SPI_SEND_SYNCHRONOUS)));
        t.base.rx_buffer = out;

#if defined(DISP_SPI_HALF_DUPLEX)
		t.base.rxlength = t.base.length;
		t.base.length = 0;	/* no MOSI phase in half-duplex reads */
#else
		t.base.rxlength = 0; /* in full-duplex mode, zero means same as tx length */
#endif
    }

    if (flags & DISP_SPI_ADDRESS_8) {
        t.address_bits = 8;
    } else if (flags & DISP_SPI_ADDRESS_16) {
        t.address_bits = 16;
    } else if (flags & DISP_SPI_ADDRESS_24) {
        t.address_bits = 24;
    } else if (flags & DISP_SPI_ADDRESS_32) {
        t.address_bits = 32;
    }
    if (t.address_bits) {
        t.base.addr = addr;
        t.base.flags |= SPI_TRANS_VARIABLE_ADDR;
    }

#if defined(DISP_SPI_HALF_DUPLEX)
	if (flags & DISP_SPI_MODE_DIO) {
		t.base.flags |= SPI_TRANS_MODE_DIO;
	} else if (flags & DISP_SPI_MODE_QIO) {
		t.base.flags |= SPI_TRANS_MODE_QIO;
	}

	if (flags & DISP_SPI_MODE_DIOQIO_ADDR) {
		t.base.flags |= SPI_TRANS_MODE_DIOQIO_ADDR;
	}

	if ((flags & DISP_SPI_VARIABLE_DUMMY) && dummy_bits) {
		t.dummy_bits = dummy_bits;
		t.base.flags |= SPI_TRANS_VARIABLE_DUMMY;
	}
#endif

    /* Save flags for pre/post transaction processing */
    t.base.user = (void *) flags;

    /* Poll/Complete/Queue transaction */
    if (flags & DISP_SPI_SEND_POLLING) {
		disp_wait_for_pending_transactions();	/* before polling, all previous pending transactions need to be serviced */
        spi_device_polling_transmit(spi, (spi_transaction_t *) &t);
    } else if (flags & DISP_SPI_SEND_SYNCHRONOUS) {
		disp_wait_for_pending_transactions();	/* before synchronous queueing, all previous pending transactions need to be serviced */
        spi_device_transmit(spi, (spi_transaction_t *) &t);
    } else {

		/* if necessary, ensure we can queue new transactions by servicing some previous transactions */
		if(uxQueueMessagesWaiting(TransactionPool) == 0) {
			spi_transaction_t *presult;
			while(uxQueueMessagesWaiting(TransactionPool) < SPI_TRANSACTION_POOL_RESERVE) {
				if (spi_device_get_trans_result(spi, &presult, 1) == ESP_OK) {
					xQueueSend(TransactionPool, &presult, portMAX_DELAY);	/* back to the pool to be reused */
				}
			}
		}

		spi_transaction_ext_t *pTransaction = NULL;
		xQueueReceive(TransactionPool, &pTransaction, portMAX_DELAY);
        memcpy(pTransaction, &t, sizeof(t));
        if (spi_device_queue_trans(spi, (spi_transaction_t *) pTransaction, portMAX_DELAY) != ESP_OK) {
			xQueueSend(TransactionPool, &pTransaction, portMAX_DELAY);	/* send failed transaction back to the pool to be reused */
        }
    }
}

static inline void disp_spi_send_data(uint8_t *data, size_t length) {
    disp_spi_transaction(data, length, DISP_SPI_SEND_POLLING, NULL, 0, 0);
}

static inline void disp_spi_send_colors(uint8_t *data, size_t length) {
    disp_spi_transaction(data, length,
        DISP_SPI_SEND_QUEUED | DISP_SPI_SIGNAL_FLUSH,
        NULL, 0, 0);
}


void disp_wait_for_pending_transactions(void)
{
    spi_transaction_t *presult;


	while(uxQueueMessagesWaiting(TransactionPool) < SPI_TRANSACTION_POOL_SIZE) {	/* service until the transaction reuse pool is full again */
        if (spi_device_get_trans_result(spi, &presult, 1) == ESP_OK) {
			xQueueSend(TransactionPool, &presult, portMAX_DELAY);
        }
    	//ESP_LOGI("Debug_a", "check pending");
    }
}

void st7789_send_cmd(uint8_t cmd)
{
    disp_wait_for_pending_transactions();
    gpio_set_level(ST7789_DC, 0);
    disp_spi_send_data(&cmd, 1);
}

void st7789_send_data(void * data, uint16_t length)
{
    disp_wait_for_pending_transactions();
    gpio_set_level(ST7789_DC, 1);
    disp_spi_send_data(data, length);
}

#define SIZE_B 23040	//2*DISP_BUF_SIZE / 10;
static void st7789_send_color(void * data, size_t length)
{
	static uint8_t buf_c[SIZE_B];
	uint8_t *data_i = data;
	int asd = (uint8_t)data_i[0];
    disp_wait_for_pending_transactions();
    gpio_set_level(ST7789_DC, 1);
    for(int idx = 0; idx < length ; idx +=2){		//Invert color bytes
    	buf_c[idx] = data_i[idx +1];
    	buf_c[idx+1] = data_i[idx];
    }
    disp_spi_send_colors(buf_c, length);
}

static void st7789_set_orientation(uint8_t orientation)
{
    // ESP_ASSERT(orientation < 4);

    const char *orientation_str[] = {
        "PORTRAIT", "PORTRAIT_INVERTED", "LANDSCAPE", "LANDSCAPE_INVERTED"
    };

    ESP_LOGI(TAG, "Display orientation: %s", orientation_str[orientation]);

    uint8_t data[] =
    {
#if CONFIG_LV_PREDEFINED_DISPLAY_TTGO
	0x60, 0xA0, 0x00, 0xC0
#else
	0xC0, 0x00, 0x60, 0xA0
#endif
    };

    ESP_LOGI(TAG, "0x36 command value: 0x%02X", data[orientation]);

    st7789_send_cmd(ST7789_MADCTL);
    st7789_send_data((void *) &data[orientation], 1);
}
