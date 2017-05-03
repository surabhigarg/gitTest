APP_NAME = rabbit_mq_client
APP_SRC_FILES = rabbit_mq_client.c
APP_UTIL_FILE = ../examples/utils.c

#IOT_SRC_FILES += $(shell find ../librabbitmq/ -name '*.c')
#IOT_SRC_FILES += $(shell find ../tools -name '*.c')

#cJSON
CJSON_LIB_DIR = ../cJSON/
CJSON_INCLUDE_DIR = -I../cJSON/
EXTERNAL_LIBS += -L$(CJSON_LIB_DIR)
LD_FLAG += -lcjson -lm

#FileMgr and Interface
FILE_MGR_DIR = ../../../filemgr-app/filemgr-app
INTERFACE_DIR = ../../../interface-app/interface-app
LIBCONFIG_DIR = ../../../libconfig-app/libconfig-app
WDM_DIR = ../../../watchdog-manager-app/watchdog-manager-app
RABBITMQ_DIR = ../librabbitmq/
UTILS_DIR = ../examples

FILE_INCLUDE_DIR = -I$(FILE_MGR_DIR)/inc
INTERFACE_INCLUDE_DIR = -I$(INTERFACE_DIR)/inc
LIBCONFIG_INCLUDE_DIR = -I$(LIBCONFIG_DIR)/lib
RABBITMQ_INCLUDE_DIR = -I. -I$(RABBITMQ_DIR)/ -I../examples/
WDM_INCLUDE_DIR = -I$(WDM_DIR)/inc
EXTERNAL_LIBS += -L$(FILE_MGR_DIR)/lib -L$(INTERFACE_DIR)/lib -L$(LIBCONFIG_DIR)/lib/.libs -L$(WDM_DIR)/lib -L$(RABBITMQ_DIR)/.libs -L$(UTILS_DIR)/
#EXTERNAL_LIBS += -L$(FILE_MGR_DIR)/lib -L$(INTERFACE_DIR)/lib -L$(LIBCONFIG_DIR)/lib/.libs -L$(WDM_DIR)/lib -L$(UTILS_DIR)/
LD_FLAG  += -lfilemsgdrv -lmsgutil -lfilemgr -lconfig -lsemutil -lpthread -lcloudcontrol -lsqlitecommon -lwdm -lrabbitmq -lcurl

#SQLITE3
LD_FLAG += -lsqlite3

#Aggregate all include and src directories
INCLUDE_ALL_DIRS += $(FILE_INCLUDE_DIR)
INCLUDE_ALL_DIRS += $(INTERFACE_INCLUDE_DIR)
INCLUDE_ALL_DIRS += $(LIBCONFIG_INCLUDE_DIR)
INCLUDE_ALL_DIRS += $(WDM_INCLUDE_DIR)
INCLUDE_ALL_DIRS += $(RABBITMQ_INCLUDE_DIR)
INCLUDE_ALL_DIRS += $(CJSON_INCLUDE_DIR)

COMPILER_FLAGS += -g -pthread -pg
COMPILER_FLAGS += $(LOG_FLAGS)

CJSON_MAKE_CMD = $(MAKE) -C $(CJSON_LIB_DIR)
PRE_CJSON_MAKE_CMD = $(CJSON_MAKE_CMD)
MAKE_CMD = $(CC) $(APP_UTIL_FILE) $(APP_SRC_FILES) $(COMPILER_FLAGS) -o $(APP_NAME) $(LD_FLAG) $(EXTERNAL_LIBS) $(INCLUDE_ALL_DIRS)

all:
	$(PRE_CJSON_MAKE_CMD)
	$(MAKE_CMD)

clean:
	rm -f $(APP_NAME)
