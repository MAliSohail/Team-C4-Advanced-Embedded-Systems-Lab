const angles = [0, 60, 120, 180];
let currentInspection = null;

function deviceBadge(name, value) {
  const state = value === true ? "online" : value === false ? "offline" : "unknown";
  const text = value === true ? "ONLINE" : value === false ? "OFFLINE" : "UNKNOWN";
  return `<span class="device ${state}">${name}: ${text}</span>`;
}

function renderCards(inspection) {
  const grid = document.getElementById("image-grid");
  grid.innerHTML = angles.map((angle, index) => {
    const viewNumber = index + 1;
    const view = inspection?.views?.find(item => item.view === viewNumber);
    const decision = view?.classification?.label || "NOT REVIEWED";
    const image = view
      ? `<img src="/api/inspection/${inspection.inspection_id}/image/${viewNumber}" alt="View ${viewNumber}">`
      : "Waiting for image";
    const reviewReady = ["WAITING_FOR_REVIEW", "REVIEW_IN_PROGRESS"].includes(inspection?.state);
    return `
      <article class="view-card">
        <div class="view-head"><h3>View ${viewNumber}</h3><span>${angle}°</span></div>
        <div class="image-box">${image}</div>
        <div class="controls">
          <button class="pass" ${reviewReady ? "" : "disabled"} onclick="classifyView(${viewNumber}, 'PASS')">PASS</button>
          <button class="reject" ${reviewReady ? "" : "disabled"} onclick="classifyView(${viewNumber}, 'REJECT')">REJECT</button>
        </div>
        <div class="decision ${decision}">${decision}</div>
      </article>`;
  }).join("");
}

async function refresh() {
  try {
    const response = await fetch("/api/status", { cache: "no-store" });
    const data = await response.json();
    currentInspection = data.inspection;

    document.getElementById("devices").innerHTML =
      deviceBadge("PI", data.devices.pi) +
      deviceBadge("ARDUINO", data.devices.arduino) +
      deviceBadge("MQTT", data.devices.mqtt) +
      deviceBadge("CAMERA", data.devices.camera);

    document.getElementById("state").textContent = currentInspection?.state || "IDLE";
    document.getElementById("message").textContent = currentInspection?.message || "Place an item and press the Arduino button.";
    document.getElementById("inspection-id").textContent = currentInspection?.inspection_id || "No active inspection";
    document.getElementById("progress").textContent = `${currentInspection?.completed_views || 0} / 4 views`;
    document.getElementById("start-button").disabled = data.busy;

    const result = currentInspection?.final_result || "—";
    const resultBox = document.getElementById("final-result");
    resultBox.textContent = result;
    resultBox.className = result;
    renderCards(currentInspection);
  } catch (error) {
    document.getElementById("state").textContent = "PI OFFLINE";
    document.getElementById("message").textContent = String(error);
  }
}

async function startInspection() {
  const response = await fetch("/api/inspection/start", {
    method: "POST",
    headers: { "X-Event-Source": "dashboard" }
  });
  if (!response.ok) alert(JSON.stringify(await response.json()));
  refresh();
}

async function classifyView(view, label) {
  if (!currentInspection) return;
  const response = await fetch(`/api/inspection/${currentInspection.inspection_id}/classify/${view}`, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ label })
  });
  if (!response.ok) alert(JSON.stringify(await response.json()));
  refresh();
}

renderCards(null);
refresh();
setInterval(refresh, 1000);
