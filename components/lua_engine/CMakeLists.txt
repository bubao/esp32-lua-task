idf_component_register(
	SRCS "lua_adc.c" 
	"lua_cron.c"
	"lua_file.c"
	"lua_gpio.c"
	"lua_timer.c"
	"lua_bindings.c"
	"lua_reg.c"
	"lua_engine.c"
	INCLUDE_DIRS "."
	REQUIRES lua driver esp_adc esp_event esp_http_client spi_flash fatfs littlefs esp_driver_spi esp_timer esp_cron
)
