#
# This is a project Makefile. It is assumed the directory this Makefile resides in is a
# project subdirectory.
#

PROJECT_NAME := lcder

#The name of the lvgl folder (change this if you have renamed it)
LVGL_DIR_NAME ?= GUI
#The path where the lvgl folder is
LVGL_DIR ?= $(shell pwd)
include $(LVGL_DIR)/$(LVGL_DIR_NAME)/lvgl.mk

EXTRA_COMPONENT_DIRS := ./GUI

include $(IDF_PATH)/make/project.mk
