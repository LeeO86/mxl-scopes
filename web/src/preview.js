// The WebRTC preview (SPECIFICATION §2.5, §9.1): one WHEP connection per page, shared by every view.
// When WebRTC fails the views use HLS where the browser plays it natively, else the PNG snapshot.
// Adapted from mxl-multiviewer web/src/preview.js (Apache-2.0, see NOTICE).
import { reactive } from "vue";
import { live } from "./api.js";

const RETRY_MS = 2000;

export const preview = reactive({
  stream: null,
  state: "idle", // idle, connecting, playing, error, off
  error: "",
});

let users = 0;
let pc = null;
let retry = 0;

/** A preview URL as this page reaches it: a public URL as it is, else the own MediaMTX on the page's host. */
export function previewUrl(kind) {
  const p = live.info?.preview;
  if (!p?.[kind]) return "";
  if (p.public?.[kind]) return p[kind];
  try {
    const url = new URL(p[kind]);
    url.hostname = location.hostname;
    return url.toString();
  } catch {
    return p[kind];
  }
}

function close() {
  if (pc) pc.close();
  pc = null;
  preview.stream = null;
}

function again(reason) {
  close();
  preview.state = "error";
  preview.error = reason || "";
  clearTimeout(retry);
  retry = setTimeout(connect, RETRY_MS);
}

async function connect() {
  clearTimeout(retry);
  if (live.info?.preview?.mode === "off") {
    preview.state = "off";
    return;
  }
  const url = previewUrl("whep");
  if (!url) {
    retry = setTimeout(connect, 1000);
    return;
  }
  close();
  preview.state = "connecting";
  const conn = new RTCPeerConnection();
  pc = conn;
  conn.addTransceiver("video", { direction: "recvonly" });
  conn.ontrack = (ev) => {
    if (pc === conn) preview.stream = ev.streams[0] || new MediaStream([ev.track]);
  };
  conn.onconnectionstatechange = () => {
    if (pc !== conn) return;
    if (conn.connectionState === "connected") {
      preview.state = "playing";
      preview.error = "";
    } else if (conn.connectionState === "failed" || conn.connectionState === "disconnected") {
      again(`WebRTC ${conn.connectionState}`);
    }
  };
  try {
    await conn.setLocalDescription(await conn.createOffer());
    // Send the offer with the candidates (no trickle): wait for gathering, at most 1 s.
    await new Promise((resolve) => {
      if (conn.iceGatheringState === "complete") return resolve();
      const timer = setTimeout(resolve, 1000);
      conn.addEventListener("icegatheringstatechange", () => {
        if (conn.iceGatheringState === "complete") {
          clearTimeout(timer);
          resolve();
        }
      });
    });
    if (pc !== conn) return;
    const response = await fetch(url, { method: "POST", headers: { "Content-Type": "application/sdp" }, body: conn.localDescription.sdp });
    if (!response.ok) throw new Error(`WHEP answered ${response.status}`);
    const answer = await response.text();
    if (pc !== conn) return;
    await conn.setRemoteDescription({ type: "answer", sdp: answer });
  } catch (e) {
    if (pc === conn) again(e.message);
  }
}

/** A view starts using the stream; the first one connects. */
export function usePreview() {
  if (++users === 1) connect();
}

/** A view stops using the stream; the last one closes it. */
export function releasePreview() {
  if (--users > 0) return;
  users = 0;
  clearTimeout(retry);
  close();
  preview.state = "idle";
}

/** True when this browser plays HLS in a <video> by itself (Safari). */
export const nativeHls = () => document.createElement("video").canPlayType("application/vnd.apple.mpegurl") !== "";
