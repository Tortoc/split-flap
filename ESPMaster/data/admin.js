//Used for local development use, mirrors the flag in script.js
const localDevelopment = false;

var unitsData = [];
var pollIntervalId = null;

window.addEventListener('load', function () {
	loadUnits();
});

//Re-runs the I2C scan on the ESP to pick up units connected/fixed after boot, no reboot needed
function rescanUnits() {
	if (localDevelopment) {
		loadUnits();
		return;
	}

	var xhrRequest = new XMLHttpRequest();
	xhrRequest.onreadystatechange = function () {
		if (this.readyState == 4) {
			if (this.status == 200) {
				try {
					var responseObject = JSON.parse(this.responseText);
					showBannerMessage("Rescan abgeschlossen: " + responseObject.unitsAmount + " Unit(s) gefunden.", true);
				} catch (e) {
					showBannerMessage("Rescan-Antwort konnte nicht gelesen werden.");
				}
			} else {
				showBannerMessage("Rescan fehlgeschlagen. Status: " + this.status);
			}
			loadUnits();
		}
	};

	xhrRequest.onerror = function () {
		showBannerMessage("Netzwerkfehler beim Rescan.");
		loadUnits();
	};

	xhrRequest.open("POST", "/admin/rescan-units", true);
	xhrRequest.send();
}

//Fetches live status + offset for every unit from the ESP
function loadUnits() {
	if (localDevelopment) {
		unitsData = {
			units: [
				{ index: 0, online: true, status: 0, offset: 103 },
				{ index: 1, online: true, status: 0, offset: 97 },
				{ index: 2, online: false, status: -2, offset: -1 },
			]
		};
		renderUnits(unitsData);
		showContent();
		return;
	}

	var xhrRequest = new XMLHttpRequest();
	xhrRequest.onreadystatechange = function () {
		if (this.readyState == 4) {
			if (this.status == 200) {
				try {
					var responseObject = JSON.parse(this.responseText);
					renderUnits(responseObject);
				} catch (e) {
					showBannerMessage("Failed to read unit data: " + e.message);
				}
			} else {
				showBannerMessage("Failed to load unit data. Status: " + this.status);
			}
			showContent();
		}
	};

	xhrRequest.onerror = function () {
		showBannerMessage("Network error while loading unit data.");
		showContent();
	};

	xhrRequest.open("GET", "/admin/units", true);
	xhrRequest.send();
}

//Renders the list of units and their current offset/status
function renderUnits(responseObject) {
	var container = document.getElementById("containerUnits");
	container.innerHTML = "";

	var units = responseObject.units || [];
	for (var i = 0; i < units.length; i++) {
		var unit = units[i];

		var row = document.createElement("div");
		row.className = "unit-row";
		row.id = "unitRow" + unit.index;

		var labelElement = document.createElement("div");
		labelElement.className = "unit-label";
		labelElement.innerText = "Unit " + unit.index;

		var statusElement = document.createElement("div");
		statusElement.className = "unit-status";
		statusElement.innerHTML = formatStatus(unit);

		var offsetElement = document.createElement("div");
		offsetElement.className = "unit-offset";
		offsetElement.id = "unitOffset" + unit.index;
		offsetElement.innerText = unit.online ? ("Offset: " + unit.offset) : "Offset: -";

		var inputContainer = document.createElement("div");
		inputContainer.className = "unit-input";

		var input = document.createElement("input");
		input.type = "number";
		input.min = "0";
		input.max = "2037";
		input.id = "unitInput" + unit.index;
		input.value = unit.online ? unit.offset : "";
		input.disabled = !unit.online;

		var button = document.createElement("button");
		button.id = "unitButton" + unit.index;
		button.innerText = "Save & Recalibrate";
		button.disabled = !unit.online;
		button.onclick = (function (unitIndex) {
			return function () { saveOffset(unitIndex); };
		})(unit.index);

		inputContainer.appendChild(input);
		inputContainer.appendChild(button);

		row.appendChild(labelElement);
		row.appendChild(statusElement);
		row.appendChild(offsetElement);
		row.appendChild(inputContainer);

		container.appendChild(row);
	}

	if (units.length === 0) {
		container.innerHTML = '<div style="padding: 10px;">No units configured.</div>';
	}

	populateUnitFirmwareTargetDropdown(units);
}

//Keeps the "Ziel-Unit" dropdown on the firmware update card in sync with the known units,
//preserving the current selection across refreshes where possible
function populateUnitFirmwareTargetDropdown(units) {
	var select = document.getElementById("unitFirmwareTarget");
	if (!select) {
		return;
	}

	var previousValue = select.value;
	select.innerHTML = "";

	for (var i = 0; i < units.length; i++) {
		var option = document.createElement("option");
		option.value = units[i].index;
		option.innerText = "Unit " + units[i].index;
		select.appendChild(option);
	}

	if (previousValue !== "" && select.querySelector('option[value="' + previousValue + '"]')) {
		select.value = previousValue;
	}
}

//Turns the raw online/status fields into a readable, colour coded label
function formatStatus(unit) {
	if (!unit.online) {
		return '<span class="status-offline">Offline</span>';
	}

	if (unit.status === 1) {
		return '<span class="status-online-busy">Busy / Recalibrating</span>';
	}

	if (unit.status === 0) {
		return '<span class="status-online-ready">Ready</span>';
	}

	return '<span class="status-offline">Unknown (' + unit.status + ')</span>';
}

//Sends a new offset for a single unit, then polls until it has finished recalibrating
function saveOffset(unitIndex) {
	var input = document.getElementById("unitInput" + unitIndex);
	var button = document.getElementById("unitButton" + unitIndex);
	var offsetValue = parseInt(input.value, 10);

	// 2037 = UNIT_STEPS - 1 in ESPMaster.ino (28BYJ-48 stepper steps per revolution). Keep in sync.
	if (isNaN(offsetValue) || offsetValue < 0 || offsetValue > 2037) {
		alert("Please enter an offset between 0 and 2037.");
		return;
	}

	button.disabled = true;
	button.innerText = "Saving...";

	if (localDevelopment) {
		setTimeout(function () {
			button.disabled = false;
			button.innerText = "Save & Recalibrate";
			showBannerMessage("Unit " + unitIndex + " offset updated to " + offsetValue + " (simulated).", true);
		}, 1500);
		return;
	}

	var xhrRequest = new XMLHttpRequest();
	xhrRequest.onreadystatechange = function () {
		if (this.readyState == 4) {
			if (this.status == 202) {
				showBannerMessage("Unit " + unitIndex + " is recalibrating with the new offset, this can take a few seconds...");
				pollUntilReady(unitIndex, button);
			} else {
				button.disabled = false;
				button.innerText = "Save & Recalibrate";
				showBannerMessage("Failed to set offset: " + (this.responseText || ("Status " + this.status)));
			}
		}
	};

	xhrRequest.onerror = function () {
		button.disabled = false;
		button.innerText = "Save & Recalibrate";
		showBannerMessage("Network error while setting offset.");
	};

	xhrRequest.open("POST", "/admin/unit-offset", true);
	xhrRequest.setRequestHeader("Content-Type", "application/x-www-form-urlencoded");
	xhrRequest.send("id=" + encodeURIComponent(unitIndex) + "&offset=" + encodeURIComponent(offsetValue));
}

//Polls /admin/units until the given unit is done moving (or a timeout is hit), then refreshes the view
function pollUntilReady(unitIndex, button) {
	var attempts = 0;
	var maxAttempts = 20; // ~20 seconds, a full rotation shouldn't take longer than this

	var intervalId = setInterval(function () {
		attempts++;

		var xhrRequest = new XMLHttpRequest();
		xhrRequest.onreadystatechange = function () {
			if (this.readyState == 4 && this.status == 200) {
				try {
					var responseObject = JSON.parse(this.responseText);
					var units = responseObject.units || [];
					var unit = units[unitIndex];

					if (unit && unit.online && unit.status !== 1) {
						clearInterval(intervalId);
						renderUnits(responseObject);
						showBannerMessage("Unit " + unitIndex + " is ready. Current offset: " + unit.offset, true);
						return;
					}
				} catch (e) {
					// ignore and try again on next poll
				}
			}

			if (attempts >= maxAttempts) {
				clearInterval(intervalId);
				button.disabled = false;
				button.innerText = "Save & Recalibrate";
				showBannerMessage("Timed out waiting for Unit " + unitIndex + " to finish recalibrating. Hit Refresh to check its status.");
			}
		};

		xhrRequest.open("GET", "/admin/units", true);
		xhrRequest.send();
	}, 1000);
}

//Shows a message up top of the page
function showBannerMessage(message, hideAfterDuration) {
	var bannerMessageElement = document.getElementById('bannerMessage');
	bannerMessageElement.innerHTML = message;
	bannerMessageElement.classList.remove("hidden");

	if (hideAfterDuration) {
		setTimeout(function () {
			bannerMessageElement.classList.add("hidden");
		}, 7500);
	}
}

function showContent() {
	document.getElementById("initialLoading").classList.add("hidden");
	document.getElementById("loadedContent").classList.remove("hidden");
}

//Helper to escape HTML, mirrors the one in script.js (admin.js is a standalone page)
function escapeHtml(text) {
	var map = {
		'&': '&amp;',
		'<': '&lt;',
		'>': '&gt;',
		'"': '&quot;',
		"'": '&#039;'
	};
	return String(text).replace(/[&<>"']/g, function (m) { return map[m]; });
}

//Uploads a compiled .bin sketch to the ESP over the browser, no Arduino IDE required
function uploadFirmware() {
	var fileInput = document.getElementById("firmwareFile");
	var passwordInput = document.getElementById("firmwarePassword");
	var button = document.getElementById("firmwareUploadButton");
	var progressContainer = document.getElementById("firmwareProgressContainer");
	var progressBar = document.getElementById("firmwareProgressBar");
	var statusEl = document.getElementById("firmwareStatus");

	if (!fileInput.files || fileInput.files.length === 0) {
		alert("Bitte zuerst eine .bin Datei auswählen.");
		return;
	}

	var file = fileInput.files[0];
	if (!file.name.toLowerCase().endsWith(".bin")) {
		if (!confirm("Die ausgewählte Datei endet nicht auf .bin. Trotzdem hochladen?")) {
			return;
		}
	}

	if (!confirm("Firmware '" + file.name + "' jetzt auf das ESPMaster-Modul hochladen? Das Gerät startet danach neu.")) {
		return;
	}

	if (localDevelopment) {
		simulateFirmwareUpload();
		return;
	}

	var formData = new FormData();
	formData.append("firmware", file, file.name);

	button.disabled = true;
	progressContainer.classList.remove("hidden");
	progressBar.style.width = "0%";
	progressBar.innerText = "0%";
	statusEl.innerText = "Wird hochgeladen...";
	statusEl.className = "";

	var xhr = new XMLHttpRequest();

	xhr.upload.onprogress = function (e) {
		if (e.lengthComputable) {
			var percent = Math.round((e.loaded / e.total) * 100);
			progressBar.style.width = percent + "%";
			progressBar.innerText = percent + "%";
		}
	};

	xhr.onreadystatechange = function () {
		if (this.readyState == 4) {
			if (this.status == 200) {
				progressBar.style.width = "100%";
				progressBar.innerText = "100%";
				statusEl.innerText = "Update erfolgreich! Gerät startet jetzt neu, dies kann 10-20 Sekunden dauern...";
				statusEl.className = "status-online-ready";
				setTimeout(function () {
					window.location.href = "/";
				}, 15000);
			} else {
				statusEl.innerText = "Update fehlgeschlagen: " + (this.responseText || ("Status " + this.status));
				statusEl.className = "status-offline";
				button.disabled = false;
			}
		}
	};

	xhr.onerror = function () {
		statusEl.innerText = "Netzwerkfehler beim Hochladen.";
		statusEl.className = "status-offline";
		button.disabled = false;
	};

	xhr.open("POST", "/firmware-update", true);
	xhr.setRequestHeader("X-OTA-Password", passwordInput.value);
	xhr.send(formData);
}

//Uploads a compiled Unit.ino .bin to the ESP, to be pushed to a unit afterwards
function uploadUnitFirmware() {
	var fileInput = document.getElementById("unitFirmwareFile");
	var button = document.getElementById("unitFirmwareUploadButton");
	var statusEl = document.getElementById("unitFirmwareUploadStatus");

	if (!fileInput.files || fileInput.files.length === 0) {
		alert("Bitte zuerst eine .bin Datei auswählen.");
		return;
	}

	var file = fileInput.files[0];

	if (localDevelopment) {
		statusEl.innerText = "Firmware '" + file.name + "' hochgeladen (simuliert).";
		statusEl.className = "status-online-ready";
		return;
	}

	var formData = new FormData();
	formData.append("firmware", file, file.name);

	button.disabled = true;
	statusEl.innerText = "Wird hochgeladen...";
	statusEl.className = "";

	var xhr = new XMLHttpRequest();
	xhr.onreadystatechange = function () {
		if (this.readyState == 4) {
			button.disabled = false;
			if (this.status == 200) {
				statusEl.innerText = "Firmware '" + file.name + "' hochgeladen und bereit zum Übertragen.";
				statusEl.className = "status-online-ready";
			} else {
				statusEl.innerText = "Upload fehlgeschlagen: " + (this.responseText || ("Status " + this.status));
				statusEl.className = "status-offline";
			}
		}
	};

	xhr.onerror = function () {
		button.disabled = false;
		statusEl.innerText = "Netzwerkfehler beim Hochladen.";
		statusEl.className = "status-offline";
	};

	xhr.open("POST", "/unit-firmware-upload", true);
	xhr.send(formData);
}

//Pushes the previously uploaded firmware to the selected unit over I2C
function pushUnitFirmware() {
	var select = document.getElementById("unitFirmwareTarget");
	var button = document.getElementById("unitFirmwarePushButton");
	var resultEl = document.getElementById("unitFirmwareResult");

	if (!select.value) {
		alert("Keine Ziel-Unit verfügbar.");
		return;
	}

	if (!confirm("Firmware jetzt auf Unit " + select.value + " übertragen? Die Unit macht dabei eine kurze Zwangspause und startet danach neu.")) {
		return;
	}

	if (localDevelopment) {
		resultEl.innerHTML = '<p class="status-online-ready">Update erfolgreich (simuliert). 42 von 42 Seiten geschrieben und verifiziert.</p>';
		return;
	}

	button.disabled = true;
	resultEl.innerHTML = '<p>Update läuft, bitte warten - das dauert nur wenige Sekunden...</p>';

	var xhr = new XMLHttpRequest();
	xhr.onreadystatechange = function () {
		if (this.readyState == 4) {
			button.disabled = false;

			try {
				var responseObject = JSON.parse(this.responseText);
				renderUnitFirmwareResult(responseObject);
			} catch (e) {
				resultEl.innerHTML = '<p class="status-offline">Antwort konnte nicht gelesen werden: ' + escapeHtml(this.responseText || ("Status " + this.status)) + '</p>';
			}

			loadUnits();
		}
	};

	xhr.onerror = function () {
		button.disabled = false;
		resultEl.innerHTML = '<p class="status-offline">Netzwerkfehler während des Updates.</p>';
	};

	xhr.open("POST", "/unit-firmware-push", true);
	xhr.setRequestHeader("Content-Type", "application/x-www-form-urlencoded");
	xhr.send("id=" + encodeURIComponent(select.value));
}

//Renders the JSON result of a firmware push in the result panel
function renderUnitFirmwareResult(result) {
	var resultEl = document.getElementById("unitFirmwareResult");
	var cssClass = result.success ? "status-online-ready" : "status-offline";
	var text = result.success ? (result.message || "Update erfolgreich.") : (result.error || "Update fehlgeschlagen.");

	var html = '<p class="' + cssClass + '">' + escapeHtml(text) + '</p>';
	if (typeof result.pagesWritten !== "undefined" && typeof result.totalPages !== "undefined") {
		html += '<p>Seiten geschrieben/verifiziert: ' + result.pagesWritten + ' von ' + result.totalPages + '</p>';
	}

	resultEl.innerHTML = html;
}

//Fakes a firmware upload for local development mode, mirrors localDevelopment handling elsewhere
function simulateFirmwareUpload() {
	var button = document.getElementById("firmwareUploadButton");
	var progressContainer = document.getElementById("firmwareProgressContainer");
	var progressBar = document.getElementById("firmwareProgressBar");
	var statusEl = document.getElementById("firmwareStatus");

	button.disabled = true;
	progressContainer.classList.remove("hidden");
	statusEl.innerText = "Wird hochgeladen (simuliert)...";
	statusEl.className = "";

	var percent = 0;
	var interval = setInterval(function () {
		percent += 20;
		progressBar.style.width = percent + "%";
		progressBar.innerText = percent + "%";

		if (percent >= 100) {
			clearInterval(interval);
			statusEl.innerText = "Update erfolgreich (simuliert).";
			statusEl.className = "status-online-ready";
			button.disabled = false;
		}
	}, 300);
}
