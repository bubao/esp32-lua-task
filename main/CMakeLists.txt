idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS "."
					REQUIRES esp_event driver lua_engine init_littlefs)
