// Thin typed wrapper over the ESP32's JSON API. Every call goes through the
// Vite proxy at /api, so the device host is configured in vite.config.ts and
// never hardcoded here.
//
// The display only needs to read state and pair a tag. The firmware still
// exposes /api/playback, /api/volume, /api/cards (GET/DELETE) and /api/devices
// if a control surface is ever wanted again.

export interface PlayerState {
  valid: boolean
  hasTrack?: boolean
  isPlaying?: boolean
  track?: string
  artist?: string
  album?: string
  art?: string
  device?: string
  progressMs?: number
  durationMs?: number
  volume?: number
  ageMs?: number
}

export interface DeckState {
  present: boolean
  uid: string
  paired: boolean
  uri: string
}

export interface State {
  ok: boolean
  wifi: { connected: boolean; ip?: string; rssi?: number }
  spotify: { configured: boolean; tokenOk: boolean; error?: string }
  deck: DeckState
  player: PlayerState
  cards: number
}

/** Non-2xx responses carry a JSON body with the device's own error string. */
export class ApiError extends Error {
  constructor(
    message: string,
    readonly status: number,
  ) {
    super(message)
  }
}

async function req<T>(path: string, init?: RequestInit): Promise<T> {
  const res = await fetch(`/api${path}`, {
    headers: init?.body ? { 'Content-Type': 'application/json' } : undefined,
    ...init,
  })

  let body: unknown
  try {
    body = await res.json()
  } catch {
    throw new ApiError(`${res.status} ${res.statusText}`, res.status)
  }

  if (!res.ok) {
    const msg =
      typeof body === 'object' && body !== null && 'error' in body
        ? String((body as { error: unknown }).error)
        : `${res.status} ${res.statusText}`
    throw new ApiError(msg, res.status)
  }
  return body as T
}

export const getState = () => req<State>('/state')

export const pairCard = (uid: string, uri: string) =>
  req<{ uid: string; uri: string }>('/cards', {
    method: 'POST',
    body: JSON.stringify({ uid, uri }),
  })
