// search.js — accessible autocomplete over GET /locations/search.

import { api } from "./api.js";

const typeNames = {
  city: "City", town: "Town", village: "Village", hamlet: "Hamlet", suburb: "Area",
  neighbourhood: "Neighbourhood", locality: "Locality", state: "State", district: "District",
  quarter: "Area", isolated_dwelling: "Settlement", junction: "Junction",
};

export function attachSearch(input, list, onPick) {
  let items = [];
  let active = -1;
  let timer = null;
  let controller = null;

  function close() {
    list.hidden = true;
    input.setAttribute("aria-expanded", "false");
    input.removeAttribute("aria-activedescendant");
    active = -1;
  }

  function render(results, note) {
    list.innerHTML = "";
    items = results;
    if (!results.length) {
      const li = document.createElement("li");
      li.className = "s-empty";
      li.textContent = note || "No matching places. You can also click the map.";
      list.appendChild(li);
    }
    results.forEach((r, i) => {
      const li = document.createElement("li");
      li.id = `${list.id}-opt-${i}`;
      li.setAttribute("role", "option");
      const name = document.createElement("span");
      name.className = "s-name";
      name.textContent = r.name_en || r.name;
      const sub = document.createElement("span");
      sub.className = "s-sub";
      const where = [r.district, r.state].filter(Boolean).join(", ") || (r.near ? `near ${r.near}` : "");
      const kind = typeNames[r.type] || r.type;
      sub.textContent = [kind, where].filter(Boolean).join(", ") + (r.routable ? "" : " (no road nearby)");
      li.append(name, sub);
      li.addEventListener("mousedown", (e) => { e.preventDefault(); choose(i); });
      list.appendChild(li);
    });
    list.hidden = false;
    input.setAttribute("aria-expanded", "true");
  }

  function highlight(i) {
    const opts = list.querySelectorAll('[role="option"]');
    opts.forEach((o, k) => o.setAttribute("aria-selected", k === i ? "true" : "false"));
    active = i;
    if (opts[i]) {
      input.setAttribute("aria-activedescendant", opts[i].id);
      opts[i].scrollIntoView({ block: "nearest" });
    }
  }

  function choose(i) {
    const r = items[i];
    if (!r) return;
    if (!r.routable) {
      render([], `${r.name} has no road within reach in the loaded map data.`);
      return;
    }
    input.value = r.label || r.name;
    close();
    onPick({ lat: r.latitude, lon: r.longitude, label: input.value });
  }

  input.addEventListener("input", () => {
    clearTimeout(timer);
    const q = input.value.trim();
    if (q.length < 2) { close(); return; }
    timer = setTimeout(async () => {
      if (controller) controller.abort();
      controller = new AbortController();
      try {
        const data = await api.search(q, controller.signal);
        render(data.results, data.note);
      } catch (e) {
        if (e.name !== "AbortError") render([], e.message);
      }
    }, 180);
  });

  input.addEventListener("keydown", (e) => {
    if (list.hidden) return;
    if (e.key === "ArrowDown") { e.preventDefault(); highlight(Math.min(active + 1, items.length - 1)); }
    else if (e.key === "ArrowUp") { e.preventDefault(); highlight(Math.max(active - 1, 0)); }
    else if (e.key === "Enter" && active >= 0) { e.preventDefault(); choose(active); }
    else if (e.key === "Escape") { close(); }
  });
  input.addEventListener("blur", () => setTimeout(close, 120));
}
