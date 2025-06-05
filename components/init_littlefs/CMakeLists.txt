idf_component_register(
    SRCS "init_littlefs.c" 
    INCLUDE_DIRS "."
	REQUIRES lua driver esp_adc esp_event  spi_flash fatfs littlefs esp_driver_spi
)
