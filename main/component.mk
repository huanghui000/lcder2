#
# Main Makefile. This is basically the same as a component makefile.
#

COMPONENT_ADD_INCLUDEDIRS := GUI/

COMPONENT_SRCDIRS := .
COMPONENT_SRCDIRS += GUI/
COMPONENT_SRCDIRS += screens
COMPONENT_SRCDIRS += fonts
COMPONENT_SRCDIRS += ../GUI/src/extra/libs/qrcode
COMPONENT_SRCDIRS += ../GUI/src/extra/libs/fsdrv

COMPONENT_OBJEXCLUDE += fonts/ui_font_simliSmall.o
