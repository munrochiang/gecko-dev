# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

include  $(topsrcdir)/toolkit/mozapps/installer/package-name.mk

installer:
	@$(MAKE) -C mobile/android/installer installer

package:
	@$(MAKE) -C mobile/android/installer

ifeq ($(OS_TARGET),Android)
sdk_level=$(shell $(ADB) shell getprop ro.build.version.sdk)
permissions-required=$(shell if [ $(sdk_level) -gt 22 ] ; then echo yes ; else echo no ; fi)
ifneq ($(MOZ_ANDROID_INSTALL_TARGET),)
ANDROID_SERIAL = $(MOZ_ANDROID_INSTALL_TARGET)
endif
ifneq ($(ANDROID_SERIAL),)
export ANDROID_SERIAL
else
# Determine if there's more than one device connected
android_devices=$(filter device,$(shell $(ADB) devices))
ifeq ($(android_devices),)
install::
	@echo 'No devices are connected.  Connect a device or start an emulator.'
	@exit 1
else
ifneq ($(android_devices),device)
install::
	@echo 'Multiple devices are connected. Define ANDROID_SERIAL to specify the install target.'
	$(ADB) devices
	@exit 1
endif
endif
endif

install::
ifeq ($(permissions-required),yes)

	$(ADB) install -r -g $(DIST)/$(PKG_PATH)$(PKG_BASENAME).apk
else
	$(ADB) install -r $(DIST)/$(PKG_PATH)$(PKG_BASENAME).apk
endif
else
	@echo 'Mobile can't be installed directly.'
	@exit 1
endif

deb: package
	@$(MAKE) -C mobile/android/installer deb

upload::
	@$(MAKE) -C mobile/android/installer upload

ifdef ENABLE_TESTS
# Implemented in testing/testsuite-targets.mk

mochitest-browser-chrome:
	$(RUN_MOCHITEST) --browser-chrome
	$(CHECK_TEST_ERROR)

mochitest:: mochitest-browser-chrome

.PHONY: mochitest-browser-chrome
endif

ifeq ($(OS_TARGET),Linux)
deb: installer
endif
