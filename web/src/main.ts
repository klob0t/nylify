import './style.css'
import { getState, pairCard, type State } from './api'

// The device refreshes its Spotify cache roughly every 1.6s, so polling much
// faster than this only re-reads the same answer -- but each read costs ~50ms
// and shaving the interval takes that much off how late a change appears.
const POLL_MS = 400

/** Degrees per second at full speed. 30 => one revolution per 12s. */
const SPIN_FULL_DEG_S = 30

/** Time constants: velocity closes ~63% of the remaining gap per tau. A platter
 *  reaches speed under power but coasts down against friction, so the two are
 *  deliberately asymmetric. */
const SPIN_UP_TAU = 0.9
const SPIN_DOWN_TAU = 1.7

/** How long before the pointer hides. */
const IDLE_HIDE_MS = 4000

function el<T extends HTMLElement = HTMLElement>(id: string): T {
  const node = document.getElementById(id)
  if (!node) throw new Error(`missing element #${id}`)
  return node as T
}

const ui = {
  backdrop: el('backdrop'),
  stage: el('stage'),
  discEmpty: el('disc-empty'),
  track: el('track'),
  artist: el('artist'),
  pairForm: el<HTMLFormElement>('pair-form'),
  pairUid: el('pair-uid'),
  pairUri: el<HTMLInputElement>('pair-uri'),
  pairBtn: el<HTMLButtonElement>('pair-btn'),
  pairError: el('pair-error'),
  status: el('status'),
  device: el('device'),
  conn: el('conn'),
  connText: el('conn-text'),
}

/** Writes only on change. Assigning textContent invalidates layout even when
 *  the string is identical, and this runs several times a second. */
function setText(node: HTMLElement, value: string) {
  if (node.textContent !== value) node.textContent = value
}

/** Beyond this, the device has stopped refreshing from Spotify and whatever is
 *  on screen is fiction. The normal cycle is well under 2s. */
const STALE_MS = 6000

function setConn(state: 'ok' | 'stale' | 'down', text: string, detail = '') {
  if (ui.conn.dataset.state !== state) ui.conn.dataset.state = state
  setText(ui.connText, text)
  if (ui.conn.title !== detail) ui.conn.title = detail
}

interface Disc {
  el: HTMLElement
  img: HTMLImageElement
}

function disc(id: string): Disc {
  const box = el(id)
  const img = box.querySelector('img')
  if (!img) throw new Error(`#${id} has no img`)
  return { el: box, img }
}

const discs: [Disc, Disc] = [disc('disc-a'), disc('disc-b')]

// Literal union rather than `number`: indexing a tuple with 0 | 1 is known to
// be in bounds, which keeps noUncheckedIndexedAccess satisfied without casts.
type Side = 0 | 1
let front: Side = 0 // the layer currently on screen

// --- rotation ---------------------------------------------------------------

const reduceMotion = window.matchMedia('(prefers-reduced-motion: reduce)')

let playing = false
let vinylAngle = 0
let vinylSpeed = 0
let lastFrameAt = 0

function frame(now: number) {
  // Clamp dt so a backgrounded tab does not resume with one enormous step.
  const dt = lastFrameAt ? Math.min((now - lastFrameAt) / 1000, 0.1) : 0
  lastFrameAt = now

  if (!reduceMotion.matches) {
    const target = playing ? SPIN_FULL_DEG_S : 0
    const tau = target > vinylSpeed ? SPIN_UP_TAU : SPIN_DOWN_TAU

    // Exponential approach: frame-rate independent, and never overshoots.
    vinylSpeed += (target - vinylSpeed) * (1 - Math.exp(-dt / tau))
    // It nears zero asymptotically, so settle it rather than creeping forever.
    if (target === 0 && vinylSpeed < 0.12) vinylSpeed = 0

    vinylAngle = (vinylAngle + vinylSpeed * dt) % 360
    // Both discs, not just the front one: during a slide the outgoing record
    // is still on screen and must keep turning as it leaves.
    const spin = `rotate(${vinylAngle.toFixed(2)}deg) scale(1.02)`
    discs[0].img.style.transform = spin
    discs[1].img.style.transform = spin
  }

  requestAnimationFrame(frame)
}
requestAnimationFrame(frame)

// --- rendering --------------------------------------------------------------

let currentArt = ''
let anyArtShown = false

/** Slides `incoming` in from the right and the current front out to the left. */
function slideTo(incoming: Side) {
  const enter = discs[incoming]
  const leave = discs[front]

  // Park the incoming disc off to the right with no animation, force the
  // browser to accept that position, then animate from it. Without the reflow
  // both style writes collapse into one frame and nothing moves.
  enter.el.classList.remove('leaving')
  enter.el.style.transition = 'none'
  enter.el.style.transform = 'translateX(100%)'
  enter.el.style.opacity = '0'
  void enter.el.offsetWidth

  enter.el.style.transition = ''
  enter.el.style.transform = 'translateX(0)'
  enter.el.style.opacity = '1'

  if (anyArtShown && leave !== enter) {
    leave.el.classList.add('leaving')
    leave.el.style.transition = ''
    leave.el.style.transform = 'translateX(-100%)'
    leave.el.style.opacity = '0'
    // Release the outgoing image once it is gone, so it is not kept decoded
    // and re-rotated every frame forever.
    window.setTimeout(() => {
      if (front !== incoming) return // another swap already overtook this one
      leave.img.removeAttribute('src')
    }, 700)
  }

  front = incoming
  anyArtShown = true
  ui.discEmpty.hidden = true
}

function setArt(url: string) {
  if (url === currentArt) return
  currentArt = url

  if (!url) {
    for (const d of discs) {
      d.el.classList.remove('leaving')
      d.img.removeAttribute('src')
      d.el.style.transition = 'none'
      d.el.style.transform = 'translateX(100%)'
      d.el.style.opacity = '0'
    }
    anyArtShown = false
    ui.discEmpty.hidden = false
    setBackdrop('')
    return
  }

  // Load into whichever disc is not currently on screen.
  const next: Side = anyArtShown ? (front === 0 ? 1 : 0) : front
  const target = discs[next]

  target.img.onload = () => {
    // A slower request for art we have already moved past must not win.
    if (target.img.src !== url) return
    setBackdrop(url)
    slideTo(next)
  }
  target.img.onerror = () => {
    if (target.img.src !== url) return
    ui.discEmpty.hidden = false
    setBackdrop('')
  }
  target.img.src = url
}

function setBackdrop(url: string) {
  if (url) {
    ui.backdrop.style.backgroundImage = `url("${url}")`
    ui.backdrop.classList.add('on')
  } else {
    ui.backdrop.classList.remove('on')
  }
}

function render(state: State) {
  const p = state.player
  const hasTrack = Boolean(p.valid && p.hasTrack)

  playing = hasTrack && Boolean(p.isPlaying)

  if (hasTrack) {
    setText(ui.track, p.track || '—')
    setText(ui.artist, p.artist || '')
    setArt(p.art || '')
  } else {
    setText(ui.track, 'Nothing playing')
    setText(ui.artist, state.deck.present ? '' : 'Drop a cassette on the deck')
    setArt('')
  }

  // Which speaker Spotify is actually pointed at -- the ESP32 only sends
  // commands, it never plays audio itself, so this is worth being explicit.
  if (p.device) {
    setText(ui.device, `${playing ? 'playing' : 'paused'} on ${p.device}`)
    ui.device.hidden = false
  } else {
    ui.device.hidden = true
  }

  // We reached the device, so its WiFi is necessarily up -- the interesting
  // failure is reachability, which the catch below handles.
  const rssi = state.wifi.rssi
  const detail = [state.wifi.ip, rssi !== undefined ? `${rssi} dBm` : '']
    .filter(Boolean)
    .join('  ·  ')

  // The device answering quickly says nothing about whether it is still talking
  // to Spotify -- those are different tasks. A cache that has stopped advancing
  // means the page is showing fiction, and that must not look healthy.
  const age = p.ageMs ?? 0
  if (p.valid && age > STALE_MS) {
    setConn(
      'stale',
      `stale ${Math.round(age / 1000)}s`,
      `The device is reachable but has not refreshed from Spotify in ${Math.round(age / 1000)}s.\n${detail}`,
    )
  } else {
    setConn('ok', 'connected', detail)
  }

  // The pairing form is the only control left, and only while it is needed.
  const needsPairing = state.deck.present && !state.deck.paired
  if (needsPairing && ui.pairForm.hidden) {
    ui.pairForm.hidden = false
    ui.pairError.hidden = true
    ui.pairUri.value = ''
  } else if (!needsPairing && !ui.pairForm.hidden) {
    ui.pairForm.hidden = true
  }
  if (needsPairing) ui.pairUid.textContent = state.deck.uid

  // Status line stays out of the way unless something is actually wrong.
  let problem = ''
  if (!state.wifi.connected) problem = 'Device has no WiFi'
  else if (!state.spotify.configured) problem = 'No Spotify token on the device'
  else if (state.spotify.error) problem = state.spotify.error
  setStatus(problem)
}

function setStatus(msg: string) {
  setText(ui.status, msg)
  ui.status.hidden = !msg
}

// --- polling ----------------------------------------------------------------

let inFlight = false

async function tick() {
  if (inFlight || document.hidden) return
  inFlight = true
  try {
    render(await getState())
  } catch (err) {
    playing = false
    setConn('down', 'offline', err instanceof Error ? err.message : '')
    setStatus(err instanceof Error ? `Cannot reach the device — ${err.message}` : 'Offline')
  } finally {
    inFlight = false
  }
}

// --- interactions -----------------------------------------------------------

let idleTimer = 0

function bumpIdle() {
  document.body.classList.remove('idle')
  clearTimeout(idleTimer)
  idleTimer = window.setTimeout(() => document.body.classList.add('idle'), IDLE_HIDE_MS)
}

ui.pairForm.addEventListener('submit', async (e) => {
  e.preventDefault()
  const uri = ui.pairUri.value.trim()
  const uid = ui.pairUid.textContent ?? ''
  if (!uri || !uid) return

  ui.pairBtn.disabled = true
  ui.pairError.hidden = true
  try {
    await pairCard(uid, uri)
    ui.pairUri.value = ''
    void tick()
  } catch (err) {
    ui.pairError.textContent = err instanceof Error ? err.message : String(err)
    ui.pairError.hidden = false
  } finally {
    ui.pairBtn.disabled = false
  }
})

// F or a double-click asks for real fullscreen. Purely to drop browser chrome
// on a display left running next to the deck -- the layout does not change.
async function toggleFullscreen() {
  try {
    if (document.fullscreenElement) await document.exitFullscreen()
    else await document.documentElement.requestFullscreen()
  } catch {
    /* refused (iOS Safari, or no user gesture) -- nothing to do */
  }
}

document.addEventListener('keydown', (e) => {
  if (document.activeElement instanceof HTMLInputElement) return
  if (e.key === 'f' || e.key === 'F') {
    e.preventDefault()
    void toggleFullscreen()
  }
})
ui.stage.addEventListener('dblclick', () => void toggleFullscreen())

document.addEventListener('visibilitychange', () => {
  if (!document.hidden) void tick()
})

for (const evt of ['mousemove', 'touchstart', 'keydown', 'click'] as const) {
  document.addEventListener(evt, bumpIdle, { passive: true })
}

bumpIdle()
void tick()
setInterval(() => void tick(), POLL_MS)
