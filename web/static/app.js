// Helpers compartilhados da web UI do capi_net. Sem dependências.
// Todo texto vindo do server entra no DOM via textContent (nomes e logs são
// entrada livre).

"use strict";

async function api(path, options = {}) {
  const init = { ...options, headers: { "Content-Type": "application/json" } };
  if (init.body && typeof init.body !== "string") init.body = JSON.stringify(init.body);
  const res = await fetch(path, init);
  const text = await res.text();
  const data = text ? JSON.parse(text) : null;
  if (!res.ok) {
    const err = new Error(
      data && (data.reason || data.error) ? [data.error, data.reason].filter(Boolean).join(": ")
                                          : `HTTP ${res.status}`);
    err.status = res.status;
    err.data = data;
    throw err;
  }
  return data;
}

// h("a", {href: "/x", class: "y"}, "texto", outroNo)
function h(tag, attrs = {}, ...children) {
  const el = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs || {})) {
    if (v === null || v === undefined || v === false) continue;
    if (k === "class") el.className = v;
    else if (k.startsWith("on")) el.addEventListener(k.slice(2), v);
    else el.setAttribute(k, v === true ? "" : v);
  }
  for (const c of children.flat()) {
    if (c === null || c === undefined || c === false) continue;
    el.append(c instanceof Node ? c : document.createTextNode(String(c)));
  }
  return el;
}

// Como el.replaceChildren, mas achatando arrays e ignorando null/false
// (replaceChildren converteria esses valores em texto).
function fill(el, ...children) {
  el.replaceChildren(...children.flat(Infinity).filter((c) => c !== null && c !== undefined && c !== false)
    .map((c) => (c instanceof Node ? c : document.createTextNode(String(c)))));
  return el;
}

const fmt = {
  secs(ms) {
    return (ms / 1000).toFixed(3).replace(/0+$/, "").replace(/\.$/, "");
  },
  tc(base, inc) { return `${fmt.secs(base)}+${fmt.secs(inc)}`; },
  sha(s) { return s ? s.slice(0, 7) : ""; },
  num(v, digits = 2, sign = false) {
    if (v === null || v === undefined || Number.isNaN(v)) return "–";
    const s = v.toFixed(digits);
    return sign && v > 0 ? "+" + s : s;
  },
  int(v) { return v === null || v === undefined ? "–" : v.toLocaleString("pt-BR"); },
  // nós por segundo: "7,02 M", "850 k"
  nps(v) {
    if (v === null || v === undefined) return "–";
    if (v >= 1e6) return `${(v / 1e6).toLocaleString("pt-BR", { maximumFractionDigits: 2, minimumFractionDigits: 2 })} M`;
    if (v >= 1e3) return `${Math.round(v / 1e3).toLocaleString("pt-BR")} k`;
    return String(Math.round(v));
  },
  date(iso) {
    if (!iso) return "–";
    const d = new Date(iso);
    return d.toLocaleString("pt-BR", { dateStyle: "short", timeStyle: "short" });
  },
  ago(iso) {
    if (!iso) return "–";
    const s = Math.round((Date.now() - new Date(iso).getTime()) / 1000);
    if (s < 60) return `há ${Math.max(s, 0)} s`;
    if (s < 3600) return `há ${Math.round(s / 60)} min`;
    if (s < 86400) return `há ${Math.round(s / 3600)} h`;
    return `há ${Math.round(s / 86400)} d`;
  },
};

const STATUS_LABEL = {
  validating: "validando", invalid: "inválido", queued: "na fila", running: "rodando",
  finished: "terminado", stopped: "parado",
};
const RESULT_LABEL = {
  pending: "pendente", accepted: "aceito", rejected: "rejeitado", inconclusive: "inconclusivo",
};

function statusBadge(status) {
  return h("span", { class: `badge ${status}` }, STATUS_LABEL[status] || status);
}
function resultBadge(result) {
  if (result === "pending") return h("span", { class: "muted" }, "–");
  return h("span", { class: `badge ${result}` }, RESULT_LABEL[result] || result);
}

// Barra do LLR entre as fronteiras [lower, upper], com o zero marcado.
function llrBar(llr, lower, upper) {
  const bar = h("span", { class: "llrbar", title: `LLR ${fmt.num(llr)} em [${fmt.num(lower)}, ${fmt.num(upper)}]` });
  if (llr === null || llr === undefined || lower === null || upper === null) return bar;
  const pos = (v) => ((Math.min(Math.max(v, lower), upper) - lower) / (upper - lower)) * 100;
  const zero = pos(0), cur = pos(llr);
  const fill = h("span", { class: "fill" });
  fill.style.left = `${Math.min(zero, cur)}%`;
  fill.style.width = `${Math.abs(cur - zero)}%`;
  fill.style.background = llr >= 0 ? "var(--good)" : "var(--bad)";
  const z = h("span", { class: "zero" });
  z.style.left = `${zero}%`;
  bar.append(fill, z);
  return bar;
}

function header(active) {
  const link = (href, label, key) =>
    h("a", { href, class: active === key ? "active" : null }, label);
  return h("header", { class: "top" },
    h("a", { class: "brand", href: "/tests" }, "capi_net"),
    h("nav", {}, link("/tests", "Testes", "tests"), link("/clients", "Clients", "clients"),
      link("/tests/new", "Novo teste", "new")));
}

function mount(active, build) {
  document.body.replaceChildren(header(active), h("main", { id: "main" }));
  return build(document.getElementById("main"));
}

function errorBox(err) {
  return h("div", { class: "error-box" }, err.message || String(err));
}

// Recarrega a cada `ms` enquanto a aba estiver visível.
function autoRefresh(fn, ms) {
  let timer = null;
  const tick = async () => {
    if (document.visibilityState === "visible") {
      try { await fn(); } catch (e) { console.error(e); }
    }
    timer = setTimeout(tick, ms);
  };
  timer = setTimeout(tick, ms);
  return () => clearTimeout(timer);
}
