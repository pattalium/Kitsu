export type CareAction = "pet" | "feed" | "play" | "listen_once";

export type ConnectionPath = "gateway" | "cloud" | "offline";

export interface KitsuVitals {
  energy: number;
  affection: number;
  curiosity: number;
}

export interface NearbyPeer {
  id: string;
  name: string;
  role: "client" | "repeater" | "room" | "sensor";
  lastSeen: string;
  encounters: number;
}

export interface TimelineEvent {
  id: string;
  label: string;
  detail: string;
  at: string;
  tone: "care" | "mesh" | "system";
}

export interface CompanionSnapshot {
  companionId: string;
  uid: string;
  name: string;
  species: string;
  mood: string;
  bond: number;
  evolution: number;
  vitals: KitsuVitals;
  connection: {
    path: ConnectionPath;
    serverRegistered: boolean;
    gatewayName: string | null;
    lastSeen: string;
  };
  mesh: {
    profile: string;
    txLocked: boolean;
    peersSeen: number;
    unreadMessages: number;
  };
  peers: NearbyPeer[];
  events: TimelineEvent[];
  cursor: string;
}

export interface CompanionChoice {
  id: string;
  hardware_uid: string;
  display_name: string;
  status: string;
  last_seen_at: string | null;
}

interface SnapshotProjection {
  companion: Record<string, unknown>;
  vitals: Record<string, unknown>;
  mood: Record<string, unknown>;
  bond: Record<string, unknown>;
  evolution: Record<string, unknown>;
  connectivity: Record<string, unknown>;
  mesh: Record<string, unknown>;
  counts: Record<string, unknown>;
  recent_events: Array<Record<string, unknown>>;
  cursor: string;
}

interface PeerProjection {
  public_key_b64: string;
  public_key_hex: string;
  role: NearbyPeer["role"];
  name: string;
  seen_count: number;
  last_seen: {
    epoch: number | null;
  };
  updated_at: string;
}

export interface BrowserSession {
  authenticated: true;
  owner_id: string;
  csrf_token: string;
  expires_at: string;
}

export class KitsuApiError extends Error {
  constructor(
    public readonly status: number,
    message: string,
  ) {
    super(message);
    this.name = "KitsuApiError";
  }
}

function configuredHttpsUrl(raw: string | undefined): string {
  if (!raw) return "";
  try {
    const url = new URL(raw);
    if (url.protocol !== "https:" || url.username || url.password) return "";
    return url.toString();
  } catch {
    return "";
  }
}

const configuredBase = configuredHttpsUrl(import.meta.env.VITE_KITSU_API_BASE).replace(/\/$/, "");

export const isApiConfigured = configuredBase.length > 0;

export const serverRepositoryUrl = configuredHttpsUrl(
  import.meta.env.VITE_KITSU_SERVER_REPOSITORY_URL,
) || "https://github.com/pattalium/Kitsu";

async function request<T>(path: string, init: RequestInit = {}): Promise<T> {
  const response = await fetch(`${configuredBase}${path}`, {
    ...init,
    credentials: "include",
    headers: {
      Accept: "application/json",
      ...(init.body ? { "Content-Type": "application/json" } : {}),
      ...init.headers,
    },
  });

  if (!response.ok) {
    const problem = await response.json().catch(() => null) as
      | { error?: { code?: string; message?: string } }
      | null;
    throw new KitsuApiError(
      response.status,
      problem?.error?.message ?? problem?.error?.code ?? `Request failed (${response.status})`,
    );
  }

  return response.json() as Promise<T>;
}

function numberValue(record: Record<string, unknown>, names: string[], fallback: number): number {
  for (const name of names) {
    const value = record[name];
    if (typeof value === "number" && Number.isFinite(value)) return value;
  }
  return fallback;
}

function stringValue(record: Record<string, unknown>, names: string[], fallback: string): string {
  for (const name of names) {
    const value = record[name];
    if (typeof value === "string" && value.trim()) return value;
  }
  return fallback;
}

function booleanValue(record: Record<string, unknown>, names: string[], fallback: boolean): boolean {
  for (const name of names) {
    const value = record[name];
    if (typeof value === "boolean") return value;
  }
  return fallback;
}

function relativeTime(value: unknown): string {
  if (typeof value !== "string" && typeof value !== "number") return "unknown";
  const timestamp = typeof value === "number" ? value * 1_000 : Date.parse(value);
  if (!Number.isFinite(timestamp)) return "unknown";
  const seconds = Math.max(0, Math.round((Date.now() - timestamp) / 1_000));
  if (seconds < 10) return "just now";
  if (seconds < 60) return `${seconds} sec`;
  if (seconds < 3_600) return `${Math.round(seconds / 60)} min`;
  if (seconds < 86_400) return `${Math.round(seconds / 3_600)} h`;
  return `${Math.round(seconds / 86_400)} d`;
}

function eventTone(type: string): TimelineEvent["tone"] {
  if (type.startsWith("mesh.")) return "mesh";
  if (type.startsWith("companion.")) return "care";
  return "system";
}

function humanizeType(type: string): string {
  const sentence = type.replaceAll(".", " ").replaceAll("_", " ");
  return sentence.charAt(0).toUpperCase() + sentence.slice(1);
}

function mapEvent(event: Record<string, unknown>): TimelineEvent {
  const type = stringValue(event, ["event_type"], "system.event");
  const body = typeof event.body === "object" && event.body !== null
    ? event.body as Record<string, unknown>
    : {};
  return {
    id: stringValue(event, ["event_id", "cursor"], crypto.randomUUID()),
    label: stringValue(body, ["label", "title"], humanizeType(type)),
    detail: stringValue(body, ["detail", "summary"], "Recorded by your companion"),
    at: relativeTime(event.received_at ?? event.observed_epoch),
    tone: eventTone(type),
  };
}

function mapPeer(peer: PeerProjection): NearbyPeer {
  return {
    id: peer.public_key_b64,
    name: peer.name || "Unnamed MeshCore peer",
    role: peer.role,
    lastSeen: relativeTime(peer.last_seen.epoch ?? peer.updated_at),
    encounters: Math.max(0, peer.seen_count),
  };
}

function mapSnapshot(
  item: CompanionChoice,
  projection: SnapshotProjection,
  peers: PeerProjection[],
): CompanionSnapshot {
  const online = booleanValue(projection.connectivity, ["online"], false);
  const serverRegistered = typeof projection.connectivity.gateway_id === "string"
    && projection.connectivity.gateway_id.length > 0;
  const companionId = stringValue(projection.companion, ["id"], item.id);
  return {
    companionId,
    uid: stringValue(projection.companion, ["hardware_uid"], item.hardware_uid),
    name: stringValue(projection.companion, ["display_name"], item.display_name),
    species: stringValue(projection.companion, ["species", "pack_name"], "Companion"),
    mood: stringValue(projection.mood, ["label", "name", "mood"], "Calm"),
    bond: numberValue(projection.bond, ["value", "level", "bond"], 0),
    evolution: numberValue(projection.evolution, ["stage", "level", "evolution"], 0),
    vitals: {
      energy: numberValue(projection.vitals, ["energy"], 0),
      affection: numberValue(projection.vitals, ["affection"], 0),
      curiosity: numberValue(projection.vitals, ["curiosity"], 0),
    },
    connection: {
      path: online ? "gateway" : "offline",
      serverRegistered,
      gatewayName: online ? "Home gateway" : null,
      lastSeen: relativeTime(projection.connectivity.last_seen_at ?? item.last_seen_at),
    },
    mesh: {
      profile: stringValue(projection.mesh, ["profile", "profile_name"], "UK/EU Narrow"),
      txLocked: booleanValue(projection.mesh, ["tx_locked"], true),
      peersSeen: numberValue(projection.counts, ["peers"], peers.length),
      unreadMessages: numberValue(projection.counts, ["unread_messages"], 0),
    },
    peers: peers.slice(0, 12).map(mapPeer),
    events: projection.recent_events.map(mapEvent),
    cursor: projection.cursor,
  };
}

export function getBrowserSession(): Promise<BrowserSession> {
  return request<BrowserSession>("/v1/browser/session");
}

export async function getCompanionChoices(): Promise<CompanionChoice[]> {
  const list = await request<{ items: CompanionChoice[] }>("/v1/companions");
  return list.items;
}

export async function getCompanionSnapshot(companion: CompanionChoice): Promise<CompanionSnapshot> {
  const id = encodeURIComponent(companion.id);
  const [projection, peerList] = await Promise.all([
    request<SnapshotProjection>(`/v1/companions/${id}/snapshot`),
    request<{ items: PeerProjection[] }>(`/v1/companions/${id}/peers`),
  ]);
  return mapSnapshot(companion, projection, peerList.items);
}

const careActionTypes: Record<CareAction, string> = {
  pet: "companion.pet",
  feed: "companion.feed",
  play: "companion.play",
  listen_once: "companion.listen_once",
};

export function sendCareAction(
  companionId: string,
  action: CareAction,
  csrfToken: string,
  idempotencyKey: string,
): Promise<{ id: string; status: string }> {
  return request(`/v1/companions/${encodeURIComponent(companionId)}/actions`, {
    method: "POST",
    headers: {
      "X-CSRF-Token": csrfToken,
      "Idempotency-Key": idempotencyKey,
    },
    body: JSON.stringify({
      action_type: careActionTypes[action],
      parameters: action === "listen_once" ? { duration_ms: 60_000 } : {},
      expires_in_seconds: action === "listen_once" ? 90 : 30,
    }),
  });
}

export function signInUrl(returnUrl = "/"): string {
  const params = new URLSearchParams({ return_url: new URL(returnUrl, window.location.origin).toString() });
  return `${configuredBase}/v1/browser/auth/start?${params}`;
}
