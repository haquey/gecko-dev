/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
"use strict";

const { Ci } = require("chrome");
const { rootSpec } = require("devtools/shared/specs/root");
const {
  FrontClassWithSpec,
  registerFront,
} = require("devtools/shared/protocol");

loader.lazyRequireGetter(this, "getFront", "devtools/shared/protocol", true);
loader.lazyRequireGetter(
  this,
  "TabDescriptorFront",
  "devtools/shared/fronts/descriptors/tab",
  true
);
loader.lazyRequireGetter(
  this,
  "BrowsingContextTargetFront",
  "devtools/shared/fronts/targets/browsing-context",
  true
);
loader.lazyRequireGetter(
  this,
  "LocalTabTargetFront",
  "devtools/shared/fronts/targets/local-tab",
  true
);

class RootFront extends FrontClassWithSpec(rootSpec) {
  constructor(client, form) {
    super(client);

    // Root Front is a special Front. It is the only one to set its actor ID manually
    // out of the form object returned by RootActor.sayHello which is called when calling
    // DevToolsClient.connect().
    this.actorID = form.from;

    this.applicationType = form.applicationType;
    this.traits = form.traits;

    // Cache root form as this will always be the same value.
    Object.defineProperty(this, "rootForm", {
      get() {
        delete this.rootForm;
        this.rootForm = this.getRoot();
        return this.rootForm;
      },
      configurable: true,
    });

    // Cache of already created global scoped fronts
    // [typeName:string => Front instance]
    this.fronts = new Map();

    this._client = client;
  }

  /**
   * Retrieve all service worker registrations as well as workers from the parent and
   * content processes. Listing service workers involves merging information coming from
   * registrations and workers, this method will combine this information to present a
   * unified array of serviceWorkers. If you are only interested in other workers, use
   * listWorkers.
   *
   * @return {Object}
   *         - {Array} service
   *           array of form-like objects for serviceworkers
   *         - {Array} shared
   *           Array of WorkerTargetActor forms, containing shared workers.
   *         - {Array} other
   *           Array of WorkerTargetActor forms, containing other workers.
   */
  async listAllWorkers() {
    let registrations = [];
    let workers = [];

    try {
      // List service worker registrations
      ({ registrations } = await this.listServiceWorkerRegistrations());

      workers = await this.listAllWorkerTargets();
    } catch (e) {
      // Something went wrong, maybe our client is disconnected?
    }

    const result = {
      service: [],
      shared: [],
      other: [],
    };

    registrations.forEach(front => {
      const {
        activeWorker,
        waitingWorker,
        installingWorker,
        evaluatingWorker,
      } = front;
      const newestWorker =
        activeWorker || waitingWorker || installingWorker || evaluatingWorker;

      // All the information is simply mirrored from the registration front.
      // However since registering workers will fetch similar information from the worker
      // target front and will not have a service worker registration front, consumers
      // should not read meta data directly on the registration front instance.
      result.service.push({
        active: front.active,
        fetch: front.fetch,
        id: front.id,
        lastUpdateTime: front.lastUpdateTime,
        name: front.url,
        registrationFront: front,
        scope: front.scope,
        url: front.url,
        newestWorkerId: newestWorker && newestWorker.id,
      });
    });

    workers.forEach(front => {
      const worker = {
        id: front.id,
        name: front.url,
        url: front.url,
        workerTargetFront: front,
      };
      switch (front.type) {
        case Ci.nsIWorkerDebugger.TYPE_SERVICE:
          const registration = result.service.find(r => {
            // If registrationFront is missing, it means this entry is actually
            // a workerFront that has been augmented and pushed to
            // result.service in an earlier iteration.
            // This should no longer happen after Bug 1595964 is resolved.
            if (!r.registrationFront) {
              // We can safely return false here since `r` is not a full
              // service worker registration, but merely a worker.
              return false;
            }

            /**
             * Older servers will not define `ServiceWorkerFront.id` (the value
             * of `r.newestWorkerId`), and a `ServiceWorkerFront`'s ID will only
             * match its corresponding WorkerTargetFront's ID if their
             * underlying actors are "connected" - this is only guaranteed with
             * parent-intercept mode. The `if` statement is for backward
             * compatibility and can be removed when the release channel is
             * >= FF69 _and_ parent-intercept is stable (which definitely won't
             * happen when the release channel is < FF69).
             */
            const { isParentInterceptEnabled } = r.registrationFront.traits;
            if (!r.newestWorkerId || !isParentInterceptEnabled) {
              return r.scope === front.scope;
            }

            return r.newestWorkerId === front.id;
          });

          if (registration) {
            // Before bug 1595964, URLs were not available for registrations
            // whose worker's main script is being evaluated. Now, URLs are
            // always available, and this test deals with older servers.
            // @backward-compatibility: remove in Firefox 75
            if (!registration.url) {
              registration.name = registration.url = front.url;
            }
            registration.workerTargetFront = front;
          } else {
            // If we are missing the registration, augment the worker front with
            // fields expected on service worker registration fronts so that it
            // can be displayed in UIs handling on service worker registrations.

            // When does this happen:
            // A - If parent intercept is disabled:
            //   If a service worker registration could not be found, this means we are in
            //   e10s, and registrations are not forwarded to other processes until they
            //   reach the activated state.
            // B - If parent intercept is enabled:
            //   This can apparently happen when the registration is currently
            //   in evaluating state. Cleanup is tracked in Bug 1595964.
            worker.fetch = front.fetch;
            worker.scope = front.scope;
            worker.active = false;
            result.service.push(worker);
          }
          break;
        case Ci.nsIWorkerDebugger.TYPE_SHARED:
          result.shared.push(worker);
          break;
        default:
          result.other.push(worker);
      }
    });

    return result;
  }

  /** Get the target fronts for all worker threads running in any process. */
  async listAllWorkerTargets() {
    // List workers from the Parent process
    let { workers } = await this.listWorkers();

    // And then from the Child processes
    const processes = await this.listProcesses();
    for (const processDescriptorFront of processes) {
      // Ignore parent process
      if (processDescriptorFront.isParent) {
        continue;
      }
      const front = await processDescriptorFront.getTarget();
      if (front) {
        const response = await front.listWorkers();
        workers = workers.concat(response.workers);
      }
    }

    return workers;
  }

  /**
   * Fetch the ProcessDescriptorFront for the main process.
   *
   * `getProcess` requests allows to fetch the descriptor for any process and
   * the main process is having the process ID zero.
   */
  getMainProcess() {
    return this.getProcess(0);
  }

  /**
   * Retrieve the target descriptor for the provided id.
   *
   * @return {ProcessDescriptorFront} the process descriptor front for the
   *         provided id.
   */
  async getProcess(id) {
    const { form, processDescriptor } = await super.getProcess(id);
    // Backward compatibility: FF74 or older servers will return the
    // process descriptor as the "form" property of the response.
    // Once FF75 is merged to release we can always expect `processDescriptor`
    // to be defined.
    return processDescriptor || form;
  }

  /**
   * Override default listTabs request in order to return a list of
   * BrowsingContextTargetFronts while updating their selected state.
   */
  async listTabs(options) {
    const { selected, tabs } = await super.listTabs(options);
    const targets = [];
    for (const i in tabs) {
      if (!this.actorID) {
        console.error("The root front was destroyed while processing listTabs");
        return [];
      }

      try {
        const form = tabs[i];
        const target = await this._createTargetFrontForTabForm(form);
        target.setIsSelected(i == selected);
        targets.push(target);
      } catch (e) {
        console.error("Failed to get the target for tab descriptor", e);
      }
    }
    return targets;
  }

  /**
   * Fetch the target actor for the currently selected tab, or for a specific
   * tab given as first parameter.
   *
   * @param [optional] object filter
   *        A dictionary object with following optional attributes:
   *         - outerWindowID: used to match tabs in parent process
   *         - tabId: used to match tabs in child processes
   *         - tab: a reference to xul:tab element
   *        If nothing is specified, returns the actor for the currently
   *        selected tab.
   */
  async getTab(filter) {
    const packet = {};
    if (filter) {
      if (typeof filter.outerWindowID == "number") {
        packet.outerWindowID = filter.outerWindowID;
      } else if (typeof filter.tabId == "number") {
        packet.tabId = filter.tabId;
      } else if ("tab" in filter) {
        const browser = filter.tab.linkedBrowser;
        if (browser.frameLoader.remoteTab) {
          // Tabs in child process
          packet.tabId = browser.frameLoader.remoteTab.tabId;
        } else if (browser.outerWindowID) {
          // <xul:browser> tabs in parent process
          packet.outerWindowID = browser.outerWindowID;
        } else {
          // <iframe mozbrowser> tabs in parent process
          const windowUtils = browser.contentWindow.windowUtils;
          packet.outerWindowID = windowUtils.outerWindowID;
        }
      } else {
        // Throw if a filter object have been passed but without
        // any clearly idenfified filter.
        throw new Error("Unsupported argument given to getTab request");
      }
    }

    const form = await super.getTab(packet);
    return this._createTargetFrontForTabForm(form, filter);
  }

  async _createTargetFrontForTabForm(form, filter = {}) {
    let front = this.actor(form.actor);
    if (front) {
      if (!form.actor.includes("tabDescriptor")) {
        // Backwards compatibility for servers FF74 and older.
        front.form(form);
        return front;
      }

      return front.getTarget();
    }

    if (!form.actor.includes("tabDescriptor")) {
      // Backwards compatibility for servers FF74 and older.

      // Instanciate a specialized class for a local tab as it needs some more
      // client side integration with the Firefox frontend.
      // But ignore the fake `tab` object we receive, where there is only a
      // `linkedBrowser` attribute, but this isn't a real <tab> element.
      // devtools/client/framework/test/browser_toolbox_target.js is passing such
      // a fake tab.
      if (filter && filter.tab && filter.tab.tagName == "tab") {
        front = new LocalTabTargetFront(this._client, null, this, filter.tab);
      } else {
        front = new BrowsingContextTargetFront(this._client, null, this);
      }
      // As these fronts aren't instantiated by protocol.js, we have to set their actor ID
      // manually like that:
      front.actorID = form.actor;
      front.form(form);
      this.manage(front);
      return front;
    }

    const descriptorFront = new TabDescriptorFront(this._client, null, this);
    // As these fronts aren't instantiated by protocol.js, we have to set their actor ID
    // manually like that:
    descriptorFront.actorID = form.actor;
    descriptorFront.form(form);
    this.manage(descriptorFront);
    front = await descriptorFront.getTarget(filter);
    return front;
  }

  /**
   * Fetch the target front for a given add-on.
   * This is just an helper on top of `listAddons` request.
   *
   * @param object filter
   *        A dictionary object with following attribute:
   *         - id: used to match the add-on to connect to.
   */
  async getAddon({ id }) {
    const addons = await this.listAddons();
    const webextensionDescriptorFront = addons.find(addon => addon.id === id);
    return webextensionDescriptorFront;
  }

  /**
   * Fetch the target front for a given worker.
   * This is just an helper on top of `listAllWorkers` request.
   *
   * @param id
   */
  async getWorker(id) {
    const { service, shared, other } = await this.listAllWorkers();
    const worker = [...service, ...shared, ...other].find(w => w.id === id);
    if (!worker) {
      return null;
    }
    return worker.workerTargetFront || worker.registrationFront;
  }

  /**
   * Test request that returns the object passed as first argument.
   *
   * `echo` is special as all the property of the given object have to be passed
   * on the packet object. That's not something that can be achieve by requester helper.
   */

  echo(packet) {
    packet.type = "echo";
    return this.request(packet);
  }

  /*
   * This function returns a protocol.js Front for any root actor.
   * i.e. the one directly served from RootActor.listTabs or getRoot.
   *
   * @param String typeName
   *        The type name used in protocol.js's spec for this actor.
   */
  async getFront(typeName) {
    let front = this.fronts.get(typeName);
    if (front) {
      return front;
    }
    const rootForm = await this.rootForm;
    front = getFront(this._client, typeName, rootForm);
    this.fronts.set(typeName, front);
    return front;
  }

  /*
   * This function returns true if the root actor has a registered global actor
   * with a given name.
   * @param {String} actorName
   *        The name of a global actor.
   *
   * @return {Boolean}
   */
  async hasActor(actorName) {
    const rootForm = await this.rootForm;
    return !!rootForm[actorName + "Actor"];
  }
}
exports.RootFront = RootFront;
registerFront(RootFront);
