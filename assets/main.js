/* main.js - the Ghost Tooth UI.
 *
 * Talks only to /api/state (polled) and /api/action (one-shot).  Every string
 * shown here is either a label or a value that ghost-toothAPI itself produced,
 * so nothing in the interface can claim a state the payload did not report.
 *
 * Copyright (C) 2026 Ghost-tooth-ui contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

(function () {
  "use strict";

  var POLL_MS = 1500;
  var state = null;
  var pollTimer = 0;
  var busyCount = 0;
  var lang = localStorage.getItem("gtt.lang") || "en";

  /* ------------------------------------------------------------------ i18n */
  var STRINGS = {
    en: {
      title: "Ghost Tooth",
      subtitle: "Bluetooth headset audio for PS5",
      payload: "Payload", running: "running", stopped: "stopped",
      link: "Headset", selection: "Selection",
      devices: "Bluetooth devices", scan: "Scan for devices",
      scanning: "Scanning…",
      devicesHint: "The list is what ghost-toothAPI's own inquiry pass heard. " +
                   "Put the headset in pairing mode, then scan.",
      manual: "Connect an address by hand", connect: "Connect",
      manualHint: "Any address from the log works, even one that is not in the list.",
      status: "Status", log: "Console log", logHint: "live",
      apply: "Restart payload", stop: "Stop payload", auto: "Let it pick",
      forget: "Forget pairing", help: "Help", close: "Close", working: "Working…",
      helpTitle: "Using Ghost Tooth",
      empty: "No devices yet. Press Scan - the headset must be in pairing mode.",
      autoMode: "automatic", nameMode: "name: ", addrMode: "pinned ",
      connected: "connected", selected: "selected", ignoredTag: "skipped",
      pairedTag: "paired", unnamed: "unnamed device",
      tvHint: "TVs and speakers are skipped by the payload.",
      noLoader: "No payload loader answered, so the choice was saved but not " +
                "started. Start ghost-toothAPI.elf from your loader.",
      tileMissing: "The Media tile could not be registered: ",
      helpSteps: [
        "Load ghost-tooth-ui.elf once through your payload loader. It starts a " +
        "local web server and puts a “Ghost Tooth” tile on the Media tab.",
        "From now on you open that tile - the payload loader is not needed again " +
        "until the console reboots.",
        "Put the headset in pairing mode and press Scan for devices. The scan is " +
        "the payload's own inquiry pass, so nothing else is installed.",
        "Pick a device and press Connect. The address is written to " +
        "/data/ghost-toothAPI/headset.ini and ghost-toothAPI is restarted, which " +
        "pages exactly that headset.",
        "Status and the console log below come from the payload's log file, so " +
        "you see the connection attempt as it happens."
      ],
      keys: [["D-pad", "Move focus"], ["✕ / Enter", "Activate"],
             ["△", "Scan"], ["○", "Stop payload"],
             ["Backspace", "Close dialog"], ["F5", "Refresh"]],
      st: { idle: "idle", scanning: "scanning", connecting: "connecting",
            streaming: "streaming audio", failed: "failed", disconnected: "disconnected" },
      fields: {
        state: "Link", codec: "Audio", bitpool: "Bitpool", packets: "Packets",
        queue: "Buffer", pid: "PID", ini: "Config file", bond: "Link key",
        peer: "Paired with", tile: "Media tile", port: "Port", detail: "Last event"
      }
    },
    fa: {
      title: "Ghost Tooth",
      subtitle: "صدای هندزفری بلوتوثی روی PS5",
      payload: "پیلود", running: "در حال اجرا", stopped: "متوقف",
      link: "هندزفری", selection: "انتخاب شما",
      devices: "دستگاه‌های بلوتوثی", scan: "جست‌وجوی دستگاه‌ها",
      scanning: "در حال جست‌وجو…",
      devicesHint: "این فهرست همان چیزی است که خودِ ghost-toothAPI در مرحله " +
                   "اسکن شنیده است. هندزفری را حالت pairing بگذارید، بعد Scan را بزنید.",
      manual: "اتصال دستی با آدرس", connect: "اتصال",
      manualHint: "هر آدرسی که در لاگ هست کار می‌کند، حتی اگر در فهرست نباشد.",
      status: "وضعیت", log: "لاگ کنسول", logHint: "زنده",
      apply: "شروع دوباره پیلود", stop: "توقف پیلود", auto: "خودش انتخاب کند",
      forget: "پاک کردن جفت‌شدن", help: "راهنما", close: "بستن", working: "در حال انجام…",
      helpTitle: "راهنمای Ghost Tooth",
      empty: "هنوز دستگاهی پیدا نشده. هندزفری را حالت pairing بگذارید و Scan را بزنید.",
      autoMode: "خودکار", nameMode: "با نام: ", addrMode: "ثابت‌شده ",
      connected: "متصل", selected: "انتخاب‌شده", ignoredTag: "ردشده",
      pairedTag: "جفت‌شده", unnamed: "بدون نام",
      tvHint: "تلویزیون و اسپیکر توسط پیلود رد می‌شوند.",
      noLoader: "لودر پیلود جواب نداد؛ انتخاب ذخیره شد ولی پیلود شروع نشد. " +
                "فایل ghost-toothAPI.elf را از لودر خودتان اجرا کنید.",
      tileMissing: "کاشی Media ثبت نشد: ",
      helpSteps: [
        "یک بار ghost-tooth-ui.elf را با لودر اجرا کنید. این پیلود یک وب‌سرور " +
        "محلی روشن می‌کند و کاشی «Ghost Tooth» را به تب Media اضافه می‌کند.",
        "از این به بعد همان کاشی را باز کنید؛ تا ریبوت کنسول لازم نیست دوباره " +
        "پیلود اجرا کنید.",
        "هندزفری را حالت pairing بگذارید و Scan for devices را بزنید. اسکن همان " +
        "مراحل خودِ پیلود است، چیز اضافه‌ای نصب نمی‌شود.",
        "دستگاه را انتخاب و Connect را بزنید. آدرس در " +
        "/data/ghost-toothAPI/headset.ini نوشته می‌شود و ghost-toothAPI دوباره " +
        "اجرا می‌شود و دقیقاً همان هندزفری را page می‌کند.",
        "وضعیت و لاگ پایین از فایل لاگ خود پیلود می‌آید، پس تلاش اتصال را زنده " +
        "می‌بینید."
      ],
      keys: [["جهت‌نما", "حرکت بین گزینه‌ها"], ["✕", "انتخاب"],
             ["△", "اسکن"], ["○", "توقف پیلود"],
             ["Backspace", "بستن پنجره"], ["F5", "به‌روزرسانی"]],
      st: { idle: "بی‌کار", scanning: "در حال اسکن", connecting: "در حال اتصال",
            streaming: "پخش صدا", failed: "ناموفق", disconnected: "قطع شد" },
      fields: {
        state: "لینک", codec: "صدا", bitpool: "Bitpool", packets: "بسته‌ها",
        queue: "بافر", pid: "PID", ini: "فایل تنظیمات", bond: "کلید جفت",
        peer: "جفت‌شده با", tile: "کاشی Media", port: "پورت", detail: "آخرین رویداد"
      }
    }
  };

  function t(key) {
    var table = STRINGS[lang] || STRINGS.en;
    return key in table ? table[key] : (STRINGS.en[key] !== undefined ? STRINGS.en[key] : key);
  }

  function el(tag, cls, text) {
    var n = document.createElement(tag);
    if (cls) n.className = cls;
    if (text !== undefined && text !== null) n.textContent = text;
    return n;
  }
  function $(id) { return document.getElementById(id); }

  /* ------------------------------------------------------------------ fetch */
  function request(url, opts) {
    return fetch(url, opts).then(function (r) {
      if (!r.ok) throw new Error("http " + r.status);
      return r.json();
    });
  }

  function action(name, arg) {
    var body = JSON.stringify({ action: name, arg: arg || "" });
    setBusy(true);
    return request("/api/action", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: body
    }).then(function (res) {
      schedulePoll(400);
      return res;
    }).catch(function (e) {
      showBanner(String(e && e.message || e), true);
      return { ok: false };
    }).then(function (res) { setBusy(false); return res; },
             function (e) { setBusy(false); throw e; });
  }

  var pending = 0;
  function setBusy(on) {
    pending = Math.max(0, pending + (on ? 1 : -1));
    $("busy").hidden = pending === 0;
  }

  /* ---------------------------------------------------------------- polling */
  function poll() {
    return request("/api/state").then(function (data) {
      state = data;
      render();
    }).catch(function () {
      showBanner("ghost-tooth-ui is not answering (is the payload still running?)", true);
    });
  }

  function schedulePoll(delay) {
    if (pollTimer) clearTimeout(pollTimer);
    pollTimer = setTimeout(function () { pollTimer = 0; poll().then(loop); },
                            delay || 300);
  }

  /* A poll while an operation runs has to come back quickly - the payload is
   * being restarted at that moment and every line of its log matters.  When the
   * headset is streaming the counters move, and when nothing runs there is
   * nothing to read, so the console can idle. */
  function nextDelay() {
    if (state && state.op && (state.op.busy || state.op.queued)) return 400;
    if (state && state.payload && state.payload.running) return 1000;
    if (state && (state.devices || []).length === 0) return 3000;
    return POLL_MS;
  }

  function loop() {
    if (pollTimer) return;
    pollTimer = setTimeout(function () {
      pollTimer = 0;
      poll().then(loop);
    }, nextDelay());
  }

  /* ------------------------------------------------------------------ render */
  var lastSig = "";

  function sig(o) { return o ? JSON.stringify(o) : ""; }

  function render() {
    if (!state) return;
    var s = sig([state.link, state.config, state.payload, state.scan, state.bond,
                state.tile, state.op, state.devices.length,
                state.log.lines.length]);
    if (s === lastSig) return;
    lastSig = s;

    renderPills();
    renderDevices();
    renderStatus();
    renderLog();
    renderBanner();
  }

  function pill(id, cls, text) {
    var node = $(id);
    node.className = "pill" + (cls ? " " + cls : "");
    node.querySelector("em").textContent = text;
  }

  function renderPills() {
    var p = state.payload || {}, l = state.link || {}, c = state.config || {};
    pill("pill-payload", p.running ? "on" : "", p.running ?
      t("running") + (p.pid ? " · " + p.pid : "") : t("stopped"));

    var linkCls = l.state === "streaming" ? "on" : (l.state === "failed" ? "err" :
                  (l.state === "idle" || l.state === "disconnected" ? "" : "live"));
    var linkText = (state.scan && state.scan.active) ? t("scanning") :
                   (t("st")[l.state] || l.state);
    if (l.name) linkText += " · " + l.name;
    pill("pill-link", linkCls, linkText);

    var pick = c.mode === "address" ? t("addrMode") + c.address :
               c.mode === "name" ? t("nameMode") + c.name : t("autoMode");
    pill("pill-pick", c.mode === "auto" ? "" : "acc", pick);
    $("btn-scan").disabled = !!(state.scan && state.scan.active) ||
                              !!(state.op && (state.op.busy || state.op.queued));
    if (state.target === "host") {
      /* the desktop preview runs a mock payload; say so on screen so nobody
       * mistakes a green light here for a Bluetooth link */
      mockPill("mock payload - this is not the console");
    }
    document.title = t("title") + (l.state === "streaming" ? " · " + (l.name || "") : "");
  }

  var mockPillNode = null;
  function mockPill(text) {
    if (!mockPillNode) {
      mockPillNode = el("div", "pill warn");
      mockPillNode.appendChild(el("span", "dot"));
      mockPillNode.appendChild(el("em", null, text));
      $("pills").appendChild(mockPillNode);
    }
    mockPillNode.querySelector("em").textContent = text;
  }

  function signalBars(rssi) {
    var wrap = el("span", "signal");
    var level = 0;
    if (rssi && rssi < 0) {
      level = rssi > -55 ? 4 : rssi > -67 ? 3 : rssi > -78 ? 2 : 1;
    }
    for (var i = 1; i <= 4; i++) wrap.appendChild(el("i", i <= level ? "on" : ""));
    return wrap;
  }

  function renderDevices() {
    var list = $("device-list");
    var devices = state.devices || [];
    list.textContent = "";
    $("device-count").textContent = devices.length
      ? devices.length + (devices.length === 1 ? " device" : " devices") : "";

    if (!devices.length) {
      var empty = el("li", "empty", (state.scan && state.scan.note) || t("empty"));
      list.appendChild(empty);
      return;
    }

    devices.forEach(function (d) {
      var link = state.link || {};
      var cls = "card";
      if (d.connected) cls += " connected";
      else if (d.pinned) cls += " selected";
      if (d.ignored && !d.pinned) cls += " ignored";

      var li = el("li", cls);
      li.tabIndex = 0;
      li.setAttribute("role", "option");
      li.setAttribute("aria-selected", d.pinned ? "true" : "false");
      li.dataset.addr = d.addr;

      var box = el("div");
      box.appendChild(el("div", "name", d.name || t("unnamed")));
      box.appendChild(el("div", "addr", d.addr));

      var meta = el("div", "meta");
      meta.appendChild(signalBars(d.rssi));
      if (d.rssi) meta.appendChild(el("span", "badge", d.rssi + " dBm"));
      if (d.class) meta.appendChild(el("span", "badge", "class " + d.class));
      if (d.kind && d.kind !== "unknown") meta.appendChild(el("span", "badge", d.kind));
      if (d.ignored) meta.appendChild(el("span", "badge warn", t("ignoredTag")));
      if (d.paired) meta.appendChild(el("span", "badge ok", t("pairedTag")));
      if (d.connected) meta.appendChild(el("span", "badge ok", t("connected")));
      else if (d.pinned) meta.appendChild(el("span", "badge acc", t("selected")));
      if (typeof d.score === "number" && d.score >= 0) {
        meta.appendChild(el("span", "badge", "score " + d.score));
      }
      if (d.seenAt) meta.appendChild(el("span", "badge", d.seenAt));
      box.appendChild(meta);
      li.appendChild(box);

      var btn = el("button", d.connected ? "do" : "primary do",
                   d.connected ? "✓ " + (link.state === "streaming" ? t("st").streaming : t("connected"))
                               : t("connect"));
      btn.tabIndex = -1;
      btn.addEventListener("click", function (ev) {
        ev.stopPropagation();
        connect(d);
      });
      li.appendChild(btn);

      li.addEventListener("click", function () { connect(d); });
      li.addEventListener("keydown", function (ev) {
        if (ev.key === "Enter" || ev.key === " " || ev.key === "Cross") {
          ev.preventDefault();
          connect(d);
        }
      });
      list.appendChild(li);
    });
  }

  function connect(d) {
    showBanner((lang === "fa" ? "اتصال به " : "Connecting ") + (d.name || d.addr) +
               "…");
    action("connect", d.addr);
  }

  function kv(rows) {
    var dl = $("status-grid");
    dl.textContent = "";
    rows.forEach(function (r) {
      if (r[1] === null || r[1] === undefined || r[1] === "") return;
      dl.appendChild(el("dt", null, r[0]));
      dl.appendChild(el("dd", r[2] ? r[2] : null, String(r[1])));
    });
  }

  function renderStatus() {
    var f = t("fields");
    var l = state.link || {}, p = state.payload || {}, b = state.bond || {};
    var tile = state.tile || {}, srv = state.server || {};
    var rows = [
      [f.state, t("st")[l.state] || l.state],
      [f.codec, l.codec],
      [f.bitpool, l.bitpool],
      [f.packets, l.packets],
      [f.queue, l.queueMs ? l.queueMs + " ms" : null],
      [t("payload"), p.running ? t("running") : t("stopped")],
      [f.pid, p.pid],
      [f.tile, tile.installed ? "✓ " + tile.titleId : (t("tileMissing") + (tile.error || "-"))],
      [f.port, srv.port],
      [f.bond, b.keySaved ? "✓" : "—"],
      [f.peer, b.peerName],
      [f.detail, l.detail]
    ];
    if (l.error) rows.push(["⚠", l.error, null]);
    if (l.state === "failed" || l.state === "disconnected") rows.push([f.state, l.state]);
    kv(rows);
    var badge = $("op-badge");
    if (state.op && (state.op.busy || state.op.queued)) {
      badge.hidden = false;
      badge.textContent = (state.op.name || "…") + ": " + (state.op.message || "");
      badge.className = "badge acc";
    } else if (state.op && state.op.status) {
      badge.hidden = false;
      badge.textContent = state.op.name + ": " + (state.op.message || "failed");
      badge.className = "badge err";
    } else {
      badge.hidden = true;
    }
  }

  function renderLog() {
    var pre = $("log");
    var lines = (state.log && state.log.lines) || [];
    var stick = pre.scrollTop + pre.clientHeight >= pre.scrollHeight - 24;
    pre.textContent = "";
    lines.slice(-90).forEach(function (line) {
      var cls = "l-ui";
      if (/could not|failed|refused|no headphones|busy|error|not found/i.test(line.text)) cls = "l-err";
      else if (/audio |streaming|connected|paired|found|ready/i.test(line.text)) cls = "l-ok";
      else if (line.text.indexOf("ui:") === 0) cls = "l-ui";
      else cls = "";
      var row = el("div", cls);
      row.appendChild(el("span", "t", line.t + " "));
      row.appendChild(document.createTextNode(line.text));
      pre.appendChild(row);
    });
    if (stick) pre.scrollTop = pre.scrollHeight;
  }

  var bannerTimer = 0;
  function showBanner(text, isError) {
    var b = $("banner");
    b.hidden = false;
    b.className = "banner" + (isError ? " error" : "");
    $("banner-text").textContent = text || "";
    if (!isError) {
      clearTimeout(bannerTimer);
      bannerTimer = setTimeout(hideBanner, 12000);
    }
  }
  function hideBanner() { $("banner").hidden = true; }
  function renderBanner() {
    if (!state) return;
    var p = state.payload || {};
    if (p.loaderReady === false && !p.running && $("banner").hidden) {
      showBanner(t("noLoader"), true);
      return;
    }
    var tile = state.tile || {};
    if (state.server && !state.server.demoMode && !tile.installed && $("banner").hidden) {
      showBanner(t("tileMissing") + (tile.error || "unknown"), true);
    }
  }

  /* ----------------------------------------------------------------- i18n UI */
  function applyLang() {
    document.documentElement.lang = lang;
    document.documentElement.dir = lang === "fa" ? "rtl" : "ltr";
    var nodes = document.querySelectorAll("[data-i18n]");
    for (var i = 0; i < nodes.length; i++) {
      var key = nodes[i].getAttribute("data-i18n");
      nodes[i].textContent = t(key);
    }
    $("btn-lang").textContent = lang === "fa" ? "English" : "فارسی";

    var steps = $("help-steps");
    steps.textContent = "";
    t("helpSteps").forEach(function (s) { steps.appendChild(el("li", null, s)); });
    var keys = $("help-keys");
    keys.textContent = "";
    t("keys").forEach(function (k) {
      keys.appendChild(el("b", null, k[0]));
      keys.appendChild(el("span", null, k[1]));
    });
    lastSig = "";
    render();
  }

  /* ------------------------------------------------------------ controller nav */
  function focusables() {
    var list = [];
    var nodes = document.querySelectorAll(
      'button:not([disabled]):not([tabindex="-1"]), [tabindex="0"]');
    for (var i = 0; i < nodes.length; i++) {
      var n = nodes[i];
      if (n.offsetParent !== null) list.push(n);
    }
    return list;
  }

  function moveFocus(step) {
    var list = focusables();
    if (!list.length) return;
    var idx = list.indexOf(document.activeElement);
    if (idx < 0) idx = step > 0 ? -1 : 0;
    var next = list[(idx + step + list.length) % list.length];
    next.focus();
    next.scrollIntoView({ block: "center", behavior: "smooth" });
  }

  document.addEventListener("keydown", function (ev) {
    var tag = ev.target && ev.target.tagName;
    /* Typing an address must keep the arrow keys for the caret. */
    if (tag === "INPUT") return;
    if (ev.key === "ArrowDown" || ev.key === "ArrowRight") {
      ev.preventDefault();
      moveFocus(1);
    } else if (ev.key === "ArrowUp" || ev.key === "ArrowLeft") {
      ev.preventDefault();
      moveFocus(-1);
    } else if (ev.key === "F5") {
      ev.preventDefault();
      poll();
    } else if (ev.key === "Backspace" && !document.querySelector("dialog[open]")) {
      ev.preventDefault();
      poll();
    } else if (tag !== "INPUT") {
      if (ev.key === "t" || ev.key === "T") $("btn-scan").click();
      if (ev.key === "o" || ev.key === "O") $("btn-stop").click();
    }
  });

  /* The DualSense reaches the WebKit container as a gamepad as well; map the
   * face buttons so the UI works without an on-screen keyboard. */
  var prevButtons = {};
  function gamepadPump() {
    var pads = navigator.getGamepads ? navigator.getGamepads() : [];
    for (var i = 0; i < pads.length; i++) {
      var p = pads[i];
      if (!p) continue;
      var pressed = function (idx) {
        var b = p.buttons[idx];
        var now = !!(b && (b.pressed || b.value > 0.5));
        var was = prevButtons[i + ":" + idx];
        prevButtons[i + ":" + idx] = now;
        return now && !was;
      };
      if (pressed(0)) { /* ✕ */
        var a = document.activeElement;
        if (a && a.click) a.click();
      }
      if (pressed(1)) $("btn-stop").click();     /* ○ */
      if (pressed(3)) $("btn-scan").click();     /* △ */
      if (pressed(12)) moveFocus(-1);           /* dpad up */
      if (pressed(13)) moveFocus(1);            /* dpad down */
      if (pressed(9)) $("btn-help").click();     /* options */
    }
    requestAnimationFrame(gamepadPump);
  }
  try { requestAnimationFrame(gamepadPump); } catch (e) { /* no gamepad */ }

  /* ---------------------------------------------------------------- wiring */
  $("btn-scan").addEventListener("click", function () {
    showBanner(t("scanning"));
    action("scan");
  });
  $("btn-apply").addEventListener("click", function () { action("apply"); });
  $("btn-stop").addEventListener("click", function () { action("stop"); });
  $("btn-auto").addEventListener("click", function () { action("auto"); });
  $("btn-forget").addEventListener("click", function () { action("forget"); });
  $("banner-close").addEventListener("click", hideBanner);
  $("btn-help").addEventListener("click", function () { $("help").showModal(); });
  $("btn-lang").addEventListener("click", function () {
    lang = lang === "fa" ? "en" : "fa";
    localStorage.setItem("gtt.lang", lang);
    applyLang();
  });

  $("manual-form").addEventListener("submit", function (ev) {
    ev.preventDefault();
    var v = $("manual-addr").value.trim();
    if (!v) return;
    action("connect", v);
  });

  applyLang();
  poll().then(loop);
})();
