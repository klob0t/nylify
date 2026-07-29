import { defineConfig, loadEnv } from 'vite'

// The page runs here on your PC; the data lives on the ESP32. Proxying /api
// through the dev server keeps everything same-origin, so there is no CORS
// preflight in the hot path (the firmware sends CORS headers anyway, which is
// what lets you point a phone straight at the device).
export default defineConfig(({ mode }) => {
  const env = loadEnv(mode, process.cwd(), '')

  // mDNS resolution of .local from Node is unreliable on some setups. If the
  // dev server logs ECONNREFUSED or EAI_AGAIN, put the device's IP in web/.env:
  //   VITE_DEVICE_HOST=192.168.1.50
  const host = env.VITE_DEVICE_HOST || 'cassette.local'

  return {
    server: {
      // Listen on the LAN too, so you can open the UI on a phone next to the
      // deck rather than only on the machine running Vite.
      host: true,
      proxy: {
        '/api': {
          target: `http://${host}`,
          changeOrigin: true,
          timeout: 8000,
        },
      },
    },
  }
})
