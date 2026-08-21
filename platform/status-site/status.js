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

async function validateResponse(response, kind) {
  if (kind === "updates" && response.status === 503) return "gated";
  if (!response.ok) throw new Error(`HTTP ${response.status}`);
  if (kind === "oidc") {
    const discovery = await response.json();
    if (discovery?.issuer !== "https://auth.k32.run/realms/kitsu") {
      throw new Error("issuer mismatch");
    }
  } else {
    const body = (await response.text()).trim();
    const expected = kind === "ready" ? "ready" : "ok";
    if (body !== expected) throw new Error("unexpected health response");
  }
  return "ok";
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
    const state = await validateResponse(response, element.dataset.kind);
    if (state === "gated") {
      setState(element, "gated", "Not promoted");
      return { state, counted: false };
    }
    setState(element, "ok", "Operational");
    return { state, counted: true };
  } catch {
    setState(element, "down", "Unavailable");
    return { state: "down", counted: true };
  }
}

async function run() {
  refresh.disabled = true;
  const results = await Promise.all(checks.map(probe));
  const counted = results.filter((result) => result.counted);
  const available = counted.filter((result) => result.state === "ok").length;
  const gated = results.filter((result) => result.state === "gated").length;
  const healthy = available === counted.length;
  summaryDot.className = healthy ? "dot ok" : "dot down";
  summaryTitle.textContent = healthy ? "Checked public services are operational" : "One or more public services are unavailable";
  summaryDetail.textContent = `${available} of ${counted.length} active public checks passed.${gated ? " Signed firmware updates are not promoted and are not counted as an outage." : " Signed firmware updates are promoted and included."}`;
  checked.textContent = `Checked ${new Date().toLocaleTimeString()}`;
  refresh.disabled = false;
}

refresh.addEventListener("click", () => { void run(); });
void run();
