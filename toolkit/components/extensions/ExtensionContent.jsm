/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

"use strict";

this.EXPORTED_SYMBOLS = ["ExtensionContent"];

/* globals ExtensionContent */

/*
 * This file handles the content process side of extensions. It mainly
 * takes care of content script injection, content script APIs, and
 * messaging.
 */

const Ci = Components.interfaces;
const Cc = Components.classes;
const Cu = Components.utils;
const Cr = Components.results;

Cu.import("resource://gre/modules/XPCOMUtils.jsm");
Cu.import("resource://gre/modules/Services.jsm");
Cu.import("resource://gre/modules/AppConstants.jsm");

XPCOMUtils.defineLazyModuleGetter(this, "ExtensionManagement",
                                  "resource://gre/modules/ExtensionManagement.jsm");
XPCOMUtils.defineLazyModuleGetter(this, "MatchPattern",
                                  "resource://gre/modules/MatchPattern.jsm");
XPCOMUtils.defineLazyModuleGetter(this, "PrivateBrowsingUtils",
                                  "resource://gre/modules/PrivateBrowsingUtils.jsm");

Cu.import("resource://gre/modules/ExtensionUtils.jsm");
var {
  runSafeSyncWithoutClone,
  LocaleData,
  MessageBroker,
  Messenger,
  injectAPI,
  flushJarCache,
} = ExtensionUtils;

function isWhenBeforeOrSame(when1, when2) {
  let table = {"document_start": 0,
               "document_end": 1,
               "document_idle": 2};
  return table[when1] <= table[when2];
}

// This is the fairly simple API that we inject into content
// scripts.
var api = context => {
  return {
    runtime: {
      connect: function(extensionId, connectInfo) {
        if (!connectInfo) {
          connectInfo = extensionId;
          extensionId = null;
        }
        let name = connectInfo && connectInfo.name || "";
        let recipient = extensionId ? {extensionId} : {extensionId: context.extensionId};
        return context.messenger.connect(context.messageManager, name, recipient);
      },

      getManifest: function() {
        return Cu.cloneInto(context.extension.manifest, context.cloneScope);
      },

      getURL: function(url) {
        return context.extension.baseURI.resolve(url);
      },

      onConnect: context.messenger.onConnect("runtime.onConnect"),

      onMessage: context.messenger.onMessage("runtime.onMessage"),

      sendMessage: function(...args) {
        let options; // eslint-disable-line no-unused-vars
        let extensionId, message, responseCallback;
        if (args.length == 1) {
          message = args[0];
        } else if (args.length == 2) {
          [message, responseCallback] = args;
        } else {
          [extensionId, message, options, responseCallback] = args;
        }

        let recipient = extensionId ? {extensionId} : {extensionId: context.extensionId};
        context.messenger.sendMessage(context.messageManager, message, recipient, responseCallback);
      },
    },

    extension: {
      getURL: function(url) {
        return context.extension.baseURI.resolve(url);
      },

      inIncognitoContext: PrivateBrowsingUtils.isContentWindowPrivate(context.contentWindow),
    },

    i18n: {
      getMessage: function(messageName, substitutions) {
        return context.extension.localizeMessage(messageName, substitutions);
      },
    },
  };
};

// Represents a content script.
function Script(options) {
  this.options = options;
  this.run_at = this.options.run_at;
  this.js = this.options.js || [];
  this.css = this.options.css || [];

  this.matches_ = new MatchPattern(this.options.matches);
  this.exclude_matches_ = new MatchPattern(this.options.exclude_matches || null);
  // TODO: MatchPattern should pre-mangle host-only patterns so that we
  // don't need to call a separate match function.
  this.matches_host_ = new MatchPattern(this.options.matchesHost || null);

  // TODO: Support glob patterns.
}

Script.prototype = {
  matches(window) {
    let uri = window.document.documentURIObject;
    if (!(this.matches_.matches(uri) || this.matches_host_.matchesIgnoringPath(uri))) {
      return false;
    }

    if (this.exclude_matches_.matches(uri)) {
      return false;
    }

    if (!this.options.all_frames && window.top != window) {
      return false;
    }

    if ("innerWindowID" in this.options) {
      let innerWindowID = window.QueryInterface(Ci.nsIInterfaceRequestor)
                                .getInterface(Ci.nsIDOMWindowUtils)
                                .currentInnerWindowID;

      if (innerWindowID !== this.options.innerWindowID) {
        return false;
      }
    }

    // TODO: match_about_blank.

    return true;
  },

  tryInject(extension, window, sandbox, shouldRun) {
    if (!this.matches(window)) {
      return;
    }

    if (shouldRun("document_start")) {
      let winUtils = window.QueryInterface(Ci.nsIInterfaceRequestor)
                           .getInterface(Ci.nsIDOMWindowUtils);

      for (let url of this.css) {
        url = extension.baseURI.resolve(url);
        runSafeSyncWithoutClone(winUtils.loadSheetUsingURIString, url, winUtils.AUTHOR_SHEET);
      }

      if (this.options.cssCode) {
        let url = "data:text/css;charset=utf-8," + encodeURIComponent(this.options.cssCode);
        runSafeSyncWithoutClone(winUtils.loadSheetUsingURIString, url, winUtils.AUTHOR_SHEET);
      }
    }

    let scheduled = this.run_at || "document_idle";
    if (shouldRun(scheduled)) {
      for (let url of this.js) {
        // On gonk we need to load the resources asynchronously because the
        // app: channels only support asyncOpen. This is safe only in the
        // `document_idle` state.
        if (AppConstants.platform == "gonk" && scheduled != "document_idle") {
          Cu.reportError(`Script injection: ignoring ${url} at ${scheduled}`);
          continue;
        }
        url = extension.baseURI.resolve(url);

        let options = {
          target: sandbox,
          charset: "UTF-8",
          async: AppConstants.platform == "gonk",
        };
        runSafeSyncWithoutClone(Services.scriptloader.loadSubScriptWithOptions, url, options);
      }

      if (this.options.jsCode) {
        Cu.evalInSandbox(this.options.jsCode, sandbox, "latest");
      }
    }
  },
};

function getWindowMessageManager(contentWindow) {
  let ir = contentWindow.QueryInterface(Ci.nsIInterfaceRequestor)
                        .getInterface(Ci.nsIDocShell)
                        .QueryInterface(Ci.nsIInterfaceRequestor);
  try {
    return ir.getInterface(Ci.nsIContentFrameMessageManager);
  } catch (e) {
    // Some windows don't support this interface (hidden window).
    return null;
  }
}

var ExtensionManager;

// Scope in which extension content script code can run. It uses
// Cu.Sandbox to run the code. There is a separate scope for each
// frame.
function ExtensionContext(extensionId, contentWindow, contextOptions = {}) {
  let { isExtensionPage } = contextOptions;

  this.isExtensionPage = isExtensionPage;
  this.extension = ExtensionManager.get(extensionId);
  this.extensionId = extensionId;
  this.contentWindow = contentWindow;

  this.onClose = new Set();

  let utils = contentWindow.QueryInterface(Ci.nsIInterfaceRequestor)
                           .getInterface(Ci.nsIDOMWindowUtils);
  let outerWindowId = utils.outerWindowID;
  let frameId = contentWindow == contentWindow.top ? 0 : outerWindowId;
  this.frameId = frameId;

  let mm = getWindowMessageManager(contentWindow);
  this.messageManager = mm;

  let prin;
  let contentPrincipal = contentWindow.document.nodePrincipal;
  let ssm = Services.scriptSecurityManager;
  if (ssm.isSystemPrincipal(contentPrincipal)) {
    // Make sure we don't hand out the system principal by accident.
    prin = Cc["@mozilla.org/nullprincipal;1"].createInstance(Ci.nsIPrincipal);
  } else {
    let extensionPrincipal = ssm.createCodebasePrincipal(this.extension.baseURI, {addonId: extensionId});
    prin = [contentPrincipal, extensionPrincipal];
  }

  if (isExtensionPage) {
    if (ExtensionManagement.getAddonIdForWindow(this.contentWindow) != extensionId) {
      throw new Error("Invalid target window for this extension context");
    }
    // This is an iframe with content script API enabled and its principal should be the
    // contentWindow itself. (we create a sandbox with the contentWindow as principal and with X-rays disabled
    // because it enables us to create the APIs object in this sandbox object and then copying it
    // into the iframe's window, see Bug 1214658 for rationale)
    this.sandbox = Cu.Sandbox(contentWindow, {
      sandboxPrototype: contentWindow,
      wantXrays: false,
      isWebExtensionContentScript: true,
    });
  } else {
    this.sandbox = Cu.Sandbox(prin, {
      sandboxPrototype: contentWindow,
      wantXrays: true,
      isWebExtensionContentScript: true,
      wantGlobalProperties: ["XMLHttpRequest"],
    });
  }

  let delegate = {
    getSender(context, target, sender) {
      // Nothing to do here.
    },
  };

  let url = contentWindow.location.href;
  let broker = ExtensionContent.getBroker(mm);
  // The |sender| parameter is passed directly to the extension.
  let sender = {id: this.extension.uuid, frameId, url};
  // Properties in |filter| must match those in the |recipient|
  // parameter of sendMessage.
  let filter = {extensionId, frameId};
  this.messenger = new Messenger(this, broker, sender, filter, delegate);

  this.chromeObj = Cu.createObjectIn(this.sandbox, {defineAs: "browser"});

  // Sandboxes don't get Xrays for some weird compatibility
  // reason. However, we waive here anyway in case that changes.
  Cu.waiveXrays(this.sandbox).chrome = this.chromeObj;

  injectAPI(api(this), this.chromeObj);

  // This is an iframe with content script API enabled. (See Bug 1214658 for rationale)
  if (isExtensionPage) {
    Cu.waiveXrays(this.contentWindow).chrome = this.chromeObj;
    Cu.waiveXrays(this.contentWindow).browser = this.chromeObj;
  }
}

ExtensionContext.prototype = {
  get cloneScope() {
    return this.sandbox;
  },

  execute(script, shouldRun) {
    script.tryInject(this.extension, this.contentWindow, this.sandbox, shouldRun);
  },

  callOnClose(obj) {
    this.onClose.add(obj);
  },

  forgetOnClose(obj) {
    this.onClose.delete(obj);
  },

  close() {
    for (let obj of this.onClose) {
      obj.close();
    }

    // Overwrite the content script APIs with an empty object if the APIs objects are still
    // defined in the content window (See Bug 1214658 for rationale).
    if (this.isExtensionPage && !Cu.isDeadWrapper(this.contentWindow) &&
        Cu.waiveXrays(this.contentWindow).browser === this.chromeObj) {
      Cu.createObjectIn(this.contentWindow, { defineAs: "browser" });
      Cu.createObjectIn(this.contentWindow, { defineAs: "chrome" });
    }

    Cu.nukeSandbox(this.sandbox);
    this.sandbox = null;
  },
};

function windowId(window) {
  return window.QueryInterface(Ci.nsIInterfaceRequestor)
               .getInterface(Ci.nsIDOMWindowUtils)
               .currentInnerWindowID;
}

// Responsible for creating ExtensionContexts and injecting content
// scripts into them when new documents are created.
var DocumentManager = {
  extensionCount: 0,

  // Map[windowId -> Map[extensionId -> ExtensionContext]]
  contentScriptWindows: new Map(),

  // Map[windowId -> ExtensionContext]
  extensionPageWindows: new Map(),

  init() {
    Services.obs.addObserver(this, "document-element-inserted", false);
    Services.obs.addObserver(this, "inner-window-destroyed", false);
  },

  uninit() {
    Services.obs.removeObserver(this, "document-element-inserted");
    Services.obs.removeObserver(this, "inner-window-destroyed");
  },

  getWindowState(contentWindow) {
    let readyState = contentWindow.document.readyState;
    if (readyState == "loading") {
      return "document_start";
    } else if (readyState == "interactive") {
      return "document_end";
    } else {
      return "document_idle";
    }
  },

  observe: function(subject, topic, data) {
    if (topic == "document-element-inserted") {
      let document = subject;
      let window = document && document.defaultView;
      if (!document || !document.location || !window) {
        return;
      }

      // Make sure we only load into frames that ExtensionContent.init
      // was called on (i.e., not frames for social or sidebars).
      let mm = getWindowMessageManager(window);
      if (!mm || !ExtensionContent.globals.has(mm)) {
        return;
      }

      // Enable the content script APIs should be available in subframes' window
      // if it is recognized as a valid addon id (see Bug 1214658 for rationale).
      const { CONTENTSCRIPT_PRIVILEGES } = ExtensionManagement.API_LEVELS;
      let extensionId = ExtensionManagement.getAddonIdForWindow(window);

      if (ExtensionManagement.getAPILevelForWindow(window, extensionId) == CONTENTSCRIPT_PRIVILEGES &&
          ExtensionManager.get(extensionId)) {
        DocumentManager.getExtensionPageContext(extensionId, window);
      }

      this.trigger("document_start", window);
      /* eslint-disable mozilla/balanced-listeners */
      window.addEventListener("DOMContentLoaded", this, true);
      window.addEventListener("load", this, true);
      /* eslint-enable mozilla/balanced-listeners */
    } else if (topic == "inner-window-destroyed") {
      let windowId = subject.QueryInterface(Ci.nsISupportsPRUint64).data;

      // Close any existent content-script context for the destroyed window.
      if (this.contentScriptWindows.has(windowId)) {
        let extensions = this.contentScriptWindows.get(windowId);
        for (let [, context] of extensions) {
          context.close();
        }

        this.contentScriptWindows.delete(windowId);
      }

      // Close any existent iframe extension page context for the destroyed window.
      if (this.extensionPageWindows.has(windowId)) {
        let context = this.extensionWindows.get(windowId);
        context.close();
        this.extensionPageWindows.delete(windowId);
      }
    }
  },

  handleEvent: function(event) {
    let window = event.currentTarget;
    if (event.target != window.document) {
      // We use capturing listeners so we have precedence over content script
      // listeners, but only care about events targeted to the element we're
      // listening on.
      return;
    }
    window.removeEventListener(event.type, this, true);

    // Need to check if we're still on the right page? Greasemonkey does this.

    if (event.type == "DOMContentLoaded") {
      this.trigger("document_end", window);
    } else if (event.type == "load") {
      this.trigger("document_idle", window);
    }
  },

  executeScript(global, extensionId, script) {
    let window = global.content;
    let context = this.getContentScriptContext(extensionId, window);
    if (!context) {
      return;
    }

    // TODO: Somehow make sure we have the right permissions for this origin!

    // FIXME: Script should be executed only if current state has
    // already reached its run_at state, or we have to keep it around
    // somewhere to execute later.
    context.execute(script, scheduled => true);
  },

  enumerateWindows: function*(docShell) {
    let window = docShell.QueryInterface(Ci.nsIInterfaceRequestor)
                         .getInterface(Ci.nsIDOMWindow);
    yield [window, this.getWindowState(window)];

    for (let i = 0; i < docShell.childCount; i++) {
      let child = docShell.getChildAt(i).QueryInterface(Ci.nsIDocShell);
      yield* this.enumerateWindows(child);
    }
  },

  getContentScriptContext(extensionId, window) {
    let winId = windowId(window);
    if (!this.contentScriptWindows.has(winId)) {
      this.contentScriptWindows.set(winId, new Map());
    }

    let extensions = this.contentScriptWindows.get(winId);
    if (!extensions.has(extensionId)) {
      let context = new ExtensionContext(extensionId, window);
      extensions.set(extensionId, context);
    }

    return extensions.get(extensionId);
  },

  getExtensionPageContext(extensionId, window) {
    let winId = windowId(window);

    let context = this.extensionPageWindows.get(winId);
    if (!context) {
      let context = new ExtensionContext(extensionId, window, { isExtensionPage: true });
      this.extensionPageWindows.set(winId, context);
    }

    return context;
  },

  startupExtension(extensionId) {
    if (this.extensionCount == 0) {
      this.init();
    }
    this.extensionCount++;

    let extension = ExtensionManager.get(extensionId);
    for (let global of ExtensionContent.globals.keys()) {
      // Note that we miss windows in the bfcache here. In theory we
      // could execute content scripts on a pageshow event for that
      // window, but that seems extreme.
      for (let [window, state] of this.enumerateWindows(global.docShell)) {
        for (let script of extension.scripts) {
          if (script.matches(window)) {
            let context = this.getContentScriptContext(extensionId, window);
            context.execute(script, scheduled => isWhenBeforeOrSame(scheduled, state));
          }
        }
      }
    }
  },

  shutdownExtension(extensionId) {
    // Clean up content-script contexts on extension shutdown.
    for (let [, extensions] of this.contentScriptWindows) {
      let context = extensions.get(extensionId);
      if (context) {
        context.close();
        extensions.delete(extensionId);
      }
    }

    // Clean up iframe extension page contexts on extension shutdown.
    for (let [winId, context] of this.extensionPageWindows) {
      if (context.extensionId == extensionId) {
        context.close();
        this.extensionPageWindows.delete(winId);
      }
    }

    this.extensionCount--;
    if (this.extensionCount == 0) {
      this.uninit();
    }
  },

  trigger(when, window) {
    let state = this.getWindowState(window);
    for (let [extensionId, extension] of ExtensionManager.extensions) {
      for (let script of extension.scripts) {
        if (script.matches(window)) {
          let context = this.getContentScriptContext(extensionId, window);
          context.execute(script, scheduled => scheduled == state);
        }
      }
    }
  },
};

// Represents a browser extension in the content process.
function BrowserExtensionContent(data) {
  this.id = data.id;
  this.uuid = data.uuid;
  this.data = data;
  this.scripts = data.content_scripts.map(scriptData => new Script(scriptData));
  this.webAccessibleResources = data.webAccessibleResources;
  this.whiteListedHosts = new MatchPattern(data.whiteListedHosts);

  this.localeData = new LocaleData(data.localeData);

  this.manifest = data.manifest;
  this.baseURI = Services.io.newURI(data.baseURL, null, null);

  let uri = Services.io.newURI(data.resourceURL, null, null);

  if (Services.appinfo.processType == Services.appinfo.PROCESS_TYPE_CONTENT) {
    // Extension.jsm takes care of this in the parent.
    ExtensionManagement.startupExtension(this.uuid, uri, this);
  }
}

BrowserExtensionContent.prototype = {
  shutdown() {
    if (Services.appinfo.processType == Services.appinfo.PROCESS_TYPE_CONTENT) {
      ExtensionManagement.shutdownExtension(this.uuid);
    }
  },

  localizeMessage(...args) {
    return this.localeData.localizeMessage(...args);
  },

  localize(...args) {
    return this.localeData.localize(...args);
  },
};

ExtensionManager = {
  // Map[extensionId, BrowserExtensionContent]
  extensions: new Map(),

  init() {
    Services.cpmm.addMessageListener("Extension:Startup", this);
    Services.cpmm.addMessageListener("Extension:Shutdown", this);
    Services.cpmm.addMessageListener("Extension:FlushJarCache", this);

    if (Services.cpmm.initialProcessData && "Extension:Extensions" in Services.cpmm.initialProcessData) {
      let extensions = Services.cpmm.initialProcessData["Extension:Extensions"];
      for (let data of extensions) {
        this.extensions.set(data.id, new BrowserExtensionContent(data));
        DocumentManager.startupExtension(data.id);
      }
    }
  },

  get(extensionId) {
    return this.extensions.get(extensionId);
  },

  receiveMessage({name, data}) {
    let extension;
    switch (name) {
      case "Extension:Startup": {
        extension = new BrowserExtensionContent(data);
        this.extensions.set(data.id, extension);
        DocumentManager.startupExtension(data.id);
        Services.cpmm.sendAsyncMessage("Extension:StartupComplete");
        break;
      }

      case "Extension:Shutdown": {
        extension = this.extensions.get(data.id);
        extension.shutdown();
        DocumentManager.shutdownExtension(data.id);
        this.extensions.delete(data.id);
        break;
      }

      case "Extension:FlushJarCache": {
        let nsIFile = Components.Constructor("@mozilla.org/file/local;1", "nsIFile",
                                             "initWithPath");
        let file = new nsIFile(data.path);
        flushJarCache(file);
        Services.cpmm.sendAsyncMessage("Extension:FlushJarCacheComplete");
        break;
      }
    }
  },
};

this.ExtensionContent = {
  globals: new Map(),

  init(global) {
    let broker = new MessageBroker([global]);
    this.globals.set(global, broker);

    global.addMessageListener("Extension:Execute", this);

    let windowId = global.content
                         .QueryInterface(Ci.nsIInterfaceRequestor)
                         .getInterface(Ci.nsIDOMWindowUtils)
                         .outerWindowID;
    global.sendAsyncMessage("Extension:TopWindowID", {windowId});
  },

  uninit(global) {
    this.globals.delete(global);

    let windowId = global.content
                         .QueryInterface(Ci.nsIInterfaceRequestor)
                         .getInterface(Ci.nsIDOMWindowUtils)
                         .outerWindowID;
    global.sendAsyncMessage("Extension:RemoveTopWindowID", {windowId});
  },

  getBroker(messageManager) {
    return this.globals.get(messageManager);
  },

  receiveMessage({target, name, data}) {
    switch (name) {
      case "Extension:Execute":
        let script = new Script(data.options);
        let {extensionId} = data;
        DocumentManager.executeScript(target, extensionId, script);
        break;
    }
  },
};

ExtensionManager.init();
