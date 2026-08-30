// Headless smoke test for devnotes/tools/padmap.html's script, under jsc.
// A minimal DOM shim, then the real script, then assertions against the real padmap-data.js.

var FAILS = [];
function check(cond, msg) { if (!cond) FAILS.push(msg); }

function Elem(tag) {
  this.tagName = tag; this.children = []; this.className = ""; this._text = "";
  this.dataset = {}; this.style = {}; this.attrs = {}; this.hidden = false;
  this._tbody = null;
}
Elem.prototype.appendChild = function (c) { this.children.push(c); return c; };
Elem.prototype.setAttribute = function (k, v) { this.attrs[k] = v; };
Elem.prototype.getAttribute = function (k) { return this.attrs[k]; };
Elem.prototype.addEventListener = function () {};
Elem.prototype.removeEventListener = function () {};
Elem.prototype.classList = null;
Elem.prototype.querySelector = function (sel) {
  if (sel === "tbody") { if (!this._tbody) this._tbody = new Elem("tbody"); return this._tbody; }
  return new Elem("div");
};
Object.defineProperty(Elem.prototype, "textContent", {
  get: function () { return this._text; },
  set: function (v) { this._text = String(v); this.children = []; }
});
Object.defineProperty(Elem.prototype, "innerHTML", {
  get: function () { return ""; },
  set: function (v) { this.children = []; }
});

function mkclasslist(e) {
  return { add: function () {}, remove: function () {}, contains: function () { return false; } };
}

var registry = {};
var document = {
  documentElement: new Elem("html"),
  createElement: function (t) { var e = new Elem(t); e.classList = mkclasslist(e); return e; },
  createElementNS: function (ns, t) { var e = new Elem(t); e.classList = mkclasslist(e); return e; },
  createTextNode: function (t) { var e = new Elem("#text"); e._text = t; return e; },
  querySelector: function (sel) {
    if (!registry[sel]) { registry[sel] = new Elem("div"); registry[sel].classList = mkclasslist(registry[sel]); }
    return registry[sel];
  },
  addEventListener: function () {}
};
var localStorage = {
  _v: {},
  getItem: function (k) { return this._v[k] === undefined ? null : this._v[k]; },
  setItem: function (k, v) { this._v[k] = v; },
  removeItem: function (k) { delete this._v[k]; }
};
function Blob() {}
var URL = { createObjectURL: function () { return "blob:"; }, revokeObjectURL: function () {} };
function alert(m) { print("ALERT: " + m); }
var window = {};
var FileReader = function () {};

// --- the real data, then the real script ---
eval(readFile(DATA_PATH));       // sets window.M2_PADMAP_DATA
eval(readFile(SCRIPT_PATH));

//==================================================================================================
//  assertions
//==================================================================================================

var allPortSets = [];
(function () {
  var seen = {};
  DRIVERS.forEach(function (d) { if (!seen[d.input]) { seen[d.input] = 1; allPortSets.push(d.input); } });
})();

// 90 Model 2 + 42 System 22 + 11 System 21 + 10 Model 1 GAME entries; 32 + 18 + 7 + 6 distinct port
// sets. All four families now, one table.
check(DRIVERS.length === 153, "expected 153 GAME entries, got " + DRIVERS.length);
check(allPortSets.length === 63, "expected 63 port sets, got " + allPortSets.length);

// every port set must render, seed and validate without throwing
var rendered = 0;
allPortSets.forEach(function (ps) {
  current = ps; picked = null;
  try { render(); rendered++; }
  catch (e) { FAILS.push("render(" + ps + ") threw: " + e); }
  try { validate(ps); }
  catch (e) { FAILS.push("validate(" + ps + ") threw: " + e); }
});
check(rendered === allPortSets.length, "only " + rendered + "/" + allPortSets.length + " port sets rendered");

// every port set covered by some family's TIERS table, or the library silently drops it into
// "unclassified". TIERS is family-keyed now; port set names are unique across families, so flattening
// every family's tiers into one map is equivalent to checking each within its own tab.
var tiered = {};
Object.keys(TIERS).forEach(function (fam) {
  Object.keys(TIERS[fam]).forEach(function (t) {
    TIERS[fam][t].forEach(function (m) { tiered[m] = fam + "/" + t; });
  });
});
allPortSets.forEach(function (ps) { check(tiered[ps], "port set '" + ps + "' is in no tier"); });
Object.keys(tiered).forEach(function (m) {
  check(allPortSets.indexOf(m) >= 0, "TIERS names '" + m + "', which is not a port set in the driver table");
});

// sets[] must cover every GAME entry: for each entry, the row found by (name, then parent) must be its own
allPortSets.forEach(function (ps) {
  var names = setsForPortSet(ps);
  DRIVERS.filter(function (d) { return d.input === ps; }).forEach(function (d) {
    var hit = names.indexOf(d.set) >= 0 || (d.parent !== "0" && names.indexOf(d.parent) >= 0);
    check(hit, "'" + d.set + "' (port set " + ps + ") is not reached by sets=[" + names + "]");
  });
});

// ...and no name may be claimed by two rows, or the core's lookup order would decide it
var claimed = {};
allPortSets.forEach(function (ps) {
  setsForPortSet(ps).forEach(function (n) {
    check(!claimed[n], "'" + n + "' is claimed by both " + claimed[n] + " and " + ps);
    claimed[n] = ps;
  });
});

//--------------------------------------------------------------------------------------------------
//  the two rules that matter, on the seeded (generic) row
//--------------------------------------------------------------------------------------------------

// daytona has pedals, and the generic row puts buttons 7/8 on the trigger thresholds. That is the
// collision the whole change exists to kill, so a freshly seeded daytona MUST report it.
doc = { schema: 1, rows: {} };
current = "daytona";
var dm = validate("daytona");
var pedalErrs = dm.filter(function (m) { return m.level === "e" && /trigger threshold/.test(m.text); });
check(pedalErrs.length === 2, "seeded daytona should report 2 trigger-threshold errors, got " + pedalErrs.length);

// The seeded row assigns all nine slots, so nothing is unreachable yet. Setting one to NONE must warn,
// and writing a note must clear the warning — that pairing is what stops "no control" being silent.
check(!dm.some(function (m) { return /unreachable/.test(m.text); }),
  "the seeded row assigns all nine slots, so nothing should be unreachable");
rowFor("daytona").buttons[0].source = "NONE";
check(validate("daytona").filter(function (m) { return m.level === "w" && /Button 1 .*unreachable/.test(m.text); }).length === 1,
  "button 1 set to NONE with no note should warn");
rowFor("daytona").buttons[0].why = "pad is out of buttons";
check(validate("daytona").filter(function (m) { return /Button 1 .*unreachable/.test(m.text); }).length === 0,
  "a why note should clear the unreachable warning");

// the gear off-by-one note must fire on daytona and not on vf2
check(dm.some(function (m) { return /offset by one/.test(m.text); }), "daytona should carry the gear off-by-one note");
current = "vf2";
check(!validate("vf2").some(function (m) { return /offset by one/.test(m.text); }), "vf2 should not carry the gear note");

// motoraid has two buttons and two pedals. The seeded row must therefore be CLEAN: slots 7 and 8 are
// NONE because the set has no button 7 or 8, so there is nothing to collide with the throttle and brake.
// Seeding all nine and reporting two errors here is the false positive this pairing exists to prevent.
doc = { schema: 1, rows: {} };
current = "motoraid";
var mo = validate("motoraid").filter(function (m) { return m.level === "e"; });
check(mo.length === 0, "seeded motoraid should be clean, got: " + mo.map(function (m) { return m.text; }).join(" | "));
check(rowFor("motoraid").buttons[6].source === "NONE", "motoraid slot 7 should seed to NONE, not a trigger");
check(rowFor("motoraid").buttons[0].source === "B", "motoraid slot 1 should seed to B");

// vf2 HAS a joystick, so a d-pad source must be refused
rowFor("vf2").buttons[0].source = "UP";
var vj = validate("vf2").filter(function (m) { return m.level === "e" && /D-pad/.test(m.text); });
check(vj.length === 1, "vf2 with button 1 on D-pad Up should error, got " + vj.length);

// two buttons on one control
rowFor("vf2").buttons[0].source = "A";      // button 2 is already A
var dup = validate("vf2").filter(function (m) { return m.level === "e" && /feeds MAME buttons/.test(m.text); });
check(dup.length === 1, "duplicate source should error, got " + dup.length);

// coin/start are refused
rowFor("vf2").buttons[0].source = "SELECT";
check(validate("vf2").some(function (m) { return m.level === "e" && /which carries COIN/.test(m.text); }),
  "a numbered button on SELECT should error");

// daytona seeded clean: put VR2/VR3 on the free d-pad, which daytona may use (no joystick)
doc = { schema: 1, rows: {} };
current = "daytona";
var r = rowFor("daytona");
["NONE","B","A","Y","X","UP","RIGHT","DOWN","LEFT"].forEach(function (s, i) { r.buttons[i].source = s; });
r.buttons[0].why = "GEAR N: shift down out of gear 1 reaches neutral";
relabelFromRow(r, dumpForPortSet("daytona"));
var clean = validate("daytona").filter(function (m) { return m.level === "e"; });
check(clean.length === 0, "the intended daytona row should have no errors, got: " +
  clean.map(function (m) { return m.text; }).join(" | "));

// ...and its labels must come from the driver's own words
check(r.labels.B === "GEAR 1", "daytona B should be labelled GEAR 1, got '" + r.labels.B + "'");
check(r.labels.UP === "VR1 (Red)", "daytona D-pad Up should be VR1 (Red), got '" + r.labels.UP + "'");
check(r.labels.LEFT === "VR4 (Green)", "daytona D-pad Left should be VR4 (Green), got '" + r.labels.LEFT + "'");
check(/Accelerator/.test(r.labels.R2 || ""), "daytona R2 should mention the accelerator, got '" + r.labels.R2 + "'");
check(/Brake/.test(r.labels.L2 || ""), "daytona L2 should mention the brake, got '" + r.labels.L2 + "'");
check(r.labels.LSTICK_X === "Steering", "daytona left stick X should be Steering, got '" + r.labels.LSTICK_X + "'");
check(!(r.labels.R || "").trim(), "daytona R should be free, got '" + r.labels.R + "'");

// 🚨 relabelFromRow must be IDEMPOTENT. It is called on seed and after every assignment, and the first
// version appended to the analog slots without clearing them, so a second call turned daytona's steering
// into "Steering / Steering / Steering / Stick X". Check every port set, not just this one.
allPortSets.forEach(function (ps) {
  doc = { schema: 1, rows: {} };
  var row = rowFor(ps);
  var once = JSON.stringify(row.labels);
  relabelFromRow(row, dumpForPortSet(ps));
  relabelFromRow(row, dumpForPortSet(ps));
  check(JSON.stringify(row.labels) === once, "relabelFromRow is not idempotent on " + ps);
});

// A manual label must survive a reassignment, or editing one and then moving a button silently reverts it
doc = { schema: 1, rows: {} };
current = "daytona";
var mrow = rowFor("daytona");
mrow.labels.__manual = { B: true };
mrow.labels.B = "hand written";
assign(3, "L");
check(mrow.labels.B === "hand written", "a manual label must survive a reassignment, got '" + mrow.labels.B + "'");

// vf2's labels come from PORT_NAME with the player prefix stripped
doc = { schema: 1, rows: {} };
current = "vf2";
var v = rowFor("vf2");
check(v.labels.B === "Punch", "vf2 B should be 'Punch', got '" + v.labels.B + "'");
check(v.labels.A === "Kick", "vf2 A should be 'Kick', got '" + v.labels.A + "'");
check(v.labels.Y === "Guard", "vf2 Y should be 'Guard', got '" + v.labels.Y + "'");
// vf2 declares only 3 buttons, so slots 4-9 must not leave stale generic labels behind
check(!(v.labels.X || "").trim(), "vf2 X should be blank (no button 4), got '" + v.labels.X + "'");
check(!(v.labels.R || "").trim(), "vf2 R should be blank, got '" + v.labels.R + "'");

// 🚨 A button dropped on an OCCUPIED control must SWAP with what is there, not stack on top of it.
// Reported from doa: Y holds Kick (button 3) and dragging Hold (button 1, on B) onto it left BOTH
// claiming Y — an error state the pad renders as Y still saying Kick, i.e. as the drop being ignored.
doc = { schema: 1, rows: {} };
current = "doa";
var da = rowFor("doa");
check(da.buttons[0].source === "B" && da.buttons[2].source === "Y",
  "doa should seed Hold on B and Kick on Y, got " + da.buttons[0].source + "/" + da.buttons[2].source);
assign(1, "Y");
check(da.buttons[0].source === "Y", "Hold should be on Y after the drop, got " + da.buttons[0].source);
check(da.buttons[2].source === "B", "Kick should have taken Hold's old control, got " + da.buttons[2].source);
check(validate("doa").filter(function (m) { return m.level === "e"; }).length === 0,
  "a swap must not leave a duplicate-source error");
check(da.labels.Y === "Hold" && da.labels.B === "Kick",
  "the labels must follow the swap, got Y='" + da.labels.Y + "' B='" + da.labels.B + "'");

// re-dropping a button on the control it already holds keeps it there, and is also the way out of a pair
// that is ALREADY stacked — from a hand-edited file, or from work saved before the swap existed. The drop
// wins and the other claim goes to NONE, because there is no old control to hand back.
assign(1, "Y");
check(da.buttons[0].source === "Y", "re-assigning a button to its own control must not clear it");
da.buttons[2].source = "Y";                 // the stacked state, as a stale document would carry it
assign(1, "Y");
check(da.buttons[0].source === "Y" && da.buttons[2].source === "NONE",
  "re-dropping must resolve an existing stack, got " + da.buttons[0].source + "/" + da.buttons[2].source);
da.buttons[2].source = "B";
relabelFromRow(da, dumpForPortSet("doa"));

// and a button that has NO control evicts rather than swaps — the displaced one becomes NONE, which the
// unreachable warning then asks for a reason for. Nothing is silently unassigned either way.
assign(1, "NONE");
assign(1, "A");                     // A holds Punch (button 2)
check(da.buttons[1].source === "NONE", "Punch should be evicted to NONE, got " + da.buttons[1].source);
check(validate("doa").some(function (m) { return m.level === "w" && /unreachable/.test(m.text); }),
  "the evicted button should raise the unreachable warning");

//--------------------------------------------------------------------------------------------------
//  export
//--------------------------------------------------------------------------------------------------
doc = { schema: 1, rows: {} };
["daytona", "vf2", "vcop2"].forEach(function (ps) { current = ps; rowFor(ps); });
var ex = exportDoc();
var txt = JSON.stringify(ex);
check(txt.indexOf("__manual") < 0, "export must not leak the __manual bookkeeping key");
check(ex.rows.length === 3, "export should hold 3 rows, got " + ex.rows.length);
check(ex.rows[0].id === "daytona" && ex.rows[1].id === "vcop2" && ex.rows[2].id === "vf2",
  "export rows should be sorted by id, got " + ex.rows.map(function (r) { return r.id; }).join(","));
check(ex.generic.buttons.length === 9, "the generic row needs 9 slots");
check(ex.generic.buttons.map(function (b) { return b.source; }).join(",") === "B,A,Y,X,R,L,L2_AXIS,R2_AXIS,R3",
  "the generic row must stay today's Classic exactly");
ex.rows.forEach(function (r) {
  check(r.buttons.length === 9, r.id + ": expected 9 button slots, got " + r.buttons.length);
  r.buttons.forEach(function (b, i) {
    check(BUTTON_SOURCES.indexOf(b.source) >= 0, r.id + " slot " + (i + 1) + ": '" + b.source + "' is not a known source");
  });
  check(r.sets.length > 0, r.id + ": no sets");
});

// round-trip through THE REAL LOADER. 🚨 This block used to hand-roll adoptParsed's body, and that is
// precisely why it passed through the 2026-07-31 bug below: the copy in the test carried the same wrong
// line as the copy in the tool, so the two agreed and neither was right. Never re-implement the code
// under test in the test.
var before = JSON.parse(JSON.stringify(exportDoc()));
adoptParsed(JSON.parse(JSON.stringify(before)), true);
var after = exportDoc();
check(JSON.stringify(before.rows) === JSON.stringify(after.rows), "export -> load -> export is not stable");

// 🚨 THE 2026-07-31 BUG, which shipped: loading a file marked EVERY label a manual override, so labels
// stopped following reassignment for the rest of the session. doa was authored, saved, built and played
// with BUTTON1 (Hold) bound to Y while the frontend called Y "Kick" — §1.1's two-views-of-one-fact drift,
// reintroduced by the loader rather than by a second table.
//
// The invariant, and it is the one the whole tool exists to hold: every control a button feeds is
// labelled with THAT button's wording.
function checkNoLabelDrift(row, where) {
  row.buttons.forEach(function (b, i) {
    if (b.source === "NONE" || !b.label) return;
    var slot = b.source === "L2_AXIS" ? "L2" : b.source === "R2_AXIS" ? "R2" : b.source;
    var parts = (row.labels[slot] || "").split(" / ");
    check(parts.indexOf(b.label) >= 0, where + ": BUTTON" + (i + 1) + " (" + b.label + ") is on " +
          slot + ", which is labelled '" + row.labels[slot] + "'");
  });
}

(function () {
  var file = { schema: 1, rows: [{
    id: "doa", sets: ["doa"], lightgun: false,
    buttons: [{ source: "B", label: "Hold" }, { source: "A", label: "Punch" }, { source: "Y", label: "Kick" }],
    labels: { B: "Hold", A: "Punch", Y: "Kick" }
  }] };
  adoptParsed(file, true);
  current = "doa";
  checkNoLabelDrift(doc.rows["doa"], "on load");
  assign(1, "Y");                     // drag Hold onto Y; the swap sends Kick to B
  var row = doc.rows["doa"];
  check(row.buttons[0].source === "Y" && row.buttons[2].source === "B", "the swap itself broke");
  checkNoLabelDrift(row, "after reassignment");
})();

// and every row currently authored, re-checked after one render
allPortSets.forEach(function (ps) { current = ps; render(); checkNoLabelDrift(rowFor(ps), ps); });

//==================================================================================================
print("port sets: " + allPortSets.length + ", dumps: " + Object.keys(DUMPS).length);
if (FAILS.length) { print("\n" + FAILS.length + " FAILURE(S):"); FAILS.forEach(function (f) { print("  ✗ " + f); }); }
else print("all checks passed");
