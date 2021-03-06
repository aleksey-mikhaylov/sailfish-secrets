TEMPLATE=lib
CONFIG+=plugin
TARGET=sailfishsecrets-testinappauth
TARGET = $$qtLibraryTarget($$TARGET)

include($$PWD/../../../common.pri)
include($$PWD/../../../api/libsailfishsecrets/libsailfishsecrets.pri)

DEFINES+=SAILFISH_SECRETS_BUILD_TEST_PLUGIN
HEADERS+=$$PWD/../../inappauthplugin/plugin.h
SOURCES+=$$PWD/../../inappauthplugin/plugin.cpp

target.path=/usr/lib/sailfishsecrets/
INSTALLS += target
