// api.js — thin wrapper around the C backend's REST API.
// All routing happens on the server; this file only sends requests.

const BASE = "/api/v1";

export class ApiError extends Error {
  constructor(status, code, message, field) {
    super(message);
    this.status = status;
    this.code = code;
    this.field = field;
  }
}

async function request(method, path, body, signal) {
  let res;
  try {
    res = await fetch(BASE + path, {
      method,
      headers: body ? { "Content-Type": "application/json" } : undefined,
      body: body ? JSON.stringify(body) : undefined,
      signal,
    });
  } catch (e) {
    if (e.name === "AbortError") throw e;
    throw new ApiError(0, "NETWORK", "Can't reach the routing server. Check that ira_server is running.");
  }
  let data = null;
  try { data = await res.json(); } catch { /* non-JSON */ }
  if (!res.ok || !data || data.status === "error") {
    const err = data && data.error ? data.error : {};
    let message = err.message || `Request failed (HTTP ${res.status})`;
    if (res.status === 429) {
      const wait = res.headers.get("Retry-After");
      message = `Too many requests. Try again in ${wait || "a few"} seconds.`;
    }
    throw new ApiError(res.status, err.code || "HTTP_" + res.status, message, err.field);
  }
  return data;
}

export const api = {
  config: () => request("GET", "/config"),
  health: () => request("GET", "/health"),
  search: (q, signal) => request("GET", `/locations/search?q=${encodeURIComponent(q)}&limit=8`, null, signal),
  snap: (lat, lon) => request("GET", `/locations/snap?lat=${lat}&lon=${lon}`),
  route: (body) => request("POST", "/routes", body),
  trafficSegments: (bbox, signal) => request("GET", `/traffic/segments?bbox=${bbox.join(",")}`, null, signal),
  incidents: (signal) => request("GET", "/incidents", null, signal),
  reportIncident: (body) => request("POST", "/incidents", body),
};
