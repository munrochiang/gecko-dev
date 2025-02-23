/* -*- Mode: indent-tabs-mode: nil; js-indent-level: 2 -*- */
/* vim: set sts=2 sw=2 et tw=80: */
"use strict";

add_task(function* () {
  let tab = yield BrowserTestUtils.openNewForegroundTab(gBrowser, "about:robots");
  gBrowser.selectedTab = tab;

  // TODO: Multiple windows.

  // Using pre-loaded new tab pages interferes with onUpdated events.
  // It probably shouldn't.
  SpecialPowers.setBoolPref("browser.newtab.preload", false);
  registerCleanupFunction(() => {
    SpecialPowers.clearUserPref("browser.newtab.preload");
  });

  let extension = ExtensionTestUtils.loadExtension({
    manifest: {
      "permissions": ["tabs"],

      "background": { "page": "bg/background.html" },
    },

    files: {
      "bg/blank.html": `<html><head><meta charset="utf-8"></head></html>`,

      "bg/background.html": `<html><head>
        <meta charset="utf-8">
        <script src="background.js"></script>
      </head></html>`,

      "bg/background.js": function() {
        // Wrap API methods in promise-based variants.
        let promiseTabs = {};
        Object.keys(browser.tabs).forEach(method => {
          promiseTabs[method] = (...args) => {
            return new Promise(resolve => {
              browser.tabs[method](...args, resolve);
            });
          };
        });


        let activeTab;
        let activeWindow;

        function runTests() {
          const DEFAULTS = {
            index: 2,
            windowId: activeWindow,
            active: true,
            pinned: false,
            url: "chrome://browser/content/newtab/newTab.xhtml",
          };

          let tests = [
            {
              create: { url: "http://example.com/" },
              result: { url: "http://example.com/" },
            },
            {
              create: { url: "blank.html" },
              result: { url: browser.runtime.getURL("bg/blank.html") },
            },
            {
              create: {},
              result: { url: "chrome://browser/content/newtab/newTab.xhtml" },
            },
            {
              create: { active: false },
              result: { active: false },
            },
            {
              create: { active: true },
              result: { active: true },
            },
            {
              create: { pinned: true },
              result: { pinned: true, index: 0 },
            },
            {
              create: { pinned: true, active: true },
              result: { pinned: true, active: true, index: 0 },
            },
            {
              create: { pinned: true, active: false },
              result: { pinned: true, active: false, index: 0 },
            },
            {
              create: { index: 1 },
              result: { index: 1 },
            },
            {
              create: { index: 1, active: false },
              result: { index: 1, active: false },
            },
            {
              create: { windowId: activeWindow },
              result: { windowId: activeWindow },
            },
          ];

          function nextTest() {
            if (!tests.length) {
              browser.test.notifyPass("tabs.create");
              return;
            }

            let test = tests.shift();
            let expected = Object.assign({}, DEFAULTS, test.result);

            browser.test.log(`Testing tabs.create(${JSON.stringify(test.create)}), expecting ${JSON.stringify(test.result)}`);

            let tabId;
            let updatedPromise = new Promise(resolve => {
              let onUpdated = (changedTabId, changed) => {
                if (changedTabId === tabId && changed.url) {
                  browser.tabs.onUpdated.removeListener(onUpdated);
                  resolve(changed.url);
                }
              };
              browser.tabs.onUpdated.addListener(onUpdated);
            });

            promiseTabs.create(test.create).then(tab => {
              tabId = tab.id;

              for (let key of Object.keys(expected)) {
                if (key === "url") {
                  // FIXME: This doesn't get updated until later in the load cycle.
                  continue;
                }

                browser.test.assertEq(expected[key], tab[key], `Expected value for tab.${key}`);
              }

              return updatedPromise;
            }).then(url => {
              browser.test.assertEq(expected.url, url, `Expected value for tab.url`);

              return promiseTabs.remove(tabId);
            }).then(() => {
              return promiseTabs.update(activeTab, { active: true });
            }).then(() => {
              nextTest();
            });
          }

          nextTest();
        }

        browser.tabs.query({ active: true, currentWindow: true }, tabs => {
          activeTab = tabs[0].id;
          activeWindow = tabs[0].windowId;

          runTests();
        });
      },
    },
  });

  yield extension.startup();
  yield extension.awaitFinish("tabs.create");
  yield extension.unload();

  yield BrowserTestUtils.removeTab(tab);
});

