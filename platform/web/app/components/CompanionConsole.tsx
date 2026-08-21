import { useEffect, useState } from "react";
import {
  CareAction,
  CompanionChoice,
  CompanionSnapshot,
  getBrowserSession,
  getCompanionChoices,
  getCompanionSnapshot,
  isApiConfigured,
  KitsuApiError,
  sendCareAction,
  serverRepositoryUrl,
  signInUrl,
} from "../lib/kitsu-api";

const actionPresentation: Record<CareAction, { label: string; glyph: string; animation: string }> = {
  pet: { label: "Pet", glyph: "♡", animation: "pet" },
  feed: { label: "Feed", glyph: "◇", animation: "feed" },
  play: { label: "Play", glyph: "✦", animation: "play" },
  listen_once: { label: "Listen", glyph: "⌁", animation: "listen" },
};
const approvedCompanionPreviewRevision = "20260821-approved-v2";

function speciesAnimation(species: string, animation: string): string {
  const normalized = species.trim().toLowerCase();
  if (normalized === "cat" || normalized === "fox" || normalized === "dog") {
    return `/companion/${normalized}-${animation}.gif?v=${approvedCompanionPreviewRevision}`;
  }
  return "/brand/kitsu-app-icon.png";
}

function clampPercent(value: number): number {
  return Math.max(0, Math.min(100, Math.round(value)));
}

function VitalsBar({ label, value }: { label: string; value: number }) {
  const safeValue = clampPercent(value);
  return (
    <div className="vital">
      <div className="vital-label"><span>{label}</span><strong>{safeValue}</strong></div>
      <meter className="vital-track" aria-label={`${label}: ${safeValue}%`} min={0} max={100} value={safeValue}>{safeValue}%</meter>
    </div>
  );
}

export function CompanionConsole() {
  const [snapshot, setSnapshot] = useState<CompanionSnapshot | null>(null);
  const [companions, setCompanions] = useState<CompanionChoice[]>([]);
  const [csrfToken, setCsrfToken] = useState<string | null>(null);
  const [mode, setMode] = useState<
    "loading" | "choose" | "live" | "signed-out" | "server-missing" | "error"
  >(isApiConfigured ? "loading" : "server-missing");
  const [activeAnimation, setActiveAnimation] = useState("idle");
  const [pendingAction, setPendingAction] = useState<CareAction | null>(null);
  const [notice, setNotice] = useState<string | null>(null);

  useEffect(() => {
    if (!isApiConfigured) return;
    let cancelled = false;

    async function load() {
      try {
        const session = await getBrowserSession();
        if (cancelled) return;
        if (!session.authenticated) {
          setMode("signed-out");
          return;
        }
        setCsrfToken(session.csrf_token);
        const choices = await getCompanionChoices();
        if (cancelled) return;
        setCompanions(choices);
        if (choices.length === 0) {
          setMode("server-missing");
          return;
        }
        if (choices.length > 1) {
          setMode("choose");
          return;
        }
        const companion = await getCompanionSnapshot(choices[0]);
        if (!cancelled) {
          if (!companion.connection.serverRegistered) {
            setMode("server-missing");
            return;
          }
          setSnapshot(companion);
          setMode("live");
        }
      } catch (error) {
        if (!cancelled) {
          if (error instanceof KitsuApiError && error.status === 401) setMode("signed-out");
          else if (error instanceof KitsuApiError && error.status === 404) setMode("server-missing");
          else setMode("error");
        }
      }
    }

    void load();
    return () => { cancelled = true; };
  }, []);

  async function selectCompanion(choice: CompanionChoice) {
    setSnapshot(null);
    setActiveAnimation("idle");
    setMode("loading");
    setNotice(null);
    try {
      const companion = await getCompanionSnapshot(choice);
      if (!companion.connection.serverRegistered) {
        setMode("server-missing");
        return;
      }
      setSnapshot(companion);
      setMode("live");
    } catch {
      setMode("error");
    }
  }

  if (!snapshot) {
    return (
      <main className="shell access-shell">
        <header className="topbar access-topbar">
          <a className="brand" href="#access" aria-label="Kitsu companion home">
            <span className="brand-mark">
            <img src="/brand/kitsu-app-icon.png" alt="" width={40} height={40} />
            </span>
            <span><strong>Kitsu</strong><small>companion network</small></span>
          </a>
          <div className="connection-pill" data-state="offline">
            <span className="pulse" />
            <span><strong>Private access</strong><small>{mode === "loading" ? "Checking" : "Not connected"}</small></span>
          </div>
        </header>
        <section className="access-state" id="access" aria-live="polite">
          <img
            className="access-mascot"
            src="/brand/kitsu-app-icon.png"
            alt="Kitsu"
            width={176}
            height={176}
          />
          {mode === "loading" ? (
            <><p className="eyebrow">SECURE CONNECTION</p><h1>Finding your companion.</h1><p>Checking your authenticated Kitsu account and home gateway.</p></>
          ) : null}
          {mode === "signed-out" ? (
            <><p className="eyebrow">OWNER ACCESS</p><h1>Your companion is private.</h1><p>Sign in through the configured identity provider to continue.</p><a className="access-action" href={signInUrl()}>Sign in</a></>
          ) : null}
          {mode === "choose" ? (
            <><p className="eyebrow">YOUR COMPANIONS</p><h1>Who are we visiting?</h1><p>Choose the companion you want to open. Kitsu never guesses when an account has more than one.</p><div className="companion-picker">{companions.map((choice) => <button key={choice.id} onClick={() => void selectCompanion(choice)}><strong>{choice.display_name}</strong><span>{choice.hardware_uid} · {choice.status}</span></button>)}</div></>
          ) : null}
          {mode === "server-missing" ? (
            <><p className="eyebrow">REMOTE ACCESS</p><h1>No enrolled gateway found.</h1><p>Pair the Android app over Bluetooth, add Wi-Fi, choose your gateway, and complete the physical enrollment prompt on Kitsu.</p><a className="access-action" href="https://docs.k32.run/connectivity/">Open the setup guide</a><a className="native-app-link" href={serverRepositoryUrl} rel="noreferrer">View the Kitsu source</a></>
          ) : null}
          {mode === "error" ? (
            <><p className="eyebrow">CONNECTION ERROR</p><h1>Kitsu could not verify the route.</h1><p>No preview or stale companion state is presented as live. Try again after checking the hosted service and your home gateway.</p><button className="access-action" onClick={() => window.location.reload()}>Try again</button></>
          ) : null}
          <a className="native-app-link" href="https://k32.run/#download">Get the signed native Android app</a>
        </section>
      </main>
    );
  }

  const currentSnapshot = snapshot;
  const activeAnimationPath = speciesAnimation(currentSnapshot.species, activeAnimation);

  const connectionLabel = currentSnapshot.connection.path === "gateway"
    ? currentSnapshot.connection.gatewayName ?? "Home gateway"
    : currentSnapshot.connection.path === "cloud" ? "Cloud relay" : "Offline";

  async function performAction(action: CareAction) {
    if (pendingAction) return;
    const presentation = actionPresentation[action];
    setActiveAnimation(presentation.animation);
    setPendingAction(action);
    setNotice(null);

    try {
      if (mode !== "live" || !csrfToken) {
        throw new Error("Sign in and reconnect before sending an action.");
      } else {
        const idempotencyKey = crypto.randomUUID();
        await sendCareAction(currentSnapshot.companionId, action, csrfToken, idempotencyKey);
        setNotice(`${presentation.label} queued for ${currentSnapshot.name}.`);
      }
    } catch (error) {
      setNotice(error instanceof Error ? error.message : "The action could not be queued.");
    } finally {
      window.setTimeout(
        () => setActiveAnimation("idle"),
        2200,
      );
      setPendingAction(null);
    }
  }

  return (
    <main className="shell">
      <header className="topbar">
        <a className="brand" href="#companion" aria-label="Kitsu companion home">
          <span className="brand-mark">
              <img src="/brand/kitsu-app-icon.png" alt="" width={40} height={40} />
          </span>
          <span><strong>Kitsu</strong><small>companion network</small></span>
        </a>
        <div className="connection-pill" data-state={snapshot.connection.path}>
          <span className="pulse" />
          <span><strong>{connectionLabel}</strong><small>{snapshot.connection.lastSeen}</small></span>
        </div>
        <nav aria-label="Primary">
          <a className="active" href="#companion">Companion</a>
          <a href="#mesh">Mesh</a>
          <a href="#history">History</a>
              <a href="https://k32.run/#download">Android app</a>
        </nav>
      </header>

      <section className="hero" id="companion">
        <div className="hero-copy">
          <p className="eyebrow"><span>{snapshot.species.toUpperCase()}</span> / {snapshot.uid}</p>
          <h1>{snapshot.name} is <em>{snapshot.mood.toLowerCase()}</em> today.</h1>
          <p className="lede">A companion with their own memory, personality, and a little window into the mesh around them.</p>
          <div className="identity-row">
            <span>Bond <strong>{snapshot.bond}</strong></span>
            <span>Evolution <strong>Stage {snapshot.evolution}</strong></span>
            <span className={snapshot.mesh.txLocked ? "locked" : "ready"}>{snapshot.mesh.txLocked ? "TX locked" : "TX ready"}</span>
          </div>
        </div>

        <div className="companion-stage" aria-live="polite">
          <div className="orbit orbit-one" />
          <div className="orbit orbit-two" />
          <div className="sprite-frame">
            <img
              src={activeAnimationPath}
              alt={`${snapshot.name}, the ${snapshot.species} companion`}
              width={224}
              height={224}
            />
          </div>
          <span className="mood-chip">{snapshot.mood}</span>
        </div>

        <aside className="vitals-card" aria-label="Companion vitals">
          <div className="card-heading"><span>Vitals</span><small>live state</small></div>
          <VitalsBar label="Energy" value={snapshot.vitals.energy} />
          <VitalsBar label="Affection" value={snapshot.vitals.affection} />
          <VitalsBar label="Curiosity" value={snapshot.vitals.curiosity} />
          <div className="care-grid">
            {(Object.keys(actionPresentation) as CareAction[]).map((action) => {
              const item = actionPresentation[action];
              return (
                <button key={action} disabled={pendingAction !== null || mode === "signed-out"} onClick={() => void performAction(action)}>
                  <span>{item.glyph}</span>{item.label}
                </button>
              );
            })}
          </div>
          {notice ? <p className="notice">{notice}</p> : null}
        </aside>
      </section>

      <section className="lower-grid">
        <article className="panel mesh-panel" id="mesh">
          <div className="panel-title">
            <div><p className="eyebrow">MESHCORE</p><h2>Nearby world</h2></div>
            <span className="profile">{snapshot.mesh.profile}</span>
          </div>
          <div className="mesh-summary">
            <div><strong>{snapshot.mesh.peersSeen}</strong><span>peers seen</span></div>
            <div><strong>{snapshot.mesh.unreadMessages}</strong><span>new message</span></div>
          </div>
          <div className="peer-list">
            {snapshot.peers.map((peer) => (
              <div className="peer" key={peer.id}>
                <span className={`role role-${peer.role}`}>{peer.role === "repeater" ? "R" : "C"}</span>
                <div><strong>{peer.name}</strong><small>{peer.role} · seen {peer.lastSeen} ago</small></div>
                <span className="encounters">×{peer.encounters}</span>
              </div>
            ))}
          </div>
        </article>

        <article className="panel timeline-panel" id="history">
          <div className="panel-title">
            <div><p className="eyebrow">MEMORY</p><h2>What Kitsu remembers</h2></div>
          </div>
          <div className="timeline">
            {snapshot.events.map((event) => (
              <div className="timeline-item" data-tone={event.tone} key={event.id}>
                <span className="timeline-dot" />
                <div><strong>{event.label}</strong><small>{event.detail}</small></div>
                <time>{event.at}</time>
              </div>
            ))}
          </div>
        </article>

        <article className="panel route-panel">
          <p className="eyebrow">CONNECTION ROUTE</p>
          <h2>One companion, three paths.</h2>
          <div className="route-map" aria-label="Connection route">
            <div><span>01</span><strong>Bluetooth</strong><small>Native app nearby</small></div>
            <i />
            <div className="current"><span>02</span><strong>Home gateway</strong><small>Wi-Fi to your PC</small></div>
            <i />
            <div><span>03</span><strong>Backend</strong><small>Private remote access</small></div>
          </div>
          <p className="route-note">The browser never speaks to the radio directly. Every remote action is authenticated, expires, and is still subject to Kitsu’s firmware safety rules.</p>
        </article>
      </section>
    </main>
  );
}
