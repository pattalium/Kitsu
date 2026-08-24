import { validateHealthResponse } from "/status-response.js";

const checks = [...document.querySelectorAll("[data-url]")];
const checked = document.querySelector("#checked");
const summaryDot = document.querySelector("#summary-dot");
const summaryTitle = document.querySelector("#summary-title");
const summaryDetail = document.querySelector("#summary-detail");
const refresh = document.querySelector("#refresh");

function setState(element, state, label) {
  element.querySelector(".dot").className = `dot ${state}`;
  element.querySelector("b").textContent = label;
}

async function probe(element) {
  setState(element, "pending", "Checking");
  try {
    const response = await fetch(element.dataset.url, {
      cache: "no-store",
      credentials: "omit",
      mode: "cors",
      referrerPolicy: "no-referrer",
      signal: AbortSignal.timeout(6000),
    });
    await validateHealthResponse(response);
    setState(element, "ok", "Operational");
    return "ok";
  } catch {
    setState(element, "down", "Unavailable");
    return "down";
  }
}

async function run() {
  refresh.disabled = true;
  const results = await Promise.all(checks.map(probe));
  const healthy = results.every((state) => state === "ok");
  summaryDot.className = healthy ? "dot ok" : "dot down";
  summaryTitle.textContent = healthy ? "Checked public sites are reachable" : "One or more public sites are unavailable";
  summaryDetail.textContent = healthy
    ? "Every checked public origin returned the exact expected response."
    : "At least one checked public origin did not return the exact expected response.";
  checked.textContent = `Checked ${new Date().toLocaleTimeString()}`;
  refresh.disabled = false;
}

refresh.addEventListener("click", () => { void run(); });
void run();
